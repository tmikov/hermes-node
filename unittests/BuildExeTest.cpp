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
using hermes::node_compat::KitManifest;
using hermes::node_compat::payloadAssembly;
using hermes::node_compat::readKitManifest;
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
  auto cmd = buildLinkCommand(m, "/tmp/blob.o", "/tmp/app");
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
  auto cmd = buildAssembleCommand(m, "/tmp/p.s", "/tmp/p.o");
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
  auto cmd = buildAssembleCommand(m, "/tmp/p.s", "/tmp/p.o");
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

} // namespace
