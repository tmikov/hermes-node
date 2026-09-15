/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <hermes/node-compat/build-native/build_native.h>
#include <hermes/node-compat/bundle/cjs_wrapper.h>

#include "TempTree.h"

#include "gtest/gtest.h"

#include <algorithm>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <mutex>
#include <sstream>

using namespace hermes::node_compat;
using hermes::node_compat::test::TempTree;

namespace {

bool has(const std::vector<std::string> &argv, const std::string &flag) {
  return std::find(argv.begin(), argv.end(), flag) != argv.end();
}

/// A fresh directory for compileModules() to stage into, valid for the rest
/// of the process. Not a TempTree: that removes its directory when it goes
/// out of scope, and compileModules() needs the directory to outlive the
/// call that populates it -- unlike TempTree's other uses here, which read
/// back what they wrote before the fixture ends. Left behind under /tmp for
/// the OS to reclaim, same as TempTree's own mkdtemp does structurally.
std::string makeTempDir() {
  char tmpl[] = "/tmp/hntt-bn-XXXXXX";
  const char *made = ::mkdtemp(tmpl);
  return made ? made : "";
}

/// Index of \p flag, or -1.
int at(const std::vector<std::string> &argv, const std::string &flag) {
  auto it = std::find(argv.begin(), argv.end(), flag);
  return it == argv.end() ? -1 : (int)(it - argv.begin());
}

TEST(BuildNativeTest, UnitNamesArePaddedAndValid) {
  EXPECT_EQ("hn_m000000", nativeUnitName(0));
  EXPECT_EQ("hn_m000017", nativeUnitName(17));
  EXPECT_EQ("hn_m001483", nativeUnitName(1483));
  // isValidSHUnitName permits alphanumerics and underscore only.
  for (char c : nativeUnitName(42))
    EXPECT_TRUE(isalnum((unsigned char)c) || c == '_') << c;
  // Past six digits it must still be unique, not truncated.
  EXPECT_EQ("hn_m1234567", nativeUnitName(1234567));
}

/// Every row of the parity table the spec fixes, asserted on the argv,
/// because the alternative -- comparing program output -- cannot prove a
/// flag was passed, only that behaviour happened to match.
TEST(BuildNativeTest, ShermesCommandCarriesEveryParityFlag) {
  auto argv = buildShermesCommand(
      "/kit/shermes",
      "/tmp/b/0017.js",
      "/tmp/b/0017.c",
      "node_modules/foo/index.js",
      "hn_m000017",
      /*typeScript=*/false,
      OptLevel::O3);
  EXPECT_EQ("/kit/shermes", argv[0]);
  EXPECT_TRUE(has(argv, "-emit-c"));
  EXPECT_TRUE(has(argv, "-Xes6-block-scoping"));
  EXPECT_TRUE(has(argv, "-Xasync-generators"));
  EXPECT_TRUE(has(argv, "-sm-comment=off"));
  EXPECT_TRUE(has(argv, "-g2"));
  EXPECT_TRUE(has(argv, "-w"));
  EXPECT_TRUE(has(argv, "-exported-unit=hn_m000017"));
  EXPECT_TRUE(has(argv, "-source-name=node_modules/foo/index.js"));
  EXPECT_TRUE(has(argv, "-o"));
  EXPECT_EQ("/tmp/b/0017.c", argv[at(argv, "-o") + 1]);
  EXPECT_EQ("/tmp/b/0017.js", argv.back());
  EXPECT_FALSE(has(argv, "-transform-ts"));
  EXPECT_FALSE(has(argv, "-parse-ts"));
}

TEST(BuildNativeTest, ShermesCommandAddsTransformTsOnlyForTypeScript) {
  auto argv = buildShermesCommand(
      "/kit/shermes",
      "/tmp/b/1.js",
      "/tmp/b/1.c",
      "a.ts",
      "hn_m000001",
      /*typeScript=*/true,
      OptLevel::O3);
  EXPECT_TRUE(has(argv, "-transform-ts"));
  // -parse-ts must accompany it: shermes defaults ParseFlow on whenever
  // neither -parse-flow nor -parse-ts is named explicitly, and that
  // default runs before -transform-ts's own "implies -parse-ts" logic, so
  // -transform-ts alone leaves Flow's parser handling TS-only declarations
  // like `interface`. See native_compile.cpp for the full mechanism.
  EXPECT_TRUE(has(argv, "-parse-ts"));
}

TEST(BuildNativeTest, ShermesOptLevelMapping) {
  auto level = [](OptLevel o) {
    auto argv = buildShermesCommand("s", "i.js", "o.c", "n", "u", false, o);
    for (const std::string &a : argv)
      if (a == "-O0" || a == "-Og" || a == "-Os" || a == "-O")
        return a;
    return std::string("<none>");
  };
  EXPECT_EQ("-O0", level(OptLevel::O0));
  EXPECT_EQ("-Og", level(OptLevel::O1));
  // shermes has four levels and -O is its highest, so O2 and O3 are the
  // same here and differ only on the cc side.
  EXPECT_EQ("-O", level(OptLevel::O2));
  EXPECT_EQ("-O", level(OptLevel::O3));
  EXPECT_EQ("-Os", level(OptLevel::Os));
}

/// -x c is mandatory and must precede the input: kit.manifest records the
/// C++ LINK driver, and a C++ driver compiles a .c file as C++ -- measured
/// as five hard errors on real generated code.
TEST(BuildNativeTest, CompileCommandForcesTheCLanguageBeforeTheInput) {
  KitManifest m;
  m.kitDir = "/kit";
  m.cc = "/usr/bin/clang++";
  m.ccFlags = {"-DNDEBUG", "-I/kit/include"};
  m.driverFlags = {"-arch", "arm64", "-isysroot", "/SDK"};
  auto argv = buildCompileCommand(
      m,
      "/usr/bin/clang++",
      /*driverIsClang=*/true,
      "/tmp/b/17.c",
      "/tmp/b/17.o",
      OptLevel::O3);
  int xc = at(argv, "-x");
  ASSERT_GE(xc, 0);
  EXPECT_EQ("c", argv[xc + 1]);
  int input = at(argv, "/tmp/b/17.c");
  ASSERT_GE(input, 0);
  EXPECT_LT(xc, input) << "-x c after the input does not apply to it";
  EXPECT_TRUE(has(argv, "-std=gnu11"));
  EXPECT_TRUE(has(argv, "-c"));
  EXPECT_TRUE(has(argv, "-O3"));
  EXPECT_TRUE(has(argv, "-DNDEBUG"));
  EXPECT_TRUE(has(argv, "-I/kit/include"));
  EXPECT_EQ("/tmp/b/17.o", argv[at(argv, "-o") + 1]);
  // The driver flags select a TARGET -- -arch, --target, -isysroot -- so
  // they must reach this compile as well as the assemble, or the object is
  // built for the host and the link cannot resolve it for the slice it was
  // not built for. buildAssembleCommand forwards them for exactly this
  // reason (build_exe.cpp:401).
  EXPECT_TRUE(has(argv, "-arch"));
  EXPECT_TRUE(has(argv, "arm64"));
  EXPECT_TRUE(has(argv, "-isysroot"));
  // Forwarding the whole list means link-only flags reach a compile that
  // has no use for them; this is the same suppression the assemble step
  // uses, and it is Clang-only.
  EXPECT_TRUE(has(argv, "-Qunused-arguments"));
}

TEST(BuildNativeTest, CompileCommandOmitsQunusedForNonClang) {
  KitManifest m;
  m.cc = "g++";
  auto argv = buildCompileCommand(
      m, "g++", /*driverIsClang=*/false, "a.c", "a.o", OptLevel::O3);
  // GCC rejects it outright, so guessing yes would turn an unknown driver
  // into a hard failure on first use.
  EXPECT_FALSE(has(argv, "-Qunused-arguments"));
}

TEST(BuildNativeTest, CompileOptLevelMapping) {
  KitManifest m;
  m.cc = "cc";
  auto level = [&m](OptLevel o) {
    auto argv =
        buildCompileCommand(m, "cc", /*driverIsClang=*/true, "a.c", "a.o", o);
    for (const std::string &a : argv)
      if (a.rfind("-O", 0) == 0)
        return a;
    return std::string("<none>");
  };
  EXPECT_EQ("-O0", level(OptLevel::O0));
  EXPECT_EQ("-O1", level(OptLevel::O1));
  EXPECT_EQ("-O2", level(OptLevel::O2));
  EXPECT_EQ("-O3", level(OptLevel::O3));
  EXPECT_EQ("-Os", level(OptLevel::Os));
}

TEST(BuildNativeTest, StagedPathsAreFlatAndDistinct) {
  EXPECT_EQ("/tmp/b/000017.js", stagedSourcePath("/tmp/b", 17));
  EXPECT_EQ("/tmp/b/000017.c", stagedCPath("/tmp/b", 17));
  EXPECT_EQ("/tmp/b/000017.o", stagedObjectPath("/tmp/b", 17));
  // Flat, not a tree mirroring the identity: -source-name carries the name
  // that matters, so the staged filename means nothing.
  EXPECT_EQ(
      std::string::npos, stagedSourcePath("/tmp/b", 17).find("node_modules"));
}

TEST(BuildNativeTest, LinkResponseFileQuotesAndOrders) {
  std::string out, error;
  ASSERT_TRUE(linkResponseFile(
      {"/tmp/b/payload.o", "/tmp/b/000000.o", "/Some Dir/000001.o"},
      &out,
      &error))
      << error;
  // The payload object first: buildLinkCommand places its single blob
  // argument before the entry object and the archives, and lazy archive
  // resolution depends on that order.
  EXPECT_EQ(0u, out.rfind("\"/tmp/b/payload.o\"", 0));
  // A space is quoted, not refused: checkIncbinPath permits spaces and
  // macOS paths have them, so refusing here would reject containers the
  // assembler accepts.
  EXPECT_NE(std::string::npos, out.find("\"/Some Dir/000001.o\""));
  EXPECT_EQ(3u, std::count(out.begin(), out.end(), '\n'));
}

TEST(BuildNativeTest, LinkResponseFileRefusesUnquotablePaths) {
  std::string out, error;
  // The same four characters checkIncbinPath rejects, so the two agree.
  for (const char *bad :
       {"/tmp/a\"b.o", "/tmp/a\\b.o", "/tmp/a\rb.o", "/tmp/a\nb.o"}) {
    error.clear();
    EXPECT_FALSE(linkResponseFile({bad}, &out, &error)) << bad;
    EXPECT_NE(std::string::npos, error.find(bad)) << error;
  }
}

TEST(BuildNativeTest, StageModuleWritesTheWrappedSource) {
  TempTree tree;
  std::string error;
  ASSERT_TRUE(stageModule(tree.path(), 3, "module.exports = 1;\n", &error))
      << error;
  std::ifstream in(stagedSourcePath(tree.path(), 3));
  std::ostringstream body;
  body << in.rdbuf();
  EXPECT_EQ(wrapCJS("module.exports = 1;\n"), body.str());
  EXPECT_EQ(0u, body.str().rfind(std::string(kCJSWrapperPrefix), 0));
}

/// A runner that records what it was asked to do and answers from a script.
struct FakeRunner {
  std::mutex mu;
  std::vector<std::vector<std::string>> calls;
  /// Whether a successful call creates the file its -o names.
  bool writeOutputs = true;
  /// argv[0] substring -> result.
  std::function<CommandResult(const std::vector<std::string> &)> behaviour;

  CommandResult operator()(const std::vector<std::string> &argv) {
    {
      std::lock_guard<std::mutex> lock(mu);
      calls.push_back(argv);
    }
    CommandResult r = behaviour ? behaviour(argv) : CommandResult{};
    // A successful compiler writes its -o file. The pool checks for it --
    // a tool that exits 0 and produces nothing is a failure -- so a fake
    // that does not would make every success look like that bug. Set
    // writeOutputs=false to reproduce that bug on purpose.
    if (r.ok() && writeOutputs) {
      auto it = std::find(argv.begin(), argv.end(), "-o");
      if (it != argv.end() && std::next(it) != argv.end())
        std::ofstream(*std::next(it)) << "x";
    }
    return r;
  }
};

std::vector<NativeModuleJob> threeJobs() {
  return {
      {0, "app.js", "require('./a');\n", false},
      {1, "a.js", "module.exports = 1;\n", false},
      {2, "b.ts", "export const x: number = 1;\n", true}};
}

TEST(BuildNativeTest, CompileModulesRunsTwoCommandsPerModule) {
  std::string dir = makeTempDir();
  FakeRunner fake;
  KitManifest m;
  m.cc = "cc";
  auto results = compileModules(
      threeJobs(),
      dir,
      "/kit/shermes",
      m,
      "cc",
      /*driverIsClang=*/true,
      OptLevel::O3,
      1,
      std::ref(fake));
  ASSERT_EQ(3u, results.size());
  for (const auto &r : results)
    EXPECT_TRUE(r.ok) << r.message;
  EXPECT_EQ(6u, fake.calls.size());
}

TEST(BuildNativeTest, CompileModulesStagesTheWrappedSource) {
  std::string dir = makeTempDir();
  FakeRunner fake;
  KitManifest m;
  m.cc = "cc";
  compileModules(
      threeJobs(),
      dir,
      "/kit/shermes",
      m,
      "cc",
      /*driverIsClang=*/true,
      OptLevel::O3,
      1,
      std::ref(fake));
  std::ifstream in(stagedSourcePath(dir, 1));
  std::ostringstream body;
  body << in.rdbuf();
  EXPECT_EQ(wrapCJS("module.exports = 1;\n"), body.str());
}

TEST(BuildNativeTest, CompileModulesResultsAreInModuleIndexOrder) {
  std::string dir = makeTempDir();
  FakeRunner fake;
  KitManifest m;
  m.cc = "cc";
  // Parallelism must not reorder results: the unit table is indexed by
  // module index and the verbose log reads better in order.
  auto results = compileModules(
      threeJobs(),
      dir,
      "/kit/shermes",
      m,
      "cc",
      /*driverIsClang=*/true,
      OptLevel::O3,
      4,
      std::ref(fake));
  ASSERT_EQ(3u, results.size());
  EXPECT_EQ(0u, results[0].moduleIndex);
  EXPECT_EQ(1u, results[1].moduleIndex);
  EXPECT_EQ(2u, results[2].moduleIndex);
}

TEST(BuildNativeTest, CompileModulesReportsTheStageThatFailed) {
  std::string dir = makeTempDir();
  FakeRunner fake;
  fake.behaviour = [](const std::vector<std::string> &argv) {
    CommandResult r;
    bool isShermes = argv[0].find("shermes") != std::string::npos;
    if (isShermes && argv.back().find("000001") != std::string::npos) {
      r.outcome = CommandResult::Outcome::Exited;
      r.status = 1;
      r.output = "a.js:1:1: error: nope\n";
    }
    return r;
  };
  KitManifest m;
  m.cc = "cc";
  auto results = compileModules(
      threeJobs(),
      dir,
      "/kit/shermes",
      m,
      "cc",
      /*driverIsClang=*/true,
      OptLevel::O3,
      1,
      std::ref(fake));
  EXPECT_TRUE(results[0].ok);
  EXPECT_FALSE(results[1].ok);
  EXPECT_EQ(NativeStage::Shermes, results[1].failedStage);
  EXPECT_NE(std::string::npos, results[1].diagnostics.find("error: nope"));
  EXPECT_FALSE(results[1].failedCommand.empty());
  EXPECT_TRUE(results[2].ok);
}

TEST(BuildNativeTest, CompileModulesDoesNotRunCcWhenShermesFailed) {
  std::string dir = makeTempDir();
  FakeRunner fake;
  fake.behaviour = [](const std::vector<std::string> &argv) {
    CommandResult r;
    if (argv[0].find("shermes") != std::string::npos)
      r.status = 1;
    return r;
  };
  KitManifest m;
  m.cc = "cc";
  compileModules(
      {threeJobs()[0]},
      dir,
      "/kit/shermes",
      m,
      "cc",
      /*driverIsClang=*/true,
      OptLevel::O3,
      1,
      std::ref(fake));
  ASSERT_EQ(1u, fake.calls.size());
  EXPECT_NE(std::string::npos, fake.calls[0][0].find("shermes"));
}

TEST(BuildNativeTest, CompileModulesReportsASignalDistinctly) {
  std::string dir = makeTempDir();
  FakeRunner fake;
  fake.behaviour = [](const std::vector<std::string> &) {
    CommandResult r;
    r.outcome = CommandResult::Outcome::Signalled;
    r.status = SIGSEGV;
    return r;
  };
  KitManifest m;
  m.cc = "cc";
  auto results = compileModules(
      {threeJobs()[0]},
      dir,
      "/kit/shermes",
      m,
      "cc",
      /*driverIsClang=*/true,
      OptLevel::O3,
      1,
      std::ref(fake));
  EXPECT_FALSE(results[0].ok);
  EXPECT_NE(std::string::npos, results[0].message.find("signal"));
}

TEST(BuildNativeTest, CompileModulesFailsWhenAToolWritesNoOutput) {
  std::string dir = makeTempDir();
  KitManifest m;
  m.cc = "cc";
  // Exits 0 and writes nothing. Without the existence check this counts as
  // success and the missing object surfaces much later as an undefined
  // symbol at link time, attributed to nothing.
  FakeRunner silent;
  silent.writeOutputs = false;
  auto results = compileModules(
      {threeJobs()[0]},
      dir,
      "/kit/shermes",
      m,
      "cc",
      /*driverIsClang=*/true,
      OptLevel::O3,
      1,
      std::ref(silent));
  EXPECT_FALSE(results[0].ok);
  EXPECT_EQ(NativeStage::Shermes, results[0].failedStage);
  EXPECT_NE(std::string::npos, results[0].message.find("no output"));
}

TEST(BuildNativeTest, UnitSymbolTableIsIndexedByModuleIndexWithHoles) {
  // Five container modules, of which 0, 1 and 3 are compiled JavaScript.
  std::vector<NativeModuleJob> jobs = {
      {0, "app.js", "", false}, {1, "a.js", "", false}, {3, "c.js", "", false}};
  auto table = unitSymbolTable(5, jobs);
  ASSERT_EQ(5u, table.size());
  EXPECT_EQ("hn_m000000", table[0]);
  EXPECT_EQ("hn_m000001", table[1]);
  EXPECT_EQ("", table[2]); // JSON, an addon, or a resolve-only package.json
  EXPECT_EQ("hn_m000003", table[3]);
  EXPECT_EQ("", table[4]);
}

/// A shermes-stage rejection, exited non-zero with diagnostics -- the
/// `import()`-inside-`.cjs` shape (dz 01a0a0d6-03d4) this predicate exists
/// to recover.
NativeModuleResult shermesRejection() {
  NativeModuleResult r;
  r.failedStage = NativeStage::Shermes;
  r.outcome = CommandResult::Outcome::Exited;
  r.exitStatus = 1;
  r.diagnostics = "a.js:1:1: error: nope\n";
  return r;
}

TEST(BuildNativeTest, SourceRejectionRecoversAShermesFailure) {
  EXPECT_TRUE(isNativeSourceRejection(
      /*isEntry=*/false, /*isPreload=*/false, shermesRejection()));
}

TEST(BuildNativeTest, SourceRejectionNeverAppliesToTheEntryOrAPreload) {
  // Both are certain to run, so both hard-fail unconditionally even for the
  // exact shape that is otherwise recoverable.
  EXPECT_FALSE(isNativeSourceRejection(
      /*isEntry=*/true, /*isPreload=*/false, shermesRejection()));
  EXPECT_FALSE(isNativeSourceRejection(
      /*isEntry=*/false, /*isPreload=*/true, shermesRejection()));
}

// The finding this pins: a cc-stage rejection was previously misclassified
// as a source rejection, because the predicate only excluded
// NativeStage::Stage. cc compiles shermes-GENERATED C, never the module's
// own JavaScript, so a cc failure -- the realistic case being clang running
// out of memory at -O3 on a large generated file, measured at 3.66 GB peak
// RSS on a 1,500-module build -- must be a hard build error, not a module
// packaged to throw a SyntaxError quoting the compiler's own text.
TEST(BuildNativeTest, SourceRejectionExcludesACcStageFailure) {
  NativeModuleResult r = shermesRejection();
  r.failedStage = NativeStage::Compile;
  EXPECT_FALSE(
      isNativeSourceRejection(/*isEntry=*/false, /*isPreload=*/false, r));
}

TEST(BuildNativeTest, SourceRejectionExcludesAToolchainOrMachineFailure) {
  // Signalled (a crash), SpawnFailed/WaitFailed, an exit with no captured
  // diagnostics, and a zero exit (nothing failed) are each a fact about the
  // toolchain or the machine, not the module, and must stay hard errors.
  NativeModuleResult signalled = shermesRejection();
  signalled.outcome = CommandResult::Outcome::Signalled;
  EXPECT_FALSE(isNativeSourceRejection(false, false, signalled));

  NativeModuleResult spawnFailed = shermesRejection();
  spawnFailed.outcome = CommandResult::Outcome::SpawnFailed;
  EXPECT_FALSE(isNativeSourceRejection(false, false, spawnFailed));

  NativeModuleResult noDiagnostics = shermesRejection();
  noDiagnostics.diagnostics.clear();
  EXPECT_FALSE(isNativeSourceRejection(false, false, noDiagnostics));

  NativeModuleResult exitedZero = shermesRejection();
  exitedZero.exitStatus = 0;
  EXPECT_FALSE(isNativeSourceRejection(false, false, exitedZero));
}

} // namespace
