/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <hermes/node-compat/build-exe/build_exe.h>
#include <hermes/node-compat/build-exe/kit_manifest.h>

#include "TempTree.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

using hermes::node_compat::buildAssembleCommand;
using hermes::node_compat::buildLinkCommand;
using hermes::node_compat::DriverCandidate;
using hermes::node_compat::driverCandidates;
using hermes::node_compat::DriverSource;
using hermes::node_compat::formatCommandLine;
using hermes::node_compat::KitManifest;
using hermes::node_compat::payloadAssembly;
using hermes::node_compat::readKitManifest;
using hermes::node_compat::recordedDriverWasRejected;
using hermes::node_compat::resolveDriver;
using hermes::node_compat::versionOutputIsClang;
using hermes::node_compat::test::TempTree;

namespace {

TEST(KitManifestTest, ParsesKeysInOrder) {
  TempTree tree;
  tree.write(
      "kit/kit.manifest",
      "# comment\n"
      "\n"
      "version: 1.2.3\n"
      "cc: /usr/bin/clang\n"
      "driverflag: -O3\n"
      "driverflag: -rdynamic\n"
      "linkarg: -Wl,--whole-archive\n"
      "linkarg: {kit}/libhermesNapi.a\n"
      "linkarg: -Wl,--no-whole-archive\n"
      "linkarg: {kit}/libhermes-node-kit.a\n"
      "linkarg: -lpthread\n");
  std::string error;
  auto m = readKitManifest(tree.path("kit"), &error);
  ASSERT_TRUE(m.has_value()) << error;
  EXPECT_EQ(m->kitDir, tree.path("kit"));
  EXPECT_EQ(m->version, "1.2.3");
  EXPECT_EQ(m->cc, "/usr/bin/clang");
  ASSERT_EQ(m->driverFlags.size(), 2u);
  EXPECT_EQ(m->driverFlags[0], "-O3"); // order is load-bearing
  EXPECT_EQ(m->driverFlags[1], "-rdynamic");
  ASSERT_EQ(m->linkArgs.size(), 5u);
  EXPECT_EQ(m->linkArgs[0], "-Wl,--whole-archive");
  EXPECT_EQ(m->linkArgs[1], tree.path("kit") + "/libhermesNapi.a");
  EXPECT_EQ(m->linkArgs[2], "-Wl,--no-whole-archive");
  EXPECT_EQ(m->linkArgs[3], tree.path("kit") + "/libhermes-node-kit.a");
  EXPECT_EQ(m->linkArgs[4], "-lpthread"); // system libs stay last
}

TEST(KitManifestTest, MissingFileIsAnError) {
  TempTree tree;
  std::string error;
  EXPECT_FALSE(readKitManifest(tree.path("nope"), &error).has_value());
  EXPECT_NE(error.find("kit.manifest"), std::string::npos);
}

TEST(KitManifestTest, MissingRequiredKeyIsAnError) {
  TempTree tree;
  tree.write("kit/kit.manifest", "version: 1.2.3\nlinkarg: -lm\n");
  std::string error;
  EXPECT_FALSE(readKitManifest(tree.path("kit"), &error).has_value());
  EXPECT_NE(error.find("cc"), std::string::npos);
}

TEST(KitManifestTest, UnknownKeyIsAnError) {
  // Not ignored: an unknown key means this kit was cut by a newer
  // make-kit.py that records something we would silently drop.
  TempTree tree;
  tree.write(
      "kit/kit.manifest", "version: 1\ncc: cc\nlinkarg: -lm\nsysroot: /x\n");
  std::string error;
  EXPECT_FALSE(readKitManifest(tree.path("kit"), &error).has_value());
  EXPECT_NE(error.find("sysroot"), std::string::npos);
}

TEST(BuildExeTest, LinkCommandPutsObjectsBeforeArchives) {
  KitManifest m;
  m.cc = "/usr/bin/clang";
  m.driverFlags = {"-O3"};
  m.linkArgs = {
      "-Wl,--whole-archive",
      "/k/libhermesNapi.a",
      "-Wl,--no-whole-archive",
      "/k/libhermes-node-kit.a",
      "-lpthread"};
  m.kitDir = "/k";
  auto cmd = buildLinkCommand(m, m.cc, "/tmp/blob.o", "/tmp/app");
  ASSERT_GE(cmd.size(), 8u);
  EXPECT_EQ(cmd[0], "/usr/bin/clang");
  EXPECT_EQ(cmd[1], "-O3");
  // Both objects precede every archive, or lazy resolution finds nothing.
  auto idx = [&](const std::string &s) {
    return std::find(cmd.begin(), cmd.end(), s) - cmd.begin();
  };
  EXPECT_LT(idx("/tmp/blob.o"), idx("/k/libhermes-node-kit.a"));
  EXPECT_LT(
      idx("/k/hermes-node-bundle-main.o"), idx("/k/libhermes-node-kit.a"));
  EXPECT_LT(idx("/k/libhermes-node-kit.a"), idx("-lpthread"));
  EXPECT_LT(idx("-o"), (long)cmd.size() - 1);
}

// The payload object has to be assembled for the architecture it will be
// linked into, so the manifest's driver flags reach the assemble step as
// well as the link. The case that forced this cannot be run here at all --
// it needs a Darwin host and a two-slice kit -- so what is checkable is the
// command line: every driver flag, in the order the manifest gave them,
// ahead of the input.
TEST(BuildExeTest, AssembleCommandForwardsTheDriverFlags) {
  KitManifest m;
  m.cc = "/usr/bin/clang++";
  // Exactly what utils/make-kit.py records for a link line configured with
  // CMAKE_OSX_ARCHITECTURES="x86_64;arm64" -- verified by running that
  // classifier over such a line; `-arch` and its value are two entries.
  m.driverFlags = {
      "-arch",
      "x86_64",
      "-arch",
      "arm64",
      "-isysroot",
      "/SDK",
      "-mmacosx-version-min=11.0",
      "-rdynamic"};
  auto cmd = buildAssembleCommand(m, m.cc, true, "/tmp/p.s", "/tmp/p.o");
  ASSERT_EQ(cmd.size(), 14u);
  EXPECT_EQ(cmd[0], "/usr/bin/clang++");
  // Link-only flags (-rdynamic here) are forwarded too, so the noise they
  // make on a compile has to be silenced rather than avoided.
  EXPECT_EQ(cmd[1], "-Qunused-arguments");
  std::vector<std::string> expectedFlags(cmd.begin() + 2, cmd.begin() + 10);
  EXPECT_EQ(expectedFlags, m.driverFlags);
  std::vector<std::string> tail(cmd.begin() + 10, cmd.end());
  EXPECT_EQ(
      tail, (std::vector<std::string>{"-c", "/tmp/p.s", "-o", "/tmp/p.o"}));
}

// A kit whose manifest records no driver flags at all (the hand-written
// ones test/build-exe-errors.js uses) still gets a command that is only a
// compile of the generated file.
TEST(BuildExeTest, AssembleCommandWithNoDriverFlags) {
  KitManifest m;
  m.cc = "cc";
  auto cmd = buildAssembleCommand(m, m.cc, true, "/tmp/p.s", "/tmp/p.o");
  EXPECT_EQ(
      cmd,
      (std::vector<std::string>{
          "cc", "-Qunused-arguments", "-c", "/tmp/p.s", "-o", "/tmp/p.o"}));
}

// The lines of the generated assembly whose absence is invisible in the
// produced binary until something else goes wrong: the alignment
// openEmbeddedBundle() enforces before it will run an embedded payload,
// and -- on ELF -- the note without which the linker marks the whole
// executable as needing an executable stack.
//
// Both formats are asserted on every host. payloadAssembly() takes the
// format as a parameter precisely so that this is possible: with an
// #ifdef, the arm for the platform the test is not running on would not be
// in the test binary either, and a typo there would pass everything until
// the first Darwin build.
TEST(BuildExeTest, PayloadAssemblyElf) {
  std::string s = payloadAssembly(
      "/tmp/some dir/app.hbb", hermes::node_compat::ObjectFormat::ELF);
  EXPECT_EQ(
      s,
      "\t.section .rodata\n"
      "\t.p2align 4\n"
      "\t.globl hermesNodeBundleStart\n"
      "hermesNodeBundleStart:\n"
      "\t.incbin \"/tmp/some dir/app.hbb\"\n"
      "\t.globl hermesNodeBundleEnd\n"
      "hermesNodeBundleEnd:\n"
      "\t.section .note.GNU-stack,\"\",@progbits\n");
}

TEST(BuildExeTest, PayloadAssemblyMachO) {
  std::string s = payloadAssembly(
      "/tmp/some dir/app.hbb", hermes::node_compat::ObjectFormat::MachO);
  EXPECT_EQ(
      s,
      "\t.section __DATA,__const\n"
      "\t.p2align 4\n"
      "\t.globl _hermesNodeBundleStart\n"
      "_hermesNodeBundleStart:\n"
      "\t.incbin \"/tmp/some dir/app.hbb\"\n"
      "\t.globl _hermesNodeBundleEnd\n"
      "_hermesNodeBundleEnd:\n");
}

// Both spellings share the properties that make the payload usable at all:
// 16-byte alignment (kBundlePayloadAlign is 8, and openEmbeddedBundle()
// refuses a misaligned base), and a start symbol before the bytes with an
// end symbol after them, since the size the app entry computes is their
// difference.
TEST(BuildExeTest, PayloadAssemblyAlignsAndBracketsTheBytesInBothFormats) {
  for (auto format :
       {hermes::node_compat::ObjectFormat::ELF,
        hermes::node_compat::ObjectFormat::MachO}) {
    std::string s = payloadAssembly("/tmp/app.hbb", format);
    EXPECT_NE(s.find("\t.p2align 4\n"), std::string::npos);
    EXPECT_NE(s.find("\t.incbin \"/tmp/app.hbb\"\n"), std::string::npos);
    EXPECT_LT(s.find("hermesNodeBundleStart"), s.find(".incbin"));
    EXPECT_LT(s.find(".incbin"), s.find("hermesNodeBundleEnd"));
  }
}

// The default is the host's format, which is what buildExecutable() uses.
TEST(BuildExeTest, PayloadAssemblyDefaultsToTheHostFormat) {
  EXPECT_EQ(
      payloadAssembly("/tmp/app.hbb"),
      payloadAssembly("/tmp/app.hbb", hermes::node_compat::hostObjectFormat()));
}

// --- Driver resolution -------------------------------------------------
//
// The kit records the compiler that cut it, by absolute path. That path
// belongs to the machine that cut the kit, so anywhere else it is a hint at
// best. resolveDriver() turns it into the first of four ordered candidates
// and takes the first that works, which is what lets a kit link on a
// machine with no clang installed.
//
// The decision is separated from the probing for the same reason
// buildLinkCommand() is separated from running it: the order IS the design,
// and it is checkable with no filesystem and no toolchain. The `usable`
// predicate stands in for "an executable file at this path" or "a name
// findable on PATH".

// --cc wins outright. The user named a compiler; a working recorded path
// must not silently win over it.
TEST(BuildExeTest, ResolveDriverPrefersTheExplicitOverride) {
  auto everythingWorks = [](const DriverCandidate &) { return true; };
  auto d = resolveDriver("/opt/mycc", "/usr/bin/clang++", everythingWorks);
  ASSERT_TRUE(d.has_value());
  EXPECT_EQ(d->driver, "/opt/mycc");
  EXPECT_EQ(d->source, DriverSource::Override);
}

// No override, and the recorded path is still there: use it, so a kit built
// and used on one machine behaves exactly as it does today.
TEST(BuildExeTest, ResolveDriverUsesTheRecordedPathWhenItExists) {
  auto everythingWorks = [](const DriverCandidate &) { return true; };
  auto d = resolveDriver("", "/usr/bin/clang++", everythingWorks);
  ASSERT_TRUE(d.has_value());
  EXPECT_EQ(d->driver, "/usr/bin/clang++");
  EXPECT_EQ(d->source, DriverSource::ManifestPath);
}

// The recorded path is gone -- a different machine -- but the same compiler
// is on PATH under its own name. Prefer it over the generic c++: the kit's
// archives and driver flags came from it, and a kit cut with -stdlib=libc++
// or with LTO bitcode will not link under a different driver.
TEST(BuildExeTest, ResolveDriverFallsBackToTheRecordedBasenameOnPath) {
  auto onlyBareNames = [](const DriverCandidate &c) {
    return c.driver.find('/') == std::string::npos;
  };
  auto d = resolveDriver("", "/usr/bin/clang++", onlyBareNames);
  ASSERT_TRUE(d.has_value());
  EXPECT_EQ(d->driver, "clang++");
  EXPECT_EQ(d->source, DriverSource::ManifestName);
}

// Nothing of the kit's own compiler is present. c++ is the POSIX
// conventional driver name -- g++ on a Debian-ish Linux, clang++ on macOS
// -- so it is the portable last resort before giving up.
TEST(BuildExeTest, ResolveDriverFallsBackToPortableCxx) {
  auto onlyCxx = [](const DriverCandidate &c) { return c.driver == "c++"; };
  auto d = resolveDriver("", "/usr/bin/clang++", onlyCxx);
  ASSERT_TRUE(d.has_value());
  EXPECT_EQ(d->driver, "c++");
  EXPECT_EQ(d->source, DriverSource::Fallback);
}

// An override that does not work is a hard failure, never a substitution.
// Falling through to another compiler would link the executable with
// something other than what the user named, and say nothing -- exactly the
// action at a distance that kept $CXX out of this list. Found by running
// --cc=/nope/nothing against a real kit and watching it cheerfully produce
// a 184 MB binary with a different compiler.
TEST(BuildExeTest, ResolveDriverDoesNotSubstituteForAnUnusableOverride) {
  auto onlyCxx = [](const DriverCandidate &c) { return c.driver == "c++"; };
  EXPECT_FALSE(
      resolveDriver("/opt/mycc", "/usr/bin/clang++", onlyCxx).has_value());
}

// No candidate works. The caller must be able to name every one it tried,
// so this is reported as "none of these" rather than as one missing file.
TEST(BuildExeTest, ResolveDriverReportsNothingWhenNoCandidateWorks) {
  auto nothingWorks = [](const DriverCandidate &) { return false; };
  EXPECT_FALSE(resolveDriver("", "/usr/bin/clang++", nothingWorks).has_value());
}

// The candidate list is what the failure message lists, so it has to be
// available without probing anything -- and it must not offer a basename
// identical to the path it came from, which would make the error read as
// though the same thing were tried twice.
TEST(BuildExeTest, DriverCandidatesAreOrderedAndDeduplicated) {
  auto named = [](const std::vector<DriverCandidate> &cs) {
    std::vector<std::string> out;
    for (const auto &c : cs)
      out.push_back(c.driver);
    return out;
  };
  // An override is the ONLY candidate: see
  // ResolveDriverDoesNotSubstituteForAnUnusableOverride.
  EXPECT_EQ(
      named(driverCandidates("/opt/mycc", "/usr/bin/clang++")),
      (std::vector<std::string>{"/opt/mycc"}));
  // A manifest already recording a bare name yields no separate basename.
  EXPECT_EQ(
      named(driverCandidates("", "clang++")),
      (std::vector<std::string>{"clang++", "c++"}));
  // One recording c++ itself collapses to a single candidate.
  EXPECT_EQ(
      named(driverCandidates("", "c++")), (std::vector<std::string>{"c++"}));
}

// --- The failing command line -------------------------------------------
//
// When the assemble or the link fails, the whole command is printed so the
// user can edit it and run it themselves. That is only true if what is
// printed can be pasted into a shell, so arguments that a shell would
// re-split have to be quoted -- a kit under a path with a space, or an
// `-isysroot /Some SDK`, otherwise prints as two arguments.

TEST(BuildExeTest, PlainArgumentsAreNotQuoted) {
  // Quoting everything would be safe and unreadable. A command line made of
  // ordinary paths and flags must come out looking like one.
  EXPECT_EQ(
      formatCommandLine(
          {"/usr/bin/clang++", "-O3", "-o", "/tmp/app", "/tmp/p.o"}),
      "/usr/bin/clang++ -O3 -o /tmp/app /tmp/p.o");
}

TEST(BuildExeTest, ArgumentsAShellWouldResplitAreQuoted) {
  EXPECT_EQ(
      formatCommandLine({"cc", "-isysroot", "/Some SDK", "-o", "a b"}),
      "cc -isysroot '/Some SDK' -o 'a b'");
  // An empty argument is a real argument and vanishes without quotes.
  EXPECT_EQ(formatCommandLine({"cc", ""}), "cc ''");
}

TEST(BuildExeTest, SingleQuotesInAnArgumentSurviveQuoting) {
  // The one case naive single-quoting gets wrong. Pasting the result must
  // reproduce the argument exactly, apostrophe included.
  EXPECT_EQ(
      formatCommandLine({"cc", "/tmp/it's here/x.o"}),
      "cc '/tmp/it'\\''s here/x.o'");
}

// --- Driver identification ----------------------------------------------
//
// -Qunused-arguments is a Clang spelling; g++ rejects it outright, which is
// what made the whole feature require Clang. It cannot simply be dropped:
// build_exe.h forwards the ENTIRE driver-flag list to the assemble step on
// purpose, so that a target-selecting flag can never be missed, and the
// price is link-only flags reaching a compile that warns about each one.
// So the suppression has to follow the driver actually in use.
//
// It cannot be recorded in the manifest either, because --cc can change the
// driver long after the kit was cut. That leaves asking the driver, and
// parsing its answer is the part worth testing without running anything.

TEST(BuildExeTest, RecognizesClangFromItsVersionBanner) {
  // Real first lines, copied from the machines this has to work on.
  EXPECT_TRUE(versionOutputIsClang("Ubuntu clang version 18.1.3 (1ubuntu1)\n"));
  EXPECT_TRUE(
      versionOutputIsClang("Apple clang version 15.0.0 (clang-1500.3.9.4)\n"));
  EXPECT_TRUE(versionOutputIsClang("clang version 19.1.0\n"));
}

TEST(BuildExeTest, DoesNotMistakeGccForClang) {
  EXPECT_FALSE(versionOutputIsClang(
      "g++ (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0\n"
      "Copyright (C) 2023 Free Software Foundation, Inc.\n"));
  // A driver that answers nothing at all is not Clang either. Guessing yes
  // would hand it a flag it may reject, turning an unknown driver into a
  // hard failure on its very first use.
  EXPECT_FALSE(versionOutputIsClang(""));
}

// --verbose says so when the kit's own compiler could not be used, because
// that is the difference between "this kit is being used as intended" and
// "we are linking with a substitute". But it is only true when the recorded
// compiler was actually tried and rejected: --cc skips it without judging
// it, and saying it "was not usable" then is simply false.
TEST(BuildExeTest, OnlyReportsTheRecordedDriverRejectedWhenItWasTried) {
  EXPECT_FALSE(recordedDriverWasRejected(DriverSource::Override));
  EXPECT_FALSE(recordedDriverWasRejected(DriverSource::ManifestPath));
  EXPECT_TRUE(recordedDriverWasRejected(DriverSource::ManifestName));
  EXPECT_TRUE(recordedDriverWasRejected(DriverSource::Fallback));
}

// --- The commands use the RESOLVED driver -------------------------------

// The manifest's cc: is only the first candidate. Once resolution has
// chosen something else -- a basename off PATH, or plain c++ -- both
// commands must run THAT, or the resolution would be decorative.
TEST(BuildExeTest, AssembleCommandRunsTheResolvedDriver) {
  KitManifest m;
  m.cc = "/usr/bin/clang++"; // recorded, but absent on this machine
  auto cmd = buildAssembleCommand(m, "c++", false, "/tmp/p.s", "/tmp/p.o");
  ASSERT_FALSE(cmd.empty());
  EXPECT_EQ(cmd[0], "c++");
}

TEST(BuildExeTest, LinkCommandRunsTheResolvedDriver) {
  KitManifest m;
  m.cc = "/usr/bin/clang++";
  m.kitDir = "/k";
  auto cmd = buildLinkCommand(m, "c++", "/tmp/blob.o", "/tmp/app");
  ASSERT_FALSE(cmd.empty());
  EXPECT_EQ(cmd[0], "c++");
}

// The flag appears only for Clang. With any other driver the command is the
// same minus that one argument -- the driver flags still all get forwarded,
// because the reason for forwarding them has nothing to do with which
// driver it is.
TEST(BuildExeTest, AssembleCommandOmitsTheClangOnlyFlagForOtherDrivers) {
  KitManifest m;
  m.cc = "c++";
  m.driverFlags = {"-arch", "arm64", "-rdynamic"};
  auto gcc = buildAssembleCommand(m, "c++", false, "/tmp/p.s", "/tmp/p.o");
  EXPECT_EQ(
      gcc,
      (std::vector<std::string>{
          "c++",
          "-arch",
          "arm64",
          "-rdynamic",
          "-c",
          "/tmp/p.s",
          "-o",
          "/tmp/p.o"}));
  // Same manifest, Clang: identical but for the suppression flag.
  auto clang = buildAssembleCommand(m, "c++", true, "/tmp/p.s", "/tmp/p.o");
  ASSERT_EQ(clang.size(), gcc.size() + 1);
  EXPECT_EQ(clang[1], "-Qunused-arguments");
  clang.erase(clang.begin() + 1);
  EXPECT_EQ(clang, gcc);
}

} // namespace
