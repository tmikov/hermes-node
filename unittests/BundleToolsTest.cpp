/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <hermes/node-compat/bundle/bundle_tools.h>

#include <hermes/node-compat/bundle/bundle_format.h>
#include <hermes/node-compat/bundle/bundle_generation.h>
#include <hermes/node-compat/bundle/bundle_writer.h>
#include <hermes/node-compat/bundle/native_digest.h>

#include "TempTree.h"

#include <gtest/gtest.h>

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

using namespace hermes::node_compat;
using hermes::node_compat::test::TempTree;
using hermes::node_compat::test::writeFile;

namespace {

/// The tag stamped into the fixture containers.
constexpr uint32_t kGen = 0xABCD1234;
/// What a differently-built binary would require. Deliberately unequal to
/// kGen, so the dump has a mismatch to report.
constexpr uint32_t kOtherGen = 0x0BADF00D;

/// An entry module requiring one JavaScript module and one JSON module.
/// The edges are added in the reverse of the order the table stores them
/// (specifier bytes: './cfg.json' sorts before './lib/util'), so a dump
/// that printed insertion order rather than table order would be visible.
std::vector<uint8_t> makeBundle() {
  BundleWriter writer;
  uint32_t cli = writer.addModule(
      "cli.js", ModuleKind::kJavaScript, kRequirable, "BC-CLI!!!!!!");
  uint32_t util = writer.addModule(
      "lib/util.js", ModuleKind::kJavaScript, kRequirable, "BC-UTIL");
  uint32_t cfg = writer.addModule(
      "cfg.json", ModuleKind::kJSON, kRequirable, "{ \"v\": 2 }");
  writer.addEdge(cli, "./lib/util", util);
  writer.addEdge(cli, "./cfg.json", cfg);
  writer.setEntry(cli);
  return writer.serialize(kGen);
}

std::string readFile(const std::string &path) {
  std::ifstream f(path, std::ios::binary);
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

/// The entries directly inside \p dirPath, excluding "." and "..". Used to
/// prove a failed extraction left no temp file behind: writeFileAtomically
/// names its temp file after outPath, so a leftover would show up here as
/// a sibling of whatever outPath itself was.
std::vector<std::string> listDir(const std::string &dirPath) {
  std::vector<std::string> names;
  DIR *d = ::opendir(dirPath.c_str());
  if (d == nullptr)
    return names;
  while (dirent *entry = ::readdir(d)) {
    std::string name = entry->d_name;
    if (name != "." && name != "..")
      names.push_back(name);
  }
  ::closedir(d);
  return names;
}

/// The offset of \p needle in \p text, or std::string::npos.
size_t find(const std::string &text, const std::string &needle) {
  return text.find(needle);
}

bool contains(const std::string &text, const std::string &needle) {
  return text.find(needle) != std::string::npos;
}

TEST(BundleToolsTest, DumpPrintsHeaderTablesAndTotals) {
  TempTree dir;
  std::vector<uint8_t> bytes = makeBundle();
  std::string path = dir.path() + "/app.hbb";
  writeFile(path, bytes);

  std::ostringstream out;
  std::ostringstream err;
  EXPECT_EQ(dumpBundle(path, kGen, /*verbose*/ false, out, err), 0);
  EXPECT_EQ(err.str(), "");
  const std::string text = out.str();

  EXPECT_TRUE(
      contains(text, "bundle: " + path + "   format v5  generation 0xabcd1234"))
      << text;
  EXPECT_TRUE(contains(text, "\nentry:  [0] cli.js\n")) << text;

  // A matching generation says nothing about it beyond the header line.
  EXPECT_FALSE(contains(text, "MISMATCH")) << text;

  EXPECT_TRUE(contains(text, "MODULES (3)")) << text;
  EXPECT_TRUE(contains(text, "  idx  kind  bytes  identity\n")) << text;
  // Columns are sized from the widest value present: three modules means a
  // one-digit index under a three-character heading, and the widest kind is
  // "json".
  EXPECT_TRUE(contains(text, "\n    0  js       12  cli.js\n")) << text;
  EXPECT_TRUE(contains(text, "\n    1  js        7  lib/util.js\n")) << text;
  EXPECT_TRUE(contains(text, "\n    2  json     10  cfg.json\n")) << text;

  EXPECT_TRUE(contains(text, "EDGES (2)")) << text;
  EXPECT_TRUE(contains(text, "\n  cli.js  './cfg.json'  -> [2]\n")) << text;
  EXPECT_TRUE(contains(text, "\n  cli.js  './lib/util'  -> [1]\n")) << text;

  // Each size is pinned, and pinned under its own label: a section size
  // reported under the wrong heading is exactly the silent wrong answer
  // this tool exists to prevent, and only the numbers can catch it.
  //
  // strings  65: five interned entries -- three identities (6 + 11 + 8) and
  //              two specifiers (10 + 10) -- each behind a 4-byte length.
  // modules  60: 3 records of sizeof(BundleModuleRecord) == 20.
  // edges    24: 2 records of sizeof(BundleEdgeRecord) == 12.
  // payload  40: 12, 7 and 10 bytes of payload, each padded up to the next
  //              multiple of kBundlePayloadAlign (16 + 8 + 16).
  //
  // The two record sizes the literals above are derived from, checked here
  // so the derivation is not just a comment.
  EXPECT_EQ(sizeof(BundleModuleRecord), 20u);
  EXPECT_EQ(sizeof(BundleEdgeRecord), 12u);
  EXPECT_TRUE(contains(text, "\nSECTIONS\n")) << text;
  EXPECT_TRUE(contains(text, "\n  strings  65 B    modules  60 B\n")) << text;
  EXPECT_TRUE(contains(text, "\n  edges    24 B    payload  40 B\n")) << text;
  // The total is the size of the file, which exceeds the sum of the six
  // sections (this fixture's natives and preloads are both empty, but the
  // rows are unconditional) by the header and the payload alignment
  // padding.
  EXPECT_TRUE(
      contains(text, "\ntotal " + std::to_string(bytes.size()) + " bytes\n"))
      << text;
}

// The edge table is stored sorted by (importer index, specifier bytes),
// because that is the order the runtime binary binary-searches. The dump
// prints it as stored, so a sort bug shows up here and nowhere else.
//
// The fixture is built so that the stored order disagrees with every other
// order the dump could plausibly have used. Module indices run z-entry.js,
// a-lib.js, so:
//   - insertion order       ./a-helper, ./z-dep, ./a-lib
//   - stored order          ./a-lib, ./z-dep, ./a-helper
//   - sorted by specifier   ./a-helper, ./a-lib, ./z-dep
//   - grouped by identity   ./a-helper (a-lib.js), then z-entry.js's two
// No two of those agree, so a dump that sorted or regrouped the rows would
// fail this test rather than pass it by coincidence -- which is what a
// single-importer fixture would do, since one importer makes stored order
// and specifier order the same sequence.
TEST(BundleToolsTest, EdgesPrintInStoredOrder) {
  BundleWriter writer;
  uint32_t entry = writer.addModule(
      "z-entry.js", ModuleKind::kJavaScript, kRequirable, "BC-Z");
  uint32_t lib = writer.addModule(
      "a-lib.js", ModuleKind::kJavaScript, kRequirable, "BC-A");
  uint32_t dep = writer.addModule(
      "z-dep.js", ModuleKind::kJavaScript, kRequirable, "BC-D");
  uint32_t helper = writer.addModule(
      "a-helper.js", ModuleKind::kJavaScript, kRequirable, "BC-H");
  writer.addEdge(lib, "./a-helper", helper);
  writer.addEdge(entry, "./z-dep", dep);
  writer.addEdge(entry, "./a-lib", lib);
  writer.setEntry(entry);

  TempTree dir;
  std::string path = dir.path() + "/order.hbb";
  writeFile(path, writer.serialize(kGen));

  std::ostringstream out;
  std::ostringstream err;
  ASSERT_EQ(dumpBundle(path, kGen, /*verbose*/ false, out, err), 0);
  const std::string text = out.str();

  size_t aLib = find(text, "'./a-lib'");
  size_t zDep = find(text, "'./z-dep'");
  size_t aHelper = find(text, "'./a-helper'");
  ASSERT_NE(aLib, std::string::npos) << text;
  ASSERT_NE(zDep, std::string::npos) << text;
  ASSERT_NE(aHelper, std::string::npos) << text;

  // Importer index dominates: both of module 0's edges come before module
  // 1's, even though './a-helper' sorts before both of them by bytes.
  EXPECT_LT(aLib, zDep) << text;
  EXPECT_LT(zDep, aHelper) << text;
}

TEST(BundleToolsTest, DumpReportsGenerationMismatchAndStillSucceeds) {
  TempTree dir;
  std::string path = dir.path() + "/app.hbb";
  writeFile(path, makeBundle());

  std::ostringstream out;
  std::ostringstream err;
  // A container this binary could not run is exactly the one worth looking
  // at, so the mismatch is reported rather than refused.
  EXPECT_EQ(dumpBundle(path, kOtherGen, /*verbose*/ false, out, err), 0);
  EXPECT_EQ(err.str(), "");
  const std::string text = out.str();

  EXPECT_TRUE(contains(
      text,
      "generation: 0xabcd1234  MISMATCH (this binary requires 0x0badf00d)"))
      << text;
  // Everything else is dumped exactly as it would be for a matching tag.
  EXPECT_TRUE(contains(text, "MODULES (3)")) << text;
  EXPECT_TRUE(contains(text, "EDGES (2)")) << text;
}

TEST(BundleToolsTest, VerboseAddsPerModuleEdgeCounts) {
  TempTree dir;
  std::string path = dir.path() + "/app.hbb";
  writeFile(path, makeBundle());

  std::ostringstream out;
  std::ostringstream err;
  ASSERT_EQ(dumpBundle(path, kGen, /*verbose*/ true, out, err), 0);
  const std::string text = out.str();

  EXPECT_TRUE(contains(text, "  idx  kind  bytes  in  out  identity\n"))
      << text;
  // cli.js imports both others and is imported by neither.
  EXPECT_TRUE(contains(text, "\n    0  js       12   0    2  cli.js\n"))
      << text;
  EXPECT_TRUE(contains(text, "\n    1  js        7   1    0  lib/util.js\n"))
      << text;
  EXPECT_TRUE(contains(text, "\n    2  json     10   1    0  cfg.json\n"))
      << text;
}

// Column widths come from the widest value present, not from a fixed
// guess. A guessed width is what wraps a deep node_modules identity onto a
// second line and costs the table its shape.
TEST(BundleToolsTest, ColumnsWidenToTheWidestValuePresent) {
  const std::string longIdentity =
      "node_modules/@scope/a-package-with-a-long-name/lib/internal/thing.js";

  BundleWriter writer;
  uint32_t cli =
      writer.addModule("cli.js", ModuleKind::kJavaScript, kRequirable, "BC");
  // Six digits, wider than the "bytes" heading.
  uint32_t big = writer.addModule(
      longIdentity,
      ModuleKind::kJavaScript,
      kRequirable,
      std::string(123456, 'x'));
  writer.addEdge(cli, "@scope/a-package-with-a-long-name", big);
  writer.setEntry(cli);

  TempTree dir;
  std::string path = dir.path() + "/wide.hbb";
  writeFile(path, writer.serialize(kGen));

  std::ostringstream out;
  std::ostringstream err;
  ASSERT_EQ(dumpBundle(path, kGen, /*verbose*/ false, out, err), 0);
  const std::string text = out.str();

  // The bytes column is six wide now, so the heading and the small module's
  // size are padded out to meet it.
  EXPECT_TRUE(contains(text, "  idx  kind   bytes  identity\n")) << text;
  EXPECT_TRUE(contains(text, "\n    0  js         2  cli.js\n")) << text;
  EXPECT_TRUE(contains(text, "\n    1  js    123456  " + longIdentity + "\n"))
      << text;
  // The identity is last on its line, so nothing pads or truncates it.
  EXPECT_TRUE(
      contains(text, "  cli.js  '@scope/a-package-with-a-long-name'  -> [1]\n"))
      << text;
}

// A module packaged only so the resolver could read it (flags == 0, no
// kRequirable) is never itself require()d. The dump marks it in the kind
// column, which is what makes a container's growth from such a record
// explained rather than mysterious.
TEST(BundleToolsTest, DumpMarksResolveOnlyModules) {
  BundleWriter writer;
  uint32_t cli =
      writer.addModule("cli.js", ModuleKind::kJavaScript, kRequirable, "BC");
  writer.addModule(
      "node_modules/dep/package.json",
      ModuleKind::kJSON,
      /*flags*/ 0,
      "{}");
  writer.setEntry(cli);

  TempTree dir;
  std::string path = dir.path() + "/resolve-only.hbb";
  writeFile(path, writer.serialize(kGen));

  std::ostringstream out;
  std::ostringstream err;
  ASSERT_EQ(dumpBundle(path, kGen, /*verbose*/ false, out, err), 0);
  const std::string text = out.str();

  // The requirable module's row is unmarked; the resolve-only one's kind
  // column carries the marker, which widens the column for both rows.
  EXPECT_TRUE(contains(text, "  idx  kind               bytes  identity\n"))
      << text;
  EXPECT_TRUE(contains(text, "\n    0  js                     2  cli.js\n"))
      << text;
  EXPECT_TRUE(contains(
      text,
      "\n    1  json resolve-only      2  node_modules/dep/package.json\n"))
      << text;
}

TEST(BundleToolsTest, DumpFailsOnAMissingFile) {
  TempTree dir;
  std::ostringstream out;
  std::ostringstream err;
  EXPECT_NE(dumpBundle(dir.path() + "/nope.hbb", kGen, false, out, err), 0);
  EXPECT_EQ(out.str(), "");
  EXPECT_TRUE(contains(err.str(), "nope.hbb")) << err.str();
}

TEST(BundleToolsTest, DumpFailsOnAFileThatIsNotAContainer) {
  TempTree dir;
  std::string path = dir.path() + "/notabundle";
  // Bigger than sizeof(BundleHeader), so the reader reaches the magic check
  // rather than reporting truncation first.
  writeFile(path, std::vector<uint8_t>(256, 'x'));

  std::ostringstream out;
  std::ostringstream err;
  EXPECT_NE(dumpBundle(path, kGen, false, out, err), 0);
  EXPECT_EQ(out.str(), "");
  // The reader's own diagnosis, not a second vocabulary invented here --
  // but prefixed with the file it is about. The reader never names the
  // file (one reader also serves the run path, where the container is the
  // program), so without the prefix a tool pointed at one of several files
  // would not say which one failed.
  EXPECT_TRUE(contains(err.str(), "bad magic")) << err.str();
  EXPECT_TRUE(contains(err.str(), "error: " + path + ": ")) << err.str();
}

// The same prefix on the extract path: within one binary, a malformed
// container must not name the file or not depending on which verb was
// asked for.
TEST(BundleToolsTest, ExtractNamesTheFileWhenTheContainerIsMalformed) {
  TempTree dir;
  std::string path = dir.path() + "/notabundle";
  // Bigger than sizeof(BundleHeader), so the reader reaches the magic check
  // rather than reporting truncation first.
  writeFile(path, std::vector<uint8_t>(256, 'x'));

  std::ostringstream err;
  EXPECT_NE(extractModule(path, "cli.js", dir.path() + "/out", err), 0);
  EXPECT_TRUE(contains(err.str(), "bad magic")) << err.str();
  EXPECT_TRUE(contains(err.str(), "error: " + path + ": ")) << err.str();
}

// An empty file cannot be mmap'd, so the mapping helper has a special case
// for it. It must still reach the reader and be diagnosed as the truncated
// container it is, rather than as an I/O failure.
TEST(BundleToolsTest, DumpFailsOnAnEmptyFile) {
  TempTree dir;
  std::string path = dir.path() + "/empty.hbb";
  writeFile(path, std::vector<uint8_t>());

  std::ostringstream out;
  std::ostringstream err;
  EXPECT_NE(dumpBundle(path, kGen, false, out, err), 0);
  EXPECT_TRUE(contains(err.str(), "truncated")) << err.str();
  EXPECT_TRUE(contains(err.str(), "error: " + path + ": ")) << err.str();
}

TEST(BundleToolsTest, MappedFileExposesTheWholeFile) {
  TempTree dir;
  std::string path = dir.path() + "/app.hbb";
  std::vector<uint8_t> bytes = makeBundle();
  writeFile(path, bytes);

  std::string error;
  std::optional<MappedFile> file = MappedFile::open(path, &error);
  ASSERT_TRUE(file.has_value()) << error;
  ASSERT_EQ(file->size(), bytes.size());
  EXPECT_EQ(std::memcmp(file->data(), bytes.data(), bytes.size()), 0);
  EXPECT_EQ(std::memcmp(file->data(), kBundleMagic, sizeof(kBundleMagic)), 0);
}

// Payload bytes go out verbatim, so extracting a JavaScript module's
// bytecode payload is a byte-for-byte round trip through the container.
TEST(BundleToolsTest, ExtractRoundTripsJavaScriptPayloadBytes) {
  TempTree dir;
  std::string bundlePath = dir.path() + "/app.hbb";
  writeFile(bundlePath, makeBundle());
  std::string outPath = dir.path() + "/util.hbc";

  std::ostringstream err;
  EXPECT_EQ(extractModule(bundlePath, "lib/util.js", outPath, err), 0);
  EXPECT_EQ(err.str(), "");
  EXPECT_EQ(readFile(outPath), "BC-UTIL");
}

// Same round trip for a JSON module: its payload is the source file's own
// text, so this is what makes an extracted JSON file byte-identical to the
// tree it was built from.
TEST(BundleToolsTest, ExtractRoundTripsJsonPayloadBytes) {
  TempTree dir;
  std::string bundlePath = dir.path() + "/app.hbb";
  writeFile(bundlePath, makeBundle());
  std::string outPath = dir.path() + "/cfg.json";

  std::ostringstream err;
  EXPECT_EQ(extractModule(bundlePath, "cfg.json", outPath, err), 0);
  EXPECT_EQ(err.str(), "");
  EXPECT_EQ(readFile(outPath), "{ \"v\": 2 }");
}

TEST(BundleToolsTest, ExtractFailsOnUnknownIdentityAndWritesNothing) {
  TempTree dir;
  std::string bundlePath = dir.path() + "/app.hbb";
  writeFile(bundlePath, makeBundle());
  std::string outPath = dir.path() + "/x";

  std::ostringstream err;
  EXPECT_NE(extractModule(bundlePath, "lib/nope.js", outPath, err), 0);
  EXPECT_TRUE(contains(err.str(), "lib/nope.js")) << err.str();
  EXPECT_EQ(::access(outPath.c_str(), F_OK), -1);
}

// A close typo is offered a suggestion, drawn from the container's own
// identities rather than some second vocabulary invented here.
TEST(BundleToolsTest, ExtractSuggestsCloseIdentitiesForATypo) {
  TempTree dir;
  std::string bundlePath = dir.path() + "/app.hbb";
  writeFile(bundlePath, makeBundle());
  std::string outPath = dir.path() + "/x";

  std::ostringstream err;
  EXPECT_NE(extractModule(bundlePath, "lib/utl.js", outPath, err), 0);
  EXPECT_TRUE(contains(err.str(), "did you mean")) << err.str();
  EXPECT_TRUE(contains(err.str(), "lib/util.js")) << err.str();
  EXPECT_EQ(::access(outPath.c_str(), F_OK), -1);
}

// A typo unrelated to anything in the container gets no suggestion list:
// the distance budget (a third of the identity's length) rules out every
// candidate here, so padding the list with three irrelevant identities
// would be worse than saying nothing.
TEST(BundleToolsTest, ExtractSuggestsNothingForAWildTypo) {
  TempTree dir;
  std::string bundlePath = dir.path() + "/app.hbb";
  writeFile(bundlePath, makeBundle());
  std::string outPath = dir.path() + "/x";

  std::ostringstream err;
  EXPECT_NE(
      extractModule(bundlePath, "completely-unrelated-name", outPath, err), 0);
  EXPECT_FALSE(contains(err.str(), "did you mean")) << err.str();
}

// "Up to three" needs a fourth (and fifth) candidate within budget to mean
// anything: makeBundle()'s three modules can never produce one, so that
// fixture cannot tell a real cap from no cap at all. Five identities here
// tie at edit distance 1 from the typo, so the cap is what has to break
// the tie, and it breaks it by container (module index) order -- the
// order suggestIdentities() encounters them in, since it is a stable sort.
TEST(BundleToolsTest, ExtractSuggestsAtMostThreeClosestIdentities) {
  BundleWriter writer;
  uint32_t entry = writer.addModule(
      "entry.js", ModuleKind::kJavaScript, kRequirable, "BC-E");
  writer.addModule("mod1.js", ModuleKind::kJavaScript, kRequirable, "BC-1");
  writer.addModule("mod2.js", ModuleKind::kJavaScript, kRequirable, "BC-2");
  writer.addModule("mod3.js", ModuleKind::kJavaScript, kRequirable, "BC-3");
  writer.addModule("mod4.js", ModuleKind::kJavaScript, kRequirable, "BC-4");
  writer.addModule("mod5.js", ModuleKind::kJavaScript, kRequirable, "BC-5");
  writer.setEntry(entry);

  TempTree dir;
  std::string bundlePath = dir.path() + "/five.hbb";
  writeFile(bundlePath, writer.serialize(kGen));
  std::string outPath = dir.path() + "/x";

  std::ostringstream err;
  EXPECT_NE(extractModule(bundlePath, "mod0.js", outPath, err), 0);
  const std::string text = err.str();
  EXPECT_TRUE(contains(text, "did you mean")) << text;
  EXPECT_TRUE(contains(text, "mod1.js")) << text;
  EXPECT_TRUE(contains(text, "mod2.js")) << text;
  EXPECT_TRUE(contains(text, "mod3.js")) << text;
  EXPECT_FALSE(contains(text, "mod4.js")) << text;
  EXPECT_FALSE(contains(text, "mod5.js")) << text;
}

// Extracting onto the container being read replaces it with one module's
// payload -- a fraction of its own bytes -- and nothing downstream notices,
// because the reader is holding the old inode through its mapping. The
// container has to come out of this byte-for-byte unchanged, which is the
// assertion an exit code alone cannot make.
TEST(BundleToolsTest, ExtractRefusesToWriteOntoTheContainer) {
  TempTree dir;
  std::string bundlePath = dir.path() + "/app.hbb";
  std::vector<uint8_t> bytes = makeBundle();
  writeFile(bundlePath, bytes);

  std::ostringstream err;
  EXPECT_NE(extractModule(bundlePath, "lib/util.js", bundlePath, err), 0);
  EXPECT_TRUE(contains(err.str(), "same file as the bundle")) << err.str();

  // Compared as strings rather than through memcmp: when this assertion
  // fails it fails because the file got SHORTER, and a memcmp sized from
  // the expected length would read off the end of the actual one.
  EXPECT_EQ(readFile(bundlePath), std::string(bytes.begin(), bytes.end()));
  // Nothing was written anywhere: no temp file was ever opened either.
  EXPECT_EQ(listDir(dir.path()).size(), 1u);
}

// Sameness is decided by (st_dev, st_ino), not by comparing two strings, so
// a second name for the same inode is refused as well. A symlink is the
// shape a user actually meets -- `app.hbb` in a build tree pointing at the
// real artifact -- and a string comparison would let it straight through.
TEST(BundleToolsTest, ExtractRefusesASecondNameForTheContainer) {
  TempTree dir;
  std::string bundlePath = dir.path() + "/app.hbb";
  std::vector<uint8_t> bytes = makeBundle();
  writeFile(bundlePath, bytes);

  std::string linkPath = dir.path() + "/link.hbb";
  ASSERT_EQ(::symlink(bundlePath.c_str(), linkPath.c_str()), 0);
  std::string hardPath = dir.path() + "/hard.hbb";
  ASSERT_EQ(::link(bundlePath.c_str(), hardPath.c_str()), 0);

  for (const std::string &outPath : {linkPath, hardPath}) {
    std::ostringstream err;
    EXPECT_NE(extractModule(bundlePath, "lib/util.js", outPath, err), 0)
        << outPath;
    EXPECT_TRUE(contains(err.str(), "same file as the bundle")) << err.str();
  }

  // And the "./" spelling of the container's own directory, which is the
  // one-token slip the guard exists for.
  std::ostringstream err;
  EXPECT_NE(
      extractModule(bundlePath, "lib/util.js", dir.path() + "/./app.hbb", err),
      0);
  EXPECT_TRUE(contains(err.str(), "same file as the bundle")) << err.str();

  EXPECT_EQ(readFile(bundlePath).size(), bytes.size());
}

// A different file with a name close to the container's is not the same
// file, so it is written normally. Without this the guard could be a blanket
// refusal to write into the container's directory and every test above would
// still pass.
TEST(BundleToolsTest, ExtractWritesBesideTheContainer) {
  TempTree dir;
  std::string bundlePath = dir.path() + "/app.hbb";
  writeFile(bundlePath, makeBundle());
  std::string outPath = dir.path() + "/app.hbb.util";

  std::ostringstream err;
  EXPECT_EQ(extractModule(bundlePath, "lib/util.js", outPath, err), 0);
  EXPECT_EQ(err.str(), "");
  EXPECT_EQ(readFile(outPath), "BC-UTIL");
}

// The unknown-identity tests above fail before any write is even
// attempted, so by themselves they cannot distinguish a real
// temp-file-then-rename from an implementation that never tried in the
// first place. This forces the failure to happen AFTER the temp file was
// opened and written: rename() onto an existing directory fails with
// EISDIR, so a successful module lookup and a successful write both
// happen before the failure. outPath (the directory) and its parent must
// come out exactly as they went in.
TEST(BundleToolsTest, ExtractLeavesNothingWhenTheFinalRenameFails) {
  TempTree dir;
  std::string bundlePath = dir.path() + "/app.hbb";
  writeFile(bundlePath, makeBundle());

  std::string outPath = dir.path() + "/out";
  ASSERT_EQ(::mkdir(outPath.c_str(), 0755), 0);

  std::ostringstream err;
  EXPECT_NE(extractModule(bundlePath, "cfg.json", outPath, err), 0);
  // Names the failed rename specifically, and the ".tmp" file it tried to
  // rename from -- not just "cannot open outPath", which is the message an
  // implementation that skipped the temp file and wrote straight to
  // outPath would produce instead (open() on an existing directory with
  // O_WRONLY fails with EISDIR immediately, before any write is
  // attempted). That wrong implementation still makes every assertion
  // below hold, which is exactly why this one has to pin the message too.
  EXPECT_TRUE(contains(err.str(), "rename")) << err.str();
  EXPECT_TRUE(contains(err.str(), ".tmp")) << err.str();

  struct stat st {};
  ASSERT_EQ(::stat(outPath.c_str(), &st), 0);
  EXPECT_TRUE(S_ISDIR(st.st_mode));
  EXPECT_TRUE(listDir(outPath).empty());

  // Exactly app.hbb and the still-empty out/ directory: no ".<pid>.<n>.tmp"
  // sibling of outPath was left behind.
  EXPECT_EQ(listDir(dir.path()).size(), 2u);
}

// The three outcomes verifyNatives() reports, in one container: a sidecar
// present and matching, one deleted after the build, and one present but
// with different bytes at the same recorded length. bundleGenerationTag()
// rather than the fixed kGen constant the dump tests use, because
// verifyNatives() opens through openForInspection() exactly like dumpBundle
// and extractModule do -- it does not care what tag the container carries,
// so nothing here should suggest that it does.
TEST(BundleToolsTest, VerifyNativesReportsOkMissingAndMismatch) {
  TempTree tree;
  std::string addon = tree.write("ok.node", "addon-bytes");
  std::string error;
  auto digest = nativeFileDigest(addon, &error);
  ASSERT_TRUE(digest.has_value()) << error;

  BundleWriter writer;
  uint32_t entry =
      writer.addModule("cli.js", ModuleKind::kJavaScript, kRequirable, "x");
  uint32_t okIdx =
      writer.addModule("ok.node", ModuleKind::kNative, kRequirable, "");
  uint32_t goneIdx =
      writer.addModule("gone.node", ModuleKind::kNative, kRequirable, "");
  uint32_t badIdx =
      writer.addModule("bad.node", ModuleKind::kNative, kRequirable, "");
  writer.setEntry(entry);
  writer.addNative(okIdx, "ok.node", digest->byteLength, digest->raw);
  writer.addNative(goneIdx, "gone.node", 7, std::string(32, '\x01'));
  writer.addNative(badIdx, "bad.node", 11, std::string(32, '\x02'));
  std::vector<uint8_t> bytes = writer.serialize(bundleGenerationTag());

  std::string bundlePath = tree.path("app.hbb");
  tree.writeBytes("app.hbb", bytes);
  tree.write("bad.node", "different!!"); // same length, different bytes

  std::ostringstream out, err;
  int rc = verifyNatives(bundlePath, /*verbose=*/false, out, err);
  EXPECT_EQ(rc, 1);
  std::string text = out.str();
  EXPECT_NE(text.find("OK       ok.node"), std::string::npos) << text;
  EXPECT_NE(text.find("MISSING  gone.node"), std::string::npos) << text;
  EXPECT_NE(text.find("MISMATCH bad.node"), std::string::npos) << text;
  EXPECT_NE(err.str().find("2 of 3"), std::string::npos) << err.str();
}

TEST(BundleToolsTest, VerifyNativesSucceedsWithNoNatives) {
  TempTree tree;
  BundleWriter writer;
  uint32_t entry =
      writer.addModule("cli.js", ModuleKind::kJavaScript, kRequirable, "x");
  writer.setEntry(entry);
  tree.writeBytes("app.hbb", writer.serialize(bundleGenerationTag()));

  std::ostringstream out, err;
  EXPECT_EQ(verifyNatives(tree.path("app.hbb"), false, out, err), 0);
}

// The NATIVES section is dumpBundle()'s inventory of what verifyNatives()
// later checks against disk: the module it belongs to, the sidecar it
// expects, and enough of the recorded digest to eyeball. bundleGenerationTag
// rather than kGen, matching the natives fixtures above -- dumpBundle() opens
// through openForInspection() regardless of which tag the container carries,
// so nothing here should suggest that it matters.
TEST(BundleToolsTest, DumpPrintsNativesSection) {
  TempTree tree;
  BundleWriter writer;
  uint32_t entry =
      writer.addModule("cli.js", ModuleKind::kJavaScript, kRequirable, "x");
  uint32_t addon = writer.addModule(
      "node_modules/a/build/Release/a.node",
      ModuleKind::kNative,
      kRequirable,
      "");
  writer.setEntry(entry);
  writer.addNative(addon, "a.node", 4096, std::string(32, '\xab'));
  tree.writeBytes("app.hbb", writer.serialize(bundleGenerationTag()));

  std::ostringstream out, err;
  ASSERT_EQ(
      dumpBundle(tree.path("app.hbb"), bundleGenerationTag(), false, out, err),
      0)
      << err.str();
  std::string text = out.str();
  EXPECT_NE(text.find("NATIVES"), std::string::npos) << text;
  EXPECT_NE(text.find("a.node"), std::string::npos) << text;
  EXPECT_NE(text.find("4096"), std::string::npos) << text;
  EXPECT_NE(text.find("abababab"), std::string::npos) << text;
}

// The preload table has real bytes in the file even though it has no row of
// its own in MODULES or EDGES -- SECTIONS is the only place a dump ever
// says so. A container with at least one preload is what makes preloads
// nonzero and worth pinning; DumpPrintsHeaderTablesAndTotals above already
// covers the all-zero case implicitly (its fixture has none).
TEST(BundleToolsTest, DumpPrintsPreloadsInSections) {
  TempTree tree;
  BundleWriter writer;
  uint32_t entry =
      writer.addModule("cli.js", ModuleKind::kJavaScript, kRequirable, "x");
  uint32_t setup =
      writer.addModule("setup.js", ModuleKind::kJavaScript, kRequirable, "y");
  writer.setEntry(entry);
  writer.addPreload(setup);
  tree.writeBytes("app.hbb", writer.serialize(bundleGenerationTag()));

  std::ostringstream out, err;
  ASSERT_EQ(
      dumpBundle(tree.path("app.hbb"), bundleGenerationTag(), false, out, err),
      0)
      << err.str();
  std::string text = out.str();
  // Found by locating the "natives" row and slicing out its own line,
  // rather than matching a literal string with the section-width padding
  // baked in: that padding depends on the widest value in the whole
  // SECTIONS block, which is incidental to what this test is pinning.
  size_t nativesLine = text.find("\n  natives");
  ASSERT_NE(nativesLine, std::string::npos) << text;
  size_t lineEnd = text.find('\n', nativesLine + 1);
  std::string line = text.substr(nativesLine, lineEnd - nativesLine);
  EXPECT_NE(line.find("preloads"), std::string::npos) << line;
  // sizeof(uint32_t) == 4: one preload index, and nothing else in the
  // table.
  EXPECT_NE(line.find("4 B"), std::string::npos) << line;
}

// extractModule() has nothing to write for a native: its bytes never
// entered the container in the first place (see the sidecar design note
// above extractModule() in bundle_tools.cpp). The error names both the
// module and the sidecar it actually ships as, so the caller knows where to
// look instead of finding a zero-byte file and no explanation.
TEST(BundleToolsTest, ExtractModuleRefusesANative) {
  TempTree tree;
  BundleWriter writer;
  uint32_t entry =
      writer.addModule("cli.js", ModuleKind::kJavaScript, kRequirable, "x");
  uint32_t addon =
      writer.addModule("a.node", ModuleKind::kNative, kRequirable, "");
  writer.setEntry(entry);
  writer.addNative(addon, "a.node", 4096, std::string(32, '\xab'));
  tree.writeBytes("app.hbb", writer.serialize(bundleGenerationTag()));

  std::ostringstream err;
  EXPECT_EQ(
      extractModule(tree.path("app.hbb"), "a.node", tree.path("out.bin"), err),
      1);
  EXPECT_NE(err.str().find("native addon"), std::string::npos) << err.str();
  EXPECT_NE(err.str().find("alongside"), std::string::npos) << err.str();
  EXPECT_FALSE(std::filesystem::exists(tree.path("out.bin")));
}

} // namespace
