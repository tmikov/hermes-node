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

#include <map>
#include <set>
#include <string>
#include <vector>

using namespace hermes::node_compat;
using hermes::node_compat::test::TempTree;
using hermes::node_compat::test::writeFile;

namespace {

/// Builds the fixture tree used by every resolution test in this file:
///
///   <tmp>/app/cli.js
///   <tmp>/app/lib/util.js
///   <tmp>/app/lib/index.js
///   <tmp>/app/data.json
///   <tmp>/app/node_modules/dep/package.json   {"main":"main.js"}
///   <tmp>/app/node_modules/dep/main.js
///   <tmp>/app/node_modules/noMain/index.js
class BundleResolveTest : public ::testing::Test {
 protected:
  void SetUp() override {
    appDir_ = dir_.path() + "/app";
    writeFile(appDir_ + "/cli.js", "require('./lib');\n");
    writeFile(appDir_ + "/lib/util.js", "module.exports = {};\n");
    writeFile(appDir_ + "/lib/index.js", "module.exports = {};\n");
    writeFile(appDir_ + "/data.json", "{}\n");
    writeFile(
        appDir_ + "/node_modules/dep/package.json", "{\"main\":\"main.js\"}");
    writeFile(appDir_ + "/node_modules/dep/main.js", "module.exports = {};\n");
    writeFile(
        appDir_ + "/node_modules/noMain/index.js", "module.exports = {};\n");
  }

  std::string cliJs() const {
    return appDir_ + "/cli.js";
  }

  TempTree dir_;
  std::string appDir_;
};

} // namespace

TEST_F(BundleResolveTest, ResolvesRelativeFileWithMissingExtension) {
  auto result = resolveSpecifier(cliJs(), "./lib/util");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(appDir_ + "/lib/util.js", *result);
}

TEST_F(BundleResolveTest, ResolvesRelativeDirectoryToIndex) {
  auto result = resolveSpecifier(cliJs(), "./lib");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(appDir_ + "/lib/index.js", *result);
}

TEST_F(BundleResolveTest, ResolvesRelativeExactPath) {
  auto result = resolveSpecifier(cliJs(), "./data.json");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(appDir_ + "/data.json", *result);
}

TEST_F(BundleResolveTest, ResolvesBareSpecifierViaPackageMain) {
  auto result = resolveSpecifier(cliJs(), "dep");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(appDir_ + "/node_modules/dep/main.js", *result);
}

TEST_F(BundleResolveTest, ResolvesBareSpecifierWithNoMainToIndex) {
  auto result = resolveSpecifier(cliJs(), "noMain");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(appDir_ + "/node_modules/noMain/index.js", *result);
}

TEST_F(BundleResolveTest, MissingSpecifierReturnsNullopt) {
  EXPECT_FALSE(resolveSpecifier(cliJs(), "missing").has_value());
}

TEST_F(BundleResolveTest, ResolvesFromNestedFileUpThroughNodeModules) {
  // Requiring "dep" from lib/util.js must walk up past app/lib to find
  // app/node_modules, not just look in app/lib/node_modules.
  auto result = resolveSpecifier(appDir_ + "/lib/util.js", "dep");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(appDir_ + "/node_modules/dep/main.js", *result);
}

TEST_F(BundleResolveTest, PackageMainSkipsNestedMainInSubobject) {
  // A "browser" block with its own nested "main" sorts before the real,
  // top-level "main" in this fixture -- exactly the shape that trips up a
  // scanner with no brace-depth tracking.
  writeFile(
      appDir_ + "/pkgs/browserMain/package.json",
      "{\"browser\": {\"main\": \"wrong.js\"}, \"main\": \"right.js\"}");
  writeFile(appDir_ + "/pkgs/browserMain/wrong.js", "module.exports = 0;\n");
  writeFile(appDir_ + "/pkgs/browserMain/right.js", "module.exports = 0;\n");

  auto result = resolveSpecifier(cliJs(), "./pkgs/browserMain");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(appDir_ + "/pkgs/browserMain/right.js", *result);
}

TEST_F(BundleResolveTest, PackageMainIgnoresNestedMainWithNoTopLevelMain) {
  // Only a nested "main" (inside "exports") exists; with no top-level
  // "main", resolution must fall through to index.js rather than picking
  // the nested value up by mistake.
  writeFile(
      appDir_ + "/pkgs/nestedOnly/package.json",
      "{\"exports\": {\"node\": {\"main\": \"nested.js\"}}}");
  writeFile(appDir_ + "/pkgs/nestedOnly/nested.js", "module.exports = 0;\n");
  writeFile(appDir_ + "/pkgs/nestedOnly/index.js", "module.exports = 0;\n");

  auto result = resolveSpecifier(cliJs(), "./pkgs/nestedOnly");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(appDir_ + "/pkgs/nestedOnly/index.js", *result);
}

TEST_F(BundleResolveTest, PackageMainFoundAfterMaintainersKey) {
  // "maintainers" is a real package.json key that shares a prefix with
  // "main"; it must not confuse the exact string-equality check.
  writeFile(
      appDir_ + "/pkgs/maintainersFirst/package.json",
      "{\"maintainers\": [\"a\", \"b\"], \"main\": \"right2.js\"}");
  writeFile(
      appDir_ + "/pkgs/maintainersFirst/right2.js", "module.exports = 0;\n");

  auto result = resolveSpecifier(cliJs(), "./pkgs/maintainersFirst");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(appDir_ + "/pkgs/maintainersFirst/right2.js", *result);
}

TEST_F(BundleResolveTest, PackageMainDepthUnaffectedByBraceInStringValue) {
  // The '{' and '[' inside the "description" string must not be counted as
  // real nesting, or the top-level "main" that follows would be missed.
  writeFile(
      appDir_ + "/pkgs/braceInString/package.json",
      "{\"description\": \"uses {braces} and [brackets]\", "
      "\"main\": \"right3.js\"}");
  writeFile(appDir_ + "/pkgs/braceInString/right3.js", "module.exports = 0;\n");

  auto result = resolveSpecifier(cliJs(), "./pkgs/braceInString");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(appDir_ + "/pkgs/braceInString/right3.js", *result);
}

TEST_F(BundleResolveTest, PackageMainDecodesEscapedQuoteInValue) {
  // Literal package.json bytes: {"main": "a\"b.js"} -- the escaped quote
  // must decode to a literal '"' in the value, not end the string early.
  writeFile(
      appDir_ + "/pkgs/escapedQuote/package.json", "{\"main\": \"a\\\"b.js\"}");
  writeFile(appDir_ + "/pkgs/escapedQuote/a\"b.js", "module.exports = 0;\n");

  auto result = resolveSpecifier(cliJs(), "./pkgs/escapedQuote");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(appDir_ + "/pkgs/escapedQuote/a\"b.js", *result);
}

TEST_F(BundleResolveTest, PackageMainMissingColonFallsThroughToIndex) {
  // Malformed: a "main" key with no colon before its value. Must be
  // treated as absent, not as a parse error that aborts resolution.
  writeFile(
      appDir_ + "/pkgs/missingColon/package.json",
      "{\"main\" \"shouldNotResolve.js\"}");
  writeFile(
      appDir_ + "/pkgs/missingColon/shouldNotResolve.js",
      "module.exports = 0;\n");
  writeFile(appDir_ + "/pkgs/missingColon/index.js", "module.exports = 0;\n");

  auto result = resolveSpecifier(cliJs(), "./pkgs/missingColon");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(appDir_ + "/pkgs/missingColon/index.js", *result);
}

TEST_F(BundleResolveTest, PackageMainNotConfusedByEarlierValueTextMain) {
  // Exact regression repro: a top-level *value* string that is literally
  // the text "main" (not a key -- "description"'s value), appearing
  // before the real "main" key. A scanner that checks string tokens by
  // text equality without confirming key position would hit this value,
  // see no ':' after it, and (if it also aborts on a false match) return
  // absent even though a well-formed top-level "main" follows.
  writeFile(
      appDir_ + "/pkgs/valueTextMain/package.json",
      "{\"description\": \"main\", \"main\": \"right.js\"}");
  writeFile(appDir_ + "/pkgs/valueTextMain/right.js", "module.exports = 0;\n");
  writeFile(appDir_ + "/pkgs/valueTextMain/index.js", "module.exports = 0;\n");

  auto result = resolveSpecifier(cliJs(), "./pkgs/valueTextMain");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(appDir_ + "/pkgs/valueTextMain/right.js", *result);
}

TEST_F(
    BundleResolveTest,
    PackageMainFallsThroughToIndexWhenOnlyValueTextIsMain) {
  // Same false-match text ("description": "main"), but with no real
  // top-level "main" key anywhere in the file. Must still fall through to
  // index.js cleanly rather than erroring the whole resolution out.
  writeFile(
      appDir_ + "/pkgs/valueTextMainOnly/package.json",
      "{\"description\": \"main\"}");
  writeFile(
      appDir_ + "/pkgs/valueTextMainOnly/index.js", "module.exports = 0;\n");

  auto result = resolveSpecifier(cliJs(), "./pkgs/valueTextMainOnly");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(appDir_ + "/pkgs/valueTextMainOnly/index.js", *result);
}

TEST_F(BundleResolveTest, DotSpecifierResolvesToTheOwnDirectoryIndex) {
  // "." is relative in Node (Module._resolveLookupPaths), naming the
  // requiring file's own directory. Classifying it as bare instead sends it
  // into the node_modules walk, where nothing resolves and the build fails
  // outright on code that runs fine from disk.
  auto result = resolveSpecifier(appDir_ + "/lib/util.js", ".");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(appDir_ + "/lib/index.js", *result);
}

TEST_F(BundleResolveTest, DotDotSpecifierResolvesToTheParentDirectory) {
  // ".." is relative too, and this is the dangerous half: as a bare
  // specifier it probes "<dir>/node_modules/..", which normalizes straight
  // back to "<dir>" -- so it would resolve to lib/index.js, the requiring
  // file's OWN directory, instead of the parent's index.js, with no
  // diagnostic at all.
  writeFile(appDir_ + "/index.js", "module.exports = 'parent';\n");
  auto result = resolveSpecifier(appDir_ + "/lib/util.js", "..");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(appDir_ + "/index.js", *result);
}

TEST_F(BundleResolveTest, AbsoluteSpecifierResolvesDirectly) {
  // An absolute request names its target outright -- see the comment on the
  // new branch in resolveSpecifier() for why this must be explicit rather
  // than an accident of the bare-specifier node_modules walk.
  writeFile(appDir_ + "/deep/thing.js", "module.exports = 0;\n");
  std::string target = appDir_ + "/deep/thing.js";
  auto result = resolveSpecifier(cliJs(), target);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(target, *result);
}

TEST_F(BundleResolveTest, AbsoluteSpecifierDoesNotWalkNodeModules) {
  // A file that exists ONLY under node_modules must not be found by an
  // absolute request naming a path where it does not exist. The bare walk
  // below would have produced <dir>/node_modules + this absolute path,
  // which fs::path::operator/ collapses to the absolute path itself -- the
  // right answer by accident. Pin the intent directly, so a future
  // joinNormalized() cannot silently change it.
  writeFile(appDir_ + "/node_modules/ghost/index.js", "module.exports = 0;\n");
  auto result = resolveSpecifier(cliJs(), appDir_ + "/ghost");
  EXPECT_FALSE(result.has_value());
}

TEST_F(BundleResolveTest, AbsoluteSpecifierAgreesAcrossBackends) {
  writeFile(appDir_ + "/deep/thing.js", "module.exports = 0;\n");
  std::string target = appDir_ + "/deep/thing.js";
  DiskFileSource disk;
  auto onDisk = resolveSpecifier(disk, cliJs(), target);
  ASSERT_TRUE(onDisk.has_value());

  // Same question against a container holding the same two identities,
  // rooted at the same directory. Built the way the other agreement cases
  // in BundleFileSourceTest.cpp do.
  BundleWriter writer;
  uint32_t entry =
      writer.addModule("cli.js", ModuleKind::kJavaScript, kRequirable, "x");
  writer.addModule("deep/thing.js", ModuleKind::kJavaScript, kRequirable, "x");
  writer.setEntry(entry);
  std::vector<uint8_t> bytes = writer.serialize(bundleGenerationTag());
  std::string error;
  auto reader = BundleReader::open(
      bytes.data(), bytes.size(), bundleGenerationTag(), &error);
  ASSERT_TRUE(reader.has_value()) << error;
  BundleFileSource container(*reader, appDir_);

  auto inBundle = resolveSpecifier(container, cliJs(), target);
  ASSERT_TRUE(inBundle.has_value());
  EXPECT_EQ(*inBundle, *onDisk);
}

TEST_F(BundleResolveTest, WalkNeverProbesNodeModulesInsideNodeModules) {
  // Module._nodeModulePaths skips any directory that is already named
  // "node_modules", so Node never looks in
  // "<app>/node_modules/node_modules". A package placed there is not
  // reachable under Node and must not be reachable here either.
  writeFile(
      appDir_ + "/node_modules/node_modules/ghost/index.js",
      "module.exports = 0;\n");
  EXPECT_FALSE(resolveSpecifier(appDir_ + "/node_modules/dep/main.js", "ghost")
                   .has_value());
}

// A backend that answers from an in-memory set rather than the disk is the
// whole point of the seam: if resolveSpecifier still reaches the real
// filesystem anywhere, this resolves against files that do not exist there
// and fails.
namespace {
class FakeFileSource : public FileSource {
 public:
  std::set<std::string> files;
  std::map<std::string, std::string> packageJson;

  bool isRegularFile(const std::string &path) const override {
    return files.count(path) != 0;
  }
  bool isDirectory(const std::string &path) const override {
    std::string prefix = path + "/";
    for (const std::string &f : files)
      if (f.compare(0, prefix.size(), prefix) == 0)
        return true;
    return false;
  }
  std::optional<std::string> readPackageJson(const std::string &dir) override {
    auto it = packageJson.find(dir);
    if (it == packageJson.end())
      return std::nullopt;
    return it->second;
  }
};
} // namespace

TEST(BundleResolveFileSourceTest, ResolvesThroughAnInjectedFileSource) {
  FakeFileSource fs;
  fs.files.insert("/app/cli.js");
  fs.files.insert("/app/node_modules/dep/main.js");
  fs.packageJson["/app/node_modules/dep"] = "{\"main\": \"main.js\"}";

  EXPECT_EQ(
      resolveSpecifier(fs, "/app/cli.js", "dep"),
      std::optional<std::string>("/app/node_modules/dep/main.js"));
  EXPECT_FALSE(resolveSpecifier(fs, "/app/cli.js", "ghost").has_value());
}

TEST(IsBuiltinSpecifierTest, RecognizesBareAndNodeSchemeBuiltins) {
  EXPECT_TRUE(isBuiltinSpecifier("fs"));
  EXPECT_TRUE(isBuiltinSpecifier("node:fs"));
}

TEST(IsBuiltinSpecifierTest, RejectsNonBuiltin) {
  EXPECT_FALSE(isBuiltinSpecifier("dep"));
  EXPECT_FALSE(isBuiltinSpecifier("node:dep"));
}

TEST(CommonAncestorTest, SharedPrefixOfTwoPaths) {
  EXPECT_EQ("/a/b", commonAncestor({"/a/b/c.js", "/a/b/d/e.js"}));
}

TEST(CommonAncestorTest, SingleInputReturnsItsDirectory) {
  EXPECT_EQ("/a/b", commonAncestor({"/a/b/c.js"}));
}

TEST(CommonAncestorTest, NoSharedPrefixReturnsRoot) {
  EXPECT_EQ("/", commonAncestor({"/a/x.js", "/b/y.js"}));
}

TEST(CommonAncestorTest, SegmentCompareNotStringPrefixCompare) {
  // "/a/b" and "/a/bc" share the string prefix "/a/b", but "b" and "bc" are
  // different path segments -- the shared ancestor is only "/a". A
  // substring-based implementation would wrongly return "/a/b".
  EXPECT_EQ("/a", commonAncestor({"/a/b/x.js", "/a/bc/y.js"}));
}

TEST(CommonAncestorTest, TrailingSlashOnInputPath) {
  EXPECT_EQ("/a/b", commonAncestor({"/a/b/", "/a/b/c.js"}));
}
