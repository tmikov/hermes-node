/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <hermes/node-compat/bundle/bundle_generation.h>
#include <hermes/node-compat/bundle/bundle_resolve.h>
#include <hermes/node-compat/bundle/bundle_writer.h>
#include <hermes/node-compat/bundle/file_source.h>

#include <gtest/gtest.h>

#include <sys/stat.h>

#include <fstream>
#include <optional>
#include <string>
#include <vector>

using namespace hermes::node_compat;

namespace {

/// Creates directory \p path (POSIX mkdir, not the GNU `mkdir -p` shell
/// command) along with any missing parents. Mirrors the helper in
/// unittests/BundleResolveTest.cpp.
void makeDirs(const std::string &path) {
  std::string partial;
  size_t start = 0;
  while (start < path.size()) {
    size_t slash = path.find('/', start + 1);
    partial = slash == std::string::npos ? path : path.substr(0, slash);
    if (!partial.empty())
      ::mkdir(partial.c_str(), 0755); // ignored if it already exists
    if (slash == std::string::npos)
      break;
    start = slash;
  }
}

/// A temporary directory, removed (recursively) on destruction, that also
/// tracks every file written to it so it can build an equivalent bundle
/// container on demand. Mirrors the TempDir helper in
/// unittests/BundleResolveTest.cpp, extended with the container side this
/// test needs.
///
/// The fixture's own tree (and the FileSource root) is nested one level
/// below the mkdtemp directory, at "<mkdtemp>/app" -- exactly the "appDir_"
/// nesting BundleResolveTest uses, and for the same reason: a bare ".."
/// from a top-level file (e.g. "cli.js") normalizes to the mkdtemp
/// directory's parent when the fixture root IS the mkdtemp directory, and
/// that parent is /tmp itself -- world-writable, so any local user or
/// stray tool leaving a package.json or index.js there would flip the disk
/// backend's answer and fail the agreement test on a machine this code
/// never touched. Nesting one level down means that same ".." lands on the
/// mkdtemp directory instead, which mkdtemp creates mode 0700 and this
/// fixture alone controls.
class TempTree {
 public:
  TempTree() {
    char tmpl[] = "/tmp/hnfs-test-XXXXXX";
    const char *made = ::mkdtemp(tmpl);
    EXPECT_NE(nullptr, made);
    tmpDir_ = made ? made : "";
    appDir_ = tmpDir_ + "/app";
  }
  ~TempTree() {
    if (!tmpDir_.empty())
      ::system(("rm -rf " + tmpDir_).c_str());
  }

  /// The absolute path of tree-relative \p rel, under the "app" root (not
  /// the raw mkdtemp directory -- see the class comment).
  std::string abs(const std::string &rel) const {
    return appDir_ + "/" + rel;
  }

  /// Writes \p content to tree-relative \p rel on disk, creating parent
  /// directories first, and records it so buildContainerFileSource() can
  /// package the same file into a container.
  void write(const std::string &rel, const std::string &content) {
    std::string full = abs(rel);
    size_t slash = full.find_last_of('/');
    if (slash != std::string::npos)
      makeDirs(full.substr(0, slash));
    std::ofstream f(full, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(f.is_open()) << full;
    f << content;
    files_.push_back({rel, content});
  }

  /// Packages every file written so far into a container -- package.json
  /// files as kJSON/0 (resolve-only, not require()able), everything else
  /// as kJavaScript/kRequirable -- and returns a BundleFileSource rooted
  /// at this tree's directory. The underlying reader is owned by this
  /// fixture and must outlive the returned source.
  BundleFileSource buildContainerFileSource() {
    BundleWriter w;
    for (const WrittenFile &f : files_) {
      size_t slash = f.rel.find_last_of('/');
      std::string basename =
          slash == std::string::npos ? f.rel : f.rel.substr(slash + 1);
      bool isPackageJson = basename == "package.json";
      uint32_t index = w.addModule(
          f.rel,
          isPackageJson ? ModuleKind::kJSON : ModuleKind::kJavaScript,
          isPackageJson ? 0 : kRequirable,
          f.content);
      // serialize() refuses to emit a container with no entry module (see
      // bundle_writer.cpp); resolveSpecifier() never looks at it, so the
      // first module written is as good as any other.
      if (index == 0)
        w.setEntry(index);
    }
    bytes_ = w.serialize(bundleGenerationTag());
    std::string error;
    reader_ = BundleReader::open(
        bytes_.data(), bytes_.size(), bundleGenerationTag(), &error);
    EXPECT_TRUE(reader_.has_value()) << error;
    return BundleFileSource(*reader_, appDir_);
  }

 private:
  struct WrittenFile {
    std::string rel;
    std::string content;
  };

  std::string tmpDir_;
  std::string appDir_;
  std::vector<WrittenFile> files_;
  std::vector<uint8_t> bytes_;
  std::optional<BundleReader> reader_;
};

} // namespace

TEST(BundleFileSourceTest, AnswersFilesAndDirectories) {
  BundleWriter w;
  w.setEntry(w.addModule("cli.js", ModuleKind::kJavaScript, kRequirable, "A"));
  w.addModule(
      "node_modules/dep/main.js", ModuleKind::kJavaScript, kRequirable, "B");
  w.addModule(
      "node_modules/dep/package.json",
      ModuleKind::kJSON,
      0,
      "{\"main\": \"main.js\"}");
  auto bytes = w.serialize(bundleGenerationTag());
  std::string error;
  auto r = BundleReader::open(
      bytes.data(), bytes.size(), bundleGenerationTag(), &error);
  ASSERT_TRUE(r.has_value()) << error;

  BundleFileSource src(*r, "/app");
  EXPECT_TRUE(src.isRegularFile("/app/cli.js"));
  EXPECT_FALSE(src.isRegularFile("/app/nope.js"));
  EXPECT_TRUE(src.isDirectory("/app/node_modules/dep"));
  // A prefix that is not a whole segment is not a directory: "dep" must not
  // make "depot" one.
  EXPECT_FALSE(src.isDirectory("/app/node_modules/de"));
  // Outside the root there is nothing at all.
  EXPECT_FALSE(src.isRegularFile("/etc/passwd"));
  EXPECT_FALSE(src.isDirectory("/etc"));
  EXPECT_EQ(
      src.readPackageJson("/app/node_modules/dep"),
      std::optional<std::string>("{\"main\": \"main.js\"}"));

  // Containment is never loosened, including the cases that look closest
  // to escaping. stripRoot() does no ".." interpretation and no symlink
  // resolution -- it is a literal "root_ + '/'" byte prefix check -- so
  // reasoning through each case in turn:
  //
  // "/app/../etc/passwd" DOES start with the literal bytes "/app/", so
  // stripRoot() succeeds and hands back the relative string
  // "../etc/passwd". That string is then looked up as an ordinary
  // identity (isRegularFile) or identity-prefix (isDirectory) -- and no
  // identity in this container is ever built with a ".." segment (the
  // producer only ever emits already-resolved, lexically clean tree-
  // relative paths), so the lookup simply misses. The false answer here
  // holds by construction, not because ".." was recognized and rejected.
  EXPECT_FALSE(src.isRegularFile("/app/../etc/passwd"));
  EXPECT_FALSE(src.isDirectory("/app/../etc/passwd"));
  // Same reasoning, two ".." segments: relative string "a/../../etc/passwd"
  // matches no identity and no identity prefix.
  EXPECT_FALSE(src.isRegularFile("/app/a/../../etc/passwd"));
  EXPECT_FALSE(src.isDirectory("/app/a/../../etc/passwd"));
  // "/apple/cli.js": "/app" is a plain string prefix of "/apple", but the
  // prefix stripRoot() actually requires is "/app/" (root_ plus the
  // separator) -- "/apple/cli.js"'s 5th character is 'l', not '/', so the
  // prefix check fails outright and the path is not under the root at
  // all. This is the segment-awareness guarantee applied to the root
  // itself, not just to interior directory segments.
  EXPECT_FALSE(src.isRegularFile("/apple/cli.js"));
  // A relative path with no root prefix at all can never match: stripRoot()
  // requires the literal "/app/" prefix, and "cli.js" (despite being a
  // real identity) does not begin with it -- its own first 5 bytes are
  // "cli.j", not "/app/". Resolution always hands this class fully joined,
  // root-anchored paths, never a bare identity, so this pins that a bare
  // identity string is not accidentally treated as one.
  EXPECT_FALSE(src.isRegularFile("cli.js"));
  // The empty string: comparing its (empty) first 5 bytes against "/app/"
  // is a length mismatch, so the prefix check fails and stripRoot() misses.
  EXPECT_FALSE(src.isRegularFile(""));
  EXPECT_FALSE(src.isDirectory(""));
}

TEST(BundleFileSourceTest, RootItselfIsADirectory) {
  BundleWriter w;
  w.setEntry(w.addModule("cli.js", ModuleKind::kJavaScript, kRequirable, "A"));
  auto bytes = w.serialize(bundleGenerationTag());
  std::string error;
  auto r = BundleReader::open(
      bytes.data(), bytes.size(), bundleGenerationTag(), &error);
  ASSERT_TRUE(r.has_value()) << error;

  BundleFileSource src(*r, "/app");
  EXPECT_TRUE(src.isDirectory("/app"));
  EXPECT_FALSE(src.isRegularFile("/app"));
}

// Root "/" is reachable in practice: bundle_run.cpp derives the FileSource
// root from a bundle file's parent directory (fs::path::parent_path()),
// and a bundle installed at the filesystem root -- e.g. a Docker
// `COPY app.hbb /` -- makes that parent exactly "/". Naively appending a
// separator to build the strip prefix would turn it into "//", which
// matches no real path and makes every query answer false silently (a
// closed-world failure, not an escape, but a total and quiet one). Pin
// that root "/" behaves like any other root: files resolve, the segment
// boundary at the root is still respected, and the root itself is still a
// directory.
TEST(BundleFileSourceTest, RootIsTheFilesystemRoot) {
  BundleWriter w;
  w.setEntry(w.addModule("cli.js", ModuleKind::kJavaScript, kRequirable, "A"));
  w.addModule(
      "node_modules/dep/main.js", ModuleKind::kJavaScript, kRequirable, "B");
  auto bytes = w.serialize(bundleGenerationTag());
  std::string error;
  auto r = BundleReader::open(
      bytes.data(), bytes.size(), bundleGenerationTag(), &error);
  ASSERT_TRUE(r.has_value()) << error;

  BundleFileSource src(*r, "/");
  EXPECT_TRUE(src.isDirectory("/"));
  EXPECT_TRUE(src.isRegularFile("/cli.js"));
  EXPECT_TRUE(src.isDirectory("/node_modules/dep"));
  // Still segment-aware at the root: "cli" is a string prefix of "cli.js"
  // but must not itself resolve to that file.
  EXPECT_FALSE(src.isRegularFile("/cli"));
  EXPECT_FALSE(src.isRegularFile("/nope.js"));
}

// Every specifier resolved twice: once against a real tree on disk, once
// against a container built from that same tree. A resolver change that
// affects only one backend fails here, which is the failure the shared
// algorithm exists to prevent.
TEST(BundleFileSourceTest, AgreesWithTheDiskBackend) {
  TempTree tree;
  tree.write("cli.js", "require('dep'); require('./lib/util');");
  tree.write("lib/util.js", "module.exports = 1;");
  tree.write("node_modules/dep/package.json", "{\"main\": \"lib/entry.js\"}");
  tree.write("node_modules/dep/lib/entry.js", "module.exports = 2;");
  tree.write("node_modules/noMain/index.js", "module.exports = 3;");

  BundleFileSource bundleSrc = tree.buildContainerFileSource();
  DiskFileSource diskSrc;

  const char *froms[] = {
      "cli.js", "lib/util.js", "node_modules/dep/lib/entry.js"};
  const char *specs[] = {
      "dep",
      "noMain",
      "./lib/util",
      "./util",
      "..",
      ".",
      "missing",
      "dep/lib/entry.js"};
  for (const char *from : froms) {
    for (const char *spec : specs) {
      auto onDisk = resolveSpecifier(diskSrc, tree.abs(from), spec);
      auto inBundle = resolveSpecifier(bundleSrc, tree.abs(from), spec);
      EXPECT_EQ(onDisk, inBundle) << "from " << from << " require " << spec;
    }
  }
}
