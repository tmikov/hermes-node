/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <hermes/node-compat/bundle/bundle_generation.h>
#include <hermes/node-compat/bundle/bundle_reader.h>
#include <hermes/node-compat/bundle/bundle_resolve.h>
#include <hermes/node-compat/bundle/bundle_writer.h>
#include <hermes/node-compat/bundle/file_source.h>

#include "TempTree.h"

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <vector>

using namespace hermes::node_compat;

namespace {

/// A container-building tree on top of test::TempTree: it also tracks every
/// file written to it (test::TempTree itself only manages the directory's
/// lifetime) so it can build an equivalent bundle container on demand.
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
class ContainerTree {
 public:
  ContainerTree() : appDir_(tree_.path() + "/app") {}

  /// The absolute path of tree-relative \p rel, under the "app" root (not
  /// the raw mkdtemp directory -- see the class comment).
  std::string abs(const std::string &rel) const {
    return appDir_ + "/" + rel;
  }

  /// Writes \p content to tree-relative \p rel on disk, creating parent
  /// directories first, and records it so buildContainerFileSource() can
  /// package the same file into a container.
  void write(const std::string &rel, const std::string &content) {
    hermes::node_compat::test::writeFile(abs(rel), content);
    files_.push_back({rel, content});
  }

  /// Packages every file written so far into a container -- package.json
  /// files as kJSON/0 (resolve-only, not require()able), everything else
  /// as kJavaScript/kRequirable -- and returns a BundleFileSource rooted
  /// at this tree's directory, or nullopt if BundleReader::open() failed
  /// (reported via a gtest failure, so the caller does not also need to
  /// check \p error). The underlying reader is owned by this fixture and
  /// must outlive the returned source.
  std::optional<BundleFileSource> buildContainerFileSource() {
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
    if (!reader_.has_value())
      return std::nullopt; // fail cleanly: no reader to build a source over.
    return BundleFileSource(*reader_, appDir_);
  }

  /// Like buildContainerFileSource(), but packages only the subset of
  /// written files classifyFile() (lib/bundle/bundle_build.cpp) would keep
  /// for a real container: no extension, ".js", ".cjs" or ".ts" as
  /// JavaScript, ".json" as JSON, everything else -- ".node", ".mjs", any
  /// other extension -- left out entirely. A real container is never a
  /// mirror of the tree it came from, and a divergence that only shows up
  /// when the container is MISSING something the disk has is invisible to
  /// a test whose container always has everything.
  std::optional<BundleFileSource> buildSubsetContainerFileSource() {
    BundleWriter w;
    for (const WrittenFile &f : files_) {
      size_t slash = f.rel.find_last_of('/');
      std::string basename =
          slash == std::string::npos ? f.rel : f.rel.substr(slash + 1);
      size_t dot = basename.find_last_of('.');
      std::string ext = dot == std::string::npos ? "" : basename.substr(dot);
      bool isPackageJson = basename == "package.json";
      bool isJs = ext.empty() || ext == ".js" || ext == ".cjs" || ext == ".ts";
      bool isJson = ext == ".json";
      if (!isJs && !isJson)
        continue; // classifyFile()'s kSkip: .node, .mjs, anything else.
      uint32_t index = w.addModule(
          f.rel,
          isJson ? ModuleKind::kJSON : ModuleKind::kJavaScript,
          isPackageJson ? 0 : kRequirable,
          f.content);
      if (index == 0)
        w.setEntry(index);
    }
    bytes_ = w.serialize(bundleGenerationTag());
    std::string error;
    reader_ = BundleReader::open(
        bytes_.data(), bytes_.size(), bundleGenerationTag(), &error);
    EXPECT_TRUE(reader_.has_value()) << error;
    if (!reader_.has_value())
      return std::nullopt;
    return BundleFileSource(*reader_, appDir_);
  }

 private:
  struct WrittenFile {
    std::string rel;
    std::string content;
  };

  hermes::node_compat::test::TempTree tree_;
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
  // to escaping. identityFor() does no ".." interpretation and no symlink
  // resolution -- it is a literal "root_ + '/'" byte prefix check -- so
  // reasoning through each case in turn:
  //
  // "/app/../etc/passwd" DOES start with the literal bytes "/app/", so
  // identityFor() succeeds and hands back the relative string
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
  // prefix identityFor() actually requires is "/app/" (root_ plus the
  // separator) -- "/apple/cli.js"'s 5th character is 'l', not '/', so the
  // prefix check fails outright and the path is not under the root at
  // all. This is the segment-awareness guarantee applied to the root
  // itself, not just to interior directory segments.
  EXPECT_FALSE(src.isRegularFile("/apple/cli.js"));
  // A relative path with no root prefix at all can never match: identityFor()
  // requires the literal "/app/" prefix, and "cli.js" (despite being a
  // real identity) does not begin with it -- its own first 5 bytes are
  // "cli.j", not "/app/". Resolution always hands this class fully joined,
  // root-anchored paths, never a bare identity, so this pins that a bare
  // identity string is not accidentally treated as one.
  EXPECT_FALSE(src.isRegularFile("cli.js"));
  // The empty string: comparing its (empty) first 5 bytes against "/app/"
  // is a length mismatch, so the prefix check fails and identityFor() misses.
  EXPECT_FALSE(src.isRegularFile(""));
  EXPECT_FALSE(src.isDirectory(""));
}

// identityFor() is the primitive isRegularFile/isDirectory/readPackageJson
// are all built on; the other tests in this file exercise it only through
// those. Pin its own return values directly, including the empty-view
// (root itself) and nullopt (not under root) cases.
TEST(BundleFileSourceTest, IdentityForDirectly) {
  BundleWriter w;
  w.setEntry(w.addModule("cli.js", ModuleKind::kJavaScript, kRequirable, "A"));
  w.addModule(
      "node_modules/dep/main.js", ModuleKind::kJavaScript, kRequirable, "B");
  auto bytes = w.serialize(bundleGenerationTag());
  std::string error;
  auto r = BundleReader::open(
      bytes.data(), bytes.size(), bundleGenerationTag(), &error);
  ASSERT_TRUE(r.has_value()) << error;

  BundleFileSource src(*r, "/app");
  EXPECT_EQ(
      src.identityFor("/app/cli.js"),
      std::optional<std::string_view>("cli.js"));
  EXPECT_EQ(
      src.identityFor("/app/node_modules/dep/main.js"),
      std::optional<std::string_view>("node_modules/dep/main.js"));
  // The root itself: an empty, non-nullopt view.
  EXPECT_EQ(src.identityFor("/app"), std::optional<std::string_view>(""));
  // Not lexically under root: a string prefix ("/app" of "/apple") is not
  // enough -- the next byte must be the separator.
  EXPECT_EQ(src.identityFor("/apple/cli.js"), std::nullopt);
  EXPECT_EQ(src.identityFor("/other/cli.js"), std::nullopt);
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
  ContainerTree tree;
  tree.write("cli.js", "require('dep'); require('./lib/util');");
  tree.write("lib/util.js", "module.exports = 1;");
  tree.write("node_modules/dep/package.json", "{\"main\": \"lib/entry.js\"}");
  tree.write("node_modules/dep/lib/entry.js", "module.exports = 2;");
  tree.write("node_modules/noMain/index.js", "module.exports = 3;");
  // A file beside a directory of the same name: "./lib" has to resolve
  // through the ".js" extension probe (lib.js) without either backend ever
  // reaching the isDirectory() branch for "lib" itself, and the two must
  // agree about that even when the near-miss ("lib.js" sorts immediately
  // next to every "lib/..." identity, differing only in '.' vs '/') is
  // sitting right there to be matched by mistake.
  tree.write("lib.js", "module.exports = 10;");
  // lib/'s own index.js, so a trailing-slash directory specifier ("./lib/"
  // below) actually resolves to something through the isDirectory() branch
  // instead of both backends agreeing on nullopt regardless of whether that
  // branch works: with nothing resolvable inside lib/, a backend that
  // dropped the trailing slash trim entirely would still land on nullopt
  // like the correct one, and the agreement would hold by accident.
  tree.write("lib/index.js", "module.exports = 15;");
  // An identity that is a string prefix of another: "util.js" beside
  // "utils.js" pins that neither backend's exact-match lookup (the disk's
  // stat(), the bundle's index()/find()) mistakes a query for one as a hit
  // on the other.
  tree.write("lib/utils.js", "module.exports = 11;");
  // ".ts" and ".json" extension probes: exercise the two extensions after
  // ".js" in resolveBase()'s candidate list, over both backends.
  tree.write("types.ts", "module.exports = 12;");
  tree.write("data.json", "{\"v\": 13}");
  // A nested node_modules, reached by climbing from deep inside another
  // package's own tree (node_modules/dep/lib/entry.js) past dep's own,
  // nonexistent, nested node_modules/node_modules and landing on this one
  // instead -- a multi-segment identity for both backends' directory and
  // regular-file lookups to agree about.
  tree.write(
      "node_modules/dep/node_modules/inner/index.js", "module.exports = 14;");

  std::optional<BundleFileSource> bundleSrcOpt =
      tree.buildContainerFileSource();
  ASSERT_TRUE(bundleSrcOpt.has_value());
  BundleFileSource &bundleSrc = *bundleSrcOpt;
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
      "dep/lib/entry.js",
      "./lib",
      "./lib/",
      "dep/",
      "./types",
      "./data",
      "inner",
      "./lib/utils"};
  for (const char *from : froms) {
    for (const char *spec : specs) {
      auto onDisk = resolveSpecifier(diskSrc, tree.abs(from), spec);
      auto inBundle = resolveSpecifier(bundleSrc, tree.abs(from), spec);
      EXPECT_EQ(onDisk, inBundle) << "from " << from << " require " << spec;
    }
  }

  // The loop above only checks that the two backends AGREE -- and both
  // returning nullopt satisfies that as easily as both returning a real
  // hit. Every spec but "missing" is meant to resolve from at least one of
  // the three froms (which one varies -- e.g. "./util" only resolves from
  // lib/util.js, ".." only from node_modules/dep/lib/entry.js, by walking
  // back up into node_modules/dep's own package.json main). Check that
  // directly, so a fixture edit that quietly stopped a spec from resolving
  // anywhere could not decay that entry of the matrix into two nullopts
  // agreeing with each other about nothing.
  for (const char *spec : specs) {
    if (std::string_view(spec) == "missing")
      continue; // meant to miss everywhere -- checked explicitly below.
    bool resolvesSomewhere = false;
    for (const char *from : froms) {
      if (resolveSpecifier(diskSrc, tree.abs(from), spec).has_value()) {
        resolvesSomewhere = true;
        break;
      }
    }
    EXPECT_TRUE(resolvesSomewhere) << "spec " << spec << " never resolves";
  }

  // "missing" is meant to miss from every from. Asserted directly rather
  // than only through the agreement loop above, for the same reason as the
  // positive check just above it.
  for (const char *from : froms) {
    EXPECT_EQ(
        resolveSpecifier(diskSrc, tree.abs(from), "missing"), std::nullopt);
    EXPECT_EQ(
        resolveSpecifier(bundleSrc, tree.abs(from), "missing"), std::nullopt);
  }

  // An absolute specifier OUTSIDE the root. joinNormalized() collapses to
  // the specifier itself the same way it does for the inside-root case
  // below, but nothing under either backend's root answers for it: the
  // path names neither a real file (it is not written anywhere by this
  // fixture) nor a bundled identity (BundleFileSource's containment never
  // lets an identity escape the tree it indexes). Asserted directly, not
  // only through agreement: two nullopts satisfy EXPECT_EQ as easily as
  // two real hits, so a resolver that started answering yes for an
  // out-of-root absolute path on both backends would slip past an
  // equality-only check.
  const char *outsideAbsSpec = "/nonexistent-hermes-node-bundle-test-path/x.js";
  for (const char *from : froms) {
    EXPECT_EQ(
        resolveSpecifier(diskSrc, tree.abs(from), outsideAbsSpec), std::nullopt)
        << "from " << from;
    EXPECT_EQ(
        resolveSpecifier(bundleSrc, tree.abs(from), outsideAbsSpec),
        std::nullopt)
        << "from " << from;
  }

  // An absolute specifier inside the root. std::filesystem's operator/
  // discards its left operand outright when the right one is itself
  // absolute, so joinNormalized() collapses straight to the specifier
  // itself no matter which directory the walk started from -- worth
  // pinning on its own, since it is not a spec any of the froms above
  // would otherwise exercise.
  std::string absSpec = tree.abs("lib/util.js");
  for (const char *from : froms) {
    auto onDisk = resolveSpecifier(diskSrc, tree.abs(from), absSpec);
    auto inBundle = resolveSpecifier(bundleSrc, tree.abs(from), absSpec);
    EXPECT_EQ(onDisk, inBundle) << "from " << from << " require " << absSpec;
  }
}

// AgreesWithTheDiskBackend's container mirrors the tree exactly, but a real
// one never does: classifyFile() (lib/bundle/bundle_build.cpp) skips
// ".node", ".mjs" and any other unrecognized extension. A divergence that
// only shows up when the container is MISSING something the disk has is
// invisible to a test whose container always has everything -- this one
// builds a container that is a strict subset instead, and checks both
// halves: the backends still agree wherever the container kept the file,
// and a skipped file is a clean miss rather than a neighbour the resolver
// picks up by mistake (addon.node sitting right beside the packaged
// addon.js is exactly the shape that would catch a resolver that, failing
// an exact match, fell back to "the closest thing with a known
// extension").
TEST(BundleFileSourceTest, SubsetContainerMissesSkippedFilesCleanly) {
  ContainerTree tree;
  tree.write("cli.js", "require('./addon');");
  tree.write("addon.js", "module.exports = 1;");
  // classifyFile() skips these: a native addon, ESM, and an arbitrary
  // unrecognized extension, all sitting right beside a packaged sibling
  // that shares its stem.
  tree.write("addon.node", "not really a native module, just bytes");
  tree.write("lib/thing.js", "module.exports = 2;");
  tree.write("lib/thing.node", "not really a native module, just bytes");
  tree.write("esm.mjs", "export default 1;");
  tree.write("readme.txt", "hello");

  std::optional<BundleFileSource> subsetOpt =
      tree.buildSubsetContainerFileSource();
  ASSERT_TRUE(subsetOpt.has_value());
  BundleFileSource &subset = *subsetOpt;
  DiskFileSource diskSrc;
  std::string from = tree.abs("cli.js");

  // Wherever the subset container HAS the file, the two backends still
  // agree -- the same property AgreesWithTheDiskBackend pins, now over a
  // container that is a strict subset of the tree rather than a mirror of
  // it.
  const char *kept[] = {"./addon", "./addon.js", "./lib/thing"};
  for (const char *spec : kept) {
    auto onDisk = resolveSpecifier(diskSrc, from, spec);
    auto inSubset = resolveSpecifier(subset, from, spec);
    EXPECT_EQ(onDisk, inSubset) << "require " << spec;
    // A sanity check that this is a real hit and not two nullopts
    // agreeing by accident.
    EXPECT_TRUE(onDisk.has_value()) << "require " << spec;
  }

  // A file classifyFile() would skip is a clean miss in the container --
  // never silently redirected to a same-stem neighbour that WAS packaged.
  const char *skipped[] = {
      "./addon.node", "./lib/thing.node", "./esm.mjs", "./readme.txt"};
  for (const char *spec : skipped) {
    EXPECT_EQ(resolveSpecifier(subset, from, spec), std::nullopt)
        << "require " << spec;
    // And disk agrees these ARE real files: the divergence here is by
    // design (the container legitimately omits them), not a fluke of the
    // fixture that would make the miss trivially true either way.
    EXPECT_TRUE(resolveSpecifier(diskSrc, from, spec).has_value())
        << "require " << spec;
  }
}
