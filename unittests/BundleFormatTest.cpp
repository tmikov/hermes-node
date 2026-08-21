/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <hermes/node-compat/bundle/bundle_generation.h>
#include <hermes/node-compat/bundle/bundle_reader.h>
#include <hermes/node-compat/bundle/bundle_writer.h>
#include <hermes/node-compat/bundle/native_digest.h>

#include "TempTree.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstring>

using namespace hermes::node_compat;
using hermes::node_compat::test::TempTree;

namespace {

constexpr uint32_t kGen = 0xABCD1234;

TEST(BundleFormatTest, RoundTripSingleModule) {
  BundleWriter w;
  uint32_t m =
      w.addModule("cli.js", ModuleKind::kJavaScript, kRequirable, "BYTECODE");
  w.setEntry(m);
  std::vector<uint8_t> bytes = w.serialize(kGen);

  std::string error;
  auto r = BundleReader::open(bytes.data(), bytes.size(), kGen, &error);
  ASSERT_TRUE(r.has_value()) << error;
  EXPECT_EQ(r->moduleCount(), 1u);
  EXPECT_EQ(r->entry(), m);
  EXPECT_EQ(r->identity(m), "cli.js");
  EXPECT_EQ(r->payload(m), "BYTECODE");
  EXPECT_EQ(r->kind(m), ModuleKind::kJavaScript);
}

TEST(BundleFormatTest, RoundTripsModuleFlags) {
  BundleWriter w;
  uint32_t a = w.addModule("cli.js", ModuleKind::kJavaScript, kRequirable, "A");
  uint32_t b = w.addModule(
      "node_modules/dep/package.json",
      ModuleKind::kJSON,
      /*flags*/ 0,
      "{}");
  w.setEntry(a);
  std::vector<uint8_t> bytes = w.serialize(bundleGenerationTag());

  std::string error;
  auto r = BundleReader::open(
      bytes.data(), bytes.size(), bundleGenerationTag(), &error);
  ASSERT_TRUE(r.has_value()) << error;
  EXPECT_TRUE(r->isRequirable(a));
  EXPECT_FALSE(r->isRequirable(b));
  EXPECT_EQ(r->formatVersion(), 4u);
}

TEST(BundleFormatTest, EdgeLookupHitAndMiss) {
  BundleWriter w;
  uint32_t a = w.addModule("a.js", ModuleKind::kJavaScript, kRequirable, "A");
  uint32_t b = w.addModule(
      "node_modules/b/index.js", ModuleKind::kJavaScript, kRequirable, "B");
  uint32_t c =
      w.addModule("c.json", ModuleKind::kJSON, kRequirable, "{\"x\":1}");
  w.addEdge(a, "b", b);
  w.addEdge(a, "./c.json", c);
  w.setEntry(a);
  std::vector<uint8_t> bytes = w.serialize(kGen);

  std::string error;
  auto r = BundleReader::open(bytes.data(), bytes.size(), kGen, &error);
  ASSERT_TRUE(r.has_value()) << error;

  EXPECT_EQ(r->lookup(a, "b"), std::optional<uint32_t>(b));
  EXPECT_EQ(r->lookup(a, "./c.json"), std::optional<uint32_t>(c));
  // Right specifier, wrong importer.
  EXPECT_FALSE(r->lookup(b, "b").has_value());
  // Unknown specifier.
  EXPECT_FALSE(r->lookup(a, "nope").has_value());
  EXPECT_EQ(r->kind(c), ModuleKind::kJSON);
  EXPECT_EQ(r->payload(c), "{\"x\":1}");
}

// Specifiers that share a prefix must not collide in the binary search.
TEST(BundleFormatTest, PrefixSpecifiersDoNotCollide) {
  BundleWriter w;
  uint32_t a = w.addModule("a.js", ModuleKind::kJavaScript, kRequirable, "A");
  uint32_t p = w.addModule("p.js", ModuleKind::kJavaScript, kRequirable, "P");
  uint32_t pp =
      w.addModule("pp.js", ModuleKind::kJavaScript, kRequirable, "PP");
  w.addEdge(a, "./p", p);
  w.addEdge(a, "./pp", pp);
  w.setEntry(a);
  std::vector<uint8_t> bytes = w.serialize(kGen);

  std::string error;
  auto r = BundleReader::open(bytes.data(), bytes.size(), kGen, &error);
  ASSERT_TRUE(r.has_value()) << error;
  EXPECT_EQ(r->lookup(a, "./p"), std::optional<uint32_t>(p));
  EXPECT_EQ(r->lookup(a, "./pp"), std::optional<uint32_t>(pp));
}

TEST(BundleFormatTest, StringTableDeduplicates) {
  BundleWriter w;
  uint32_t a = w.addModule("a.js", ModuleKind::kJavaScript, kRequirable, "A");
  uint32_t b = w.addModule("b.js", ModuleKind::kJavaScript, kRequirable, "B");
  uint32_t t = w.addModule(
      "node_modules/path-ish/index.js",
      ModuleKind::kJavaScript,
      kRequirable,
      "T");
  // Same specifier from two importers -- one string, two edges.
  w.addEdge(a, "path-ish", t);
  w.addEdge(b, "path-ish", t);
  w.setEntry(a);
  std::vector<uint8_t> small = w.serialize(kGen);

  BundleWriter w2;
  uint32_t a2 = w2.addModule("a.js", ModuleKind::kJavaScript, kRequirable, "A");
  uint32_t b2 = w2.addModule("b.js", ModuleKind::kJavaScript, kRequirable, "B");
  uint32_t t2 = w2.addModule(
      "node_modules/path-ish/index.js",
      ModuleKind::kJavaScript,
      kRequirable,
      "T");
  w2.addEdge(a2, "path-ish", t2);
  w2.addEdge(b2, "path-ish-other-longer-name", t2);
  w2.setEntry(a2);
  std::vector<uint8_t> big = w2.serialize(kGen);

  // Deduplication is observable: identical specifiers produce a smaller
  // string table than two distinct ones.
  EXPECT_LT(small.size(), big.size());
}

TEST(BundleFormatTest, PayloadEntriesAreAligned) {
  BundleWriter w;
  // Deliberately unaligned sizes.
  w.addModule("a.js", ModuleKind::kJavaScript, kRequirable, "1");
  uint32_t b =
      w.addModule("b.js", ModuleKind::kJavaScript, kRequirable, "22222");
  w.setEntry(0);
  std::vector<uint8_t> bytes = w.serialize(kGen);

  std::string error;
  auto r = BundleReader::open(bytes.data(), bytes.size(), kGen, &error);
  ASSERT_TRUE(r.has_value()) << error;
  const char *base = reinterpret_cast<const char *>(bytes.data());
  EXPECT_EQ((r->payload(b).data() - base) % kBundlePayloadAlign, 0u);
}

TEST(BundleFormatTest, RejectsBadMagic) {
  BundleWriter w;
  w.addModule("a.js", ModuleKind::kJavaScript, kRequirable, "A");
  w.setEntry(0);
  std::vector<uint8_t> bytes = w.serialize(kGen);
  bytes[0] = 'X';

  std::string error;
  auto r = BundleReader::open(bytes.data(), bytes.size(), kGen, &error);
  EXPECT_FALSE(r.has_value());
  EXPECT_NE(error.find("not a hermes-node bundle"), std::string::npos) << error;
}

TEST(BundleFormatTest, RejectsGenerationMismatch) {
  BundleWriter w;
  w.addModule("a.js", ModuleKind::kJavaScript, kRequirable, "A");
  w.setEntry(0);
  std::vector<uint8_t> bytes = w.serialize(kGen);

  std::string error;
  auto r = BundleReader::open(bytes.data(), bytes.size(), kGen + 1, &error);
  EXPECT_FALSE(r.has_value());
  EXPECT_NE(error.find("built by a different hermes-node"), std::string::npos)
      << error;
}

TEST(BundleFormatTest, RejectsFormatVersionMismatch) {
  BundleWriter w;
  w.addModule("a.js", ModuleKind::kJavaScript, kRequirable, "A");
  w.setEntry(0);
  std::vector<uint8_t> bytes = w.serialize(kGen);
  // formatVersion sits immediately after the 8 magic bytes.
  uint32_t bogus = kBundleFormatVersion + 99;
  std::memcpy(bytes.data() + 8, &bogus, sizeof(bogus));

  std::string error;
  auto r = BundleReader::open(bytes.data(), bytes.size(), kGen, &error);
  EXPECT_FALSE(r.has_value());
  EXPECT_NE(error.find("format version"), std::string::npos) << error;
}

// A flags bit outside kRequirable is rejected the same way an unrecognized
// kind is: a reader that let it through would silently ignore whatever that
// bit was meant to say.
TEST(BundleFormatTest, RejectsUnknownFlags) {
  BundleWriter w;
  w.addModule("a.js", ModuleKind::kJavaScript, kRequirable, "A");
  w.setEntry(0);
  std::vector<uint8_t> bytes = w.serialize(kGen);
  BundleHeader header;
  std::memcpy(&header, bytes.data(), sizeof(header));
  uint32_t bogus = kRequirable | (1u << 31);
  std::memcpy(
      bytes.data() + header.moduleTableOffset +
          offsetof(BundleModuleRecord, flags),
      &bogus,
      sizeof(bogus));

  std::string error;
  auto r = BundleReader::open(bytes.data(), bytes.size(), kGen, &error);
  EXPECT_FALSE(r.has_value());
  EXPECT_NE(error.find("unknown flags"), std::string::npos) << error;
}

// Identities cannot contain ".." today only because the producer derives
// them with lexically_relative() against a common ancestor -- the reader
// itself never validated identity shape. That was inert while an identity
// only indexed a payload inside the container; a bundle consumer turns an
// identity into a `root + "/" + identity` path (__filename, a
// Module._cache key, and what BundleFileSource answers questions about),
// so a container carrying an identity like "../etc/passwd" stops being
// inert. Built through BundleWriter, which does not validate identities
// itself, so this states what a container may contain rather than how the
// bytes happen to be laid out.
TEST(BundleFormatTest, RejectsMalformedIdentities) {
  auto expectRejected = [](std::string_view identity) {
    SCOPED_TRACE(identity);
    BundleWriter w;
    uint32_t m =
        w.addModule(identity, ModuleKind::kJavaScript, kRequirable, "X");
    w.setEntry(m);
    std::vector<uint8_t> bytes = w.serialize(kGen);

    std::string error;
    auto r = BundleReader::open(bytes.data(), bytes.size(), kGen, &error);
    EXPECT_FALSE(r.has_value());
    EXPECT_NE(error.find("malformed identity"), std::string::npos) << error;
  };

  expectRejected(""); // empty
  expectRejected("/etc/passwd"); // absolute
  expectRejected(std::string_view("a\0b.js", 6)); // embedded NUL
  expectRejected("."); // "." segment, whole identity
  expectRejected(".."); // ".." segment, whole identity
  expectRejected("../etc/passwd"); // ".." segment, leading
  expectRejected("a/../b.js"); // ".." segment, interior
  expectRejected("a/b/.."); // ".." segment, trailing
  expectRejected("./a.js"); // "." segment, leading
  expectRejected("a/./b.js"); // "." segment, interior
}

// An empty path segment ("a/", "a//b") would collapse under a real
// filesystem join the same way a "." segment would -- resolving to a path
// the identity does not spell.
TEST(BundleFormatTest, RejectsAnEmptyPathSegment) {
  for (const char *identity : {"a/", "a//b", "/a"}) {
    SCOPED_TRACE(identity);
    BundleWriter w;
    w.setEntry(
        w.addModule(identity, ModuleKind::kJavaScript, kRequirable, "A"));
    auto bytes = w.serialize(kGen);
    std::string error;
    EXPECT_FALSE(BundleReader::open(bytes.data(), bytes.size(), kGen, &error)
                     .has_value());
  }
}

TEST(BundleFormatTest, RejectsADuplicateIdentity) {
  // One record per identity is what lets byIdentity and BundleFileSource
  // agree about what a container holds.
  BundleWriter w;
  w.setEntry(w.addModule("a.js", ModuleKind::kJavaScript, kRequirable, "A"));
  w.addModule("a.js", ModuleKind::kJavaScript, kRequirable, "B");
  auto bytes = w.serialize(kGen);
  std::string error;
  EXPECT_FALSE(
      BundleReader::open(bytes.data(), bytes.size(), kGen, &error).has_value());
}

// Truncation at every byte length must be rejected, never crash. This is the
// test that catches missing bounds checks in the reader.
TEST(BundleFormatTest, RejectsTruncationAtEveryLength) {
  BundleWriter w;
  uint32_t a =
      w.addModule("a.js", ModuleKind::kJavaScript, kRequirable, "AAAA");
  uint32_t b =
      w.addModule("b.js", ModuleKind::kJavaScript, kRequirable, "BBBB");
  w.addEdge(a, "./b", b);
  w.setEntry(a);
  std::vector<uint8_t> bytes = w.serialize(kGen);

  for (size_t n = 0; n < bytes.size(); ++n) {
    std::string error;
    auto r = BundleReader::open(bytes.data(), n, kGen, &error);
    EXPECT_FALSE(r.has_value())
        << "accepted a truncated bundle of " << n << " bytes";
    EXPECT_FALSE(error.empty()) << "no error message at length " << n;
  }
}

TEST(BundleFormatTest, RejectsOutOfRangeEntry) {
  BundleWriter w;
  w.addModule("a.js", ModuleKind::kJavaScript, kRequirable, "A");
  w.setEntry(0);
  std::vector<uint8_t> bytes = w.serialize(kGen);
  uint32_t bogus = 99;
  std::memcpy(
      bytes.data() + offsetof(BundleHeader, entryModule),
      &bogus,
      sizeof(bogus));

  std::string error;
  auto r = BundleReader::open(bytes.data(), bytes.size(), kGen, &error);
  EXPECT_FALSE(r.has_value());
}

// The reader casts pointers directly onto the module and edge tables
// (bundle_reader.cpp, BundleReader::open), which is undefined behavior at a
// misaligned address even where the CPU tolerates it. The writer always
// emits both tables 4-byte aligned, so a misaligned offset can only arise
// from a corrupt or adversarial file.
//
// A bogus offset alone is not enough to pin this specific check: patching
// just the header field (as RejectsFormatVersionMismatch and
// RejectsOutOfRangeEntry do for other fields) still gets rejected, but for
// the wrong reason -- the reinterpret_cast then lands on bytes that used to
// be part of the real table, byte-shifted, and the resulting garbage
// record almost always fails some *other* check (kind, string index,
// payload range) well before the alignment check would matter. Verified by
// temporarily disabling the alignment check entirely: those simpler
// header-patch tests kept passing anyway. So instead these tests physically
// relocate the real table bytes to a misaligned file offset, keeping every
// other field byte-for-byte valid, which makes the alignment check the
// only thing standing between this input and acceptance.
TEST(BundleFormatTest, RejectsMisalignedModuleTableOffset) {
  BundleWriter w;
  uint32_t a =
      w.addModule("a.js", ModuleKind::kJavaScript, kRequirable, "AAAA");
  uint32_t b =
      w.addModule("b.js", ModuleKind::kJavaScript, kRequirable, "BBBB");
  w.addEdge(a, "./b", b);
  w.setEntry(a);
  std::vector<uint8_t> good = w.serialize(kGen);

  BundleHeader header;
  std::memcpy(&header, good.data(), sizeof(header));
  size_t moduleTableSize =
      header.moduleCount * static_cast<size_t>(sizeof(BundleModuleRecord));

  for (uint32_t delta = 1; delta <= 3; ++delta) {
    // Insert `delta` bytes right before the module table, pushing it (and
    // everything after it) forward -- moduleTableOffset becomes misaligned
    // by `delta`. Insert `4 - delta` more bytes right after the (now
    // relocated) module table, before the edge table, so the edge table's
    // total shift is a full 4 bytes and it lands back on a 4-byte
    // boundary: only the module table ends up misaligned.
    std::vector<uint8_t> bytes = good;
    bytes.insert(bytes.begin() + header.moduleTableOffset, delta, 0);
    bytes.insert(
        bytes.begin() + header.moduleTableOffset + delta + moduleTableSize,
        4 - delta,
        0);

    BundleHeader patched = header;
    patched.moduleTableOffset += delta;
    patched.edgeTableOffset += 4;
    patched.payloadOffset += 4;
    std::memcpy(bytes.data(), &patched, sizeof(patched));

    std::string error;
    auto r = BundleReader::open(bytes.data(), bytes.size(), kGen, &error);
    EXPECT_FALSE(r.has_value())
        << "accepted moduleTableOffset misaligned by " << delta;
    EXPECT_FALSE(error.empty()) << "no error message for delta " << delta;
  }
}

TEST(BundleFormatTest, RejectsMisalignedEdgeTableOffset) {
  BundleWriter w;
  uint32_t a =
      w.addModule("a.js", ModuleKind::kJavaScript, kRequirable, "AAAA");
  uint32_t b =
      w.addModule("b.js", ModuleKind::kJavaScript, kRequirable, "BBBB");
  w.addEdge(a, "./b", b);
  w.setEntry(a);
  std::vector<uint8_t> good = w.serialize(kGen);

  BundleHeader header;
  std::memcpy(&header, good.data(), sizeof(header));

  for (uint32_t delta = 1; delta <= 3; ++delta) {
    // Insert `delta` bytes right before the edge table, pushing it (and
    // the payload after it) forward. The module table is untouched and
    // stays aligned; only edgeTableOffset ends up misaligned.
    std::vector<uint8_t> bytes = good;
    bytes.insert(bytes.begin() + header.edgeTableOffset, delta, 0);

    BundleHeader patched = header;
    patched.edgeTableOffset += delta;
    patched.payloadOffset += delta;
    std::memcpy(bytes.data(), &patched, sizeof(patched));

    std::string error;
    auto r = BundleReader::open(bytes.data(), bytes.size(), kGen, &error);
    EXPECT_FALSE(r.has_value())
        << "accepted edgeTableOffset misaligned by " << delta;
    EXPECT_FALSE(error.empty()) << "no error message for delta " << delta;
  }
}

TEST(BundleFormatTest, EmptyBundleIsRejected) {
  BundleWriter w;
  std::string error;
  // No modules and no entry: serialize must refuse rather than emit a
  // container whose entry index cannot be valid.
  std::vector<uint8_t> bytes = w.serialize(kGen);
  auto r = BundleReader::open(bytes.data(), bytes.size(), kGen, &error);
  EXPECT_FALSE(r.has_value());
}

TEST(BundleFormatTest, OpenForInspectionAcceptsGenerationMismatch) {
  BundleWriter writer;
  uint32_t entry =
      writer.addModule("a.js", ModuleKind::kJavaScript, kRequirable, "bc-a");
  writer.setEntry(entry);
  std::vector<uint8_t> bytes = writer.serialize(0xAAAAAAAA);

  std::string error;
  // The enforcing entry point refuses it.
  EXPECT_FALSE(
      BundleReader::open(bytes.data(), bytes.size(), 0xBBBBBBBB, &error));
  EXPECT_NE(error.find("generation"), std::string::npos);

  // The inspecting one accepts it and reports the tag as stored.
  error.clear();
  auto reader =
      BundleReader::openForInspection(bytes.data(), bytes.size(), &error);
  ASSERT_TRUE(reader) << error;
  EXPECT_EQ(reader->generationTag(), 0xAAAAAAAAu);
  EXPECT_EQ(reader->formatVersion(), kBundleFormatVersion);
}

TEST(BundleFormatTest, OpenForInspectionStillRejectsStructuralDamage) {
  BundleWriter writer;
  uint32_t entry =
      writer.addModule("a.js", ModuleKind::kJavaScript, kRequirable, "bc-a");
  writer.setEntry(entry);
  std::vector<uint8_t> good = writer.serialize(0xAAAAAAAA);

  // Bad magic.
  std::vector<uint8_t> badMagic = good;
  badMagic[0] = 'X';
  std::string error;
  EXPECT_FALSE(BundleReader::openForInspection(
      badMagic.data(), badMagic.size(), &error));
  EXPECT_NE(error.find("magic"), std::string::npos);

  // Bad format version.
  std::vector<uint8_t> badVersion = good;
  reinterpret_cast<BundleHeader *>(badVersion.data())->formatVersion =
      kBundleFormatVersion + 1;
  error.clear();
  EXPECT_FALSE(BundleReader::openForInspection(
      badVersion.data(), badVersion.size(), &error));
  EXPECT_NE(error.find("format version"), std::string::npos);

  // Truncated below the header.
  error.clear();
  EXPECT_FALSE(BundleReader::openForInspection(good.data(), 8, &error));
  EXPECT_NE(error.find("truncated"), std::string::npos);
}

TEST(BundleFormatTest, EdgeAccessorMatchesLookup) {
  BundleWriter writer;
  uint32_t a =
      writer.addModule("a.js", ModuleKind::kJavaScript, kRequirable, "bc-a");
  uint32_t b =
      writer.addModule("b.js", ModuleKind::kJavaScript, kRequirable, "bc-b");
  writer.addEdge(a, "./b", b);
  writer.addEdge(b, "./a", a);
  writer.setEntry(a);
  std::vector<uint8_t> bytes = writer.serialize(0xAAAAAAAA);

  std::string error;
  auto reader =
      BundleReader::openForInspection(bytes.data(), bytes.size(), &error);
  ASSERT_TRUE(reader) << error;
  ASSERT_EQ(reader->edgeCount(), 2u);

  // Every edge the table holds must be findable by lookup, with the same
  // target. This is the property the dump relies on when it prints the
  // table in stored order.
  for (uint32_t i = 0; i < reader->edgeCount(); ++i) {
    BundleReader::EdgeView e = reader->edge(i);
    std::optional<uint32_t> found = reader->lookup(e.importer, e.specifier);
    ASSERT_TRUE(found);
    EXPECT_EQ(*found, e.target);
  }
}

TEST(BundleFormatTest, RoundTripsPreloads) {
  BundleWriter w;
  uint32_t entry =
      w.addModule("cli.js", ModuleKind::kJavaScript, kRequirable, "A");
  uint32_t setup =
      w.addModule("setup.js", ModuleKind::kJavaScript, kRequirable, "B");
  w.setEntry(entry);
  w.addPreload(setup);
  std::vector<uint8_t> bytes = w.serialize(bundleGenerationTag());

  std::string error;
  auto r = BundleReader::open(
      bytes.data(), bytes.size(), bundleGenerationTag(), &error);
  ASSERT_TRUE(r.has_value()) << error;
  EXPECT_EQ(r->formatVersion(), 4u);
  ASSERT_EQ(r->preloadCount(), 1u);
  EXPECT_EQ(r->preload(0), setup);
}

// Order is the meaning of this table: it is why preloads are a section
// rather than another flag bit on the module record.
TEST(BundleFormatTest, PreservesPreloadOrder) {
  BundleWriter w;
  uint32_t a = w.addModule("a.js", ModuleKind::kJavaScript, kRequirable, "A");
  uint32_t b = w.addModule("b.js", ModuleKind::kJavaScript, kRequirable, "B");
  uint32_t c = w.addModule("c.js", ModuleKind::kJavaScript, kRequirable, "C");
  w.setEntry(c);
  w.addPreload(b);
  w.addPreload(a);
  std::vector<uint8_t> bytes = w.serialize(bundleGenerationTag());
  std::string error;
  auto r = BundleReader::open(
      bytes.data(), bytes.size(), bundleGenerationTag(), &error);
  ASSERT_TRUE(r.has_value()) << error;
  ASSERT_EQ(r->preloadCount(), 2u);
  EXPECT_EQ(r->preload(0), b);
  EXPECT_EQ(r->preload(1), a);
}

TEST(BundleFormatTest, RejectsPreloadIndexOutOfRange) {
  BundleWriter w;
  uint32_t a = w.addModule("a.js", ModuleKind::kJavaScript, kRequirable, "A");
  w.setEntry(a);
  w.addPreload(7); // there is one module
  std::vector<uint8_t> bytes = w.serialize(bundleGenerationTag());
  std::string error;
  EXPECT_FALSE(BundleReader::open(
                   bytes.data(), bytes.size(), bundleGenerationTag(), &error)
                   .has_value());
  EXPECT_NE(
      error.find("preload references an unknown module"), std::string::npos)
      << error;
}

TEST(BundleFormatTest, RejectsNonRequirablePreload) {
  BundleWriter w;
  uint32_t a = w.addModule("a.js", ModuleKind::kJavaScript, kRequirable, "A");
  uint32_t pkg = w.addModule("package.json", ModuleKind::kJSON, 0, "{}");
  w.setEntry(a);
  w.addPreload(pkg);
  std::vector<uint8_t> bytes = w.serialize(bundleGenerationTag());
  std::string error;
  EXPECT_FALSE(BundleReader::open(
                   bytes.data(), bytes.size(), bundleGenerationTag(), &error)
                   .has_value());
  EXPECT_NE(error.find("non-requirable"), std::string::npos) << error;
}

TEST(BundleFormatTest, NativeRecordRoundTrips) {
  BundleWriter writer;
  uint32_t entry = writer.addModule(
      "cli.js", ModuleKind::kJavaScript, kRequirable, "bytecode");
  uint32_t addon = writer.addModule(
      "node_modules/a/build/Release/a.node",
      ModuleKind::kNative,
      kRequirable,
      "");
  writer.setEntry(entry);
  // A 32-byte digest with a NUL and a high byte, so the string table is
  // proven to carry raw bytes rather than a C string.
  std::string digest(32, '\x00');
  digest[0] = '\xff';
  digest[31] = '\x7f';
  writer.addNative(addon, "a.node", 4096, digest);

  std::vector<uint8_t> bytes = writer.serialize(/*generationTag=*/7);
  ASSERT_FALSE(bytes.empty());

  std::string error;
  auto reader = BundleReader::open(bytes.data(), bytes.size(), 7, &error);
  ASSERT_TRUE(reader.has_value()) << error;
  EXPECT_EQ(reader->formatVersion(), 4u);
  EXPECT_EQ(reader->kind(addon), ModuleKind::kNative);
  EXPECT_EQ(reader->payload(addon).size(), 0u);

  ASSERT_EQ(reader->nativeCount(), 1u);
  BundleReader::NativeView view = reader->native(0);
  EXPECT_EQ(view.moduleIndex, addon);
  EXPECT_EQ(view.sidecar, "a.node");
  EXPECT_EQ(view.byteLength, 4096u);
  EXPECT_EQ(view.digest, digest);

  auto found = reader->nativeFor(addon);
  ASSERT_TRUE(found.has_value());
  EXPECT_EQ(found->sidecar, "a.node");
  EXPECT_FALSE(reader->nativeFor(entry).has_value());
}

TEST(BundleFormatTest, NativeTableIsSortedByModuleIndex) {
  BundleWriter writer;
  uint32_t entry = writer.addModule(
      "cli.js", ModuleKind::kJavaScript, kRequirable, "bytecode");
  uint32_t b = writer.addModule("b.node", ModuleKind::kNative, kRequirable, "");
  uint32_t a = writer.addModule("a.node", ModuleKind::kNative, kRequirable, "");
  writer.setEntry(entry);
  // Added out of module-index order on purpose.
  writer.addNative(b, "b.node", 1, std::string(32, 'b'));
  writer.addNative(a, "a.node", 2, std::string(32, 'a'));

  std::vector<uint8_t> bytes = writer.serialize(7);
  std::string error;
  auto reader = BundleReader::open(bytes.data(), bytes.size(), 7, &error);
  ASSERT_TRUE(reader.has_value()) << error;
  ASSERT_EQ(reader->nativeCount(), 2u);
  EXPECT_LT(reader->native(0).moduleIndex, reader->native(1).moduleIndex);
  EXPECT_EQ(reader->nativeFor(a)->sidecar, "a.node");
  EXPECT_EQ(reader->nativeFor(b)->sidecar, "b.node");
}

TEST(BundleFormatTest, NativeTableOutOfRangeModuleIndexIsRejected) {
  BundleWriter writer;
  uint32_t entry = writer.addModule(
      "cli.js", ModuleKind::kJavaScript, kRequirable, "bytecode");
  // A real kNative module, so the container is valid until it is corrupted
  // below. Pointing the native record at `entry` instead would be rejected
  // by the "names a module that is not native" check, and this test would
  // pass without ever exercising the range check it is named for.
  uint32_t addon =
      writer.addModule("a.node", ModuleKind::kNative, kRequirable, "");
  writer.setEntry(entry);
  writer.addNative(addon, "a.node", 1, std::string(32, 'a'));
  std::vector<uint8_t> bytes = writer.serialize(7);

  // Corrupt the native record's moduleIndex to one past the last module.
  auto *header = reinterpret_cast<BundleHeader *>(bytes.data());
  auto *record = reinterpret_cast<BundleNativeRecord *>(
      bytes.data() + header->nativeTableOffset);
  record->moduleIndex = header->moduleCount;

  std::string error;
  auto reader = BundleReader::open(bytes.data(), bytes.size(), 7, &error);
  EXPECT_FALSE(reader.has_value());
  EXPECT_NE(error.find("native"), std::string::npos) << error;
}

TEST(BundleFormatTest, NativeTableNonNativeModuleIsRejected) {
  BundleWriter writer;
  uint32_t entry = writer.addModule(
      "cli.js", ModuleKind::kJavaScript, kRequirable, "bytecode");
  writer.setEntry(entry);
  // The native table's own moduleIndex range/sort checks accept this
  // trivially (one row, in range, nothing to sort against); only the kind
  // check below can catch a native record pointed at a JavaScript module.
  writer.addNative(entry, "a.node", 1, std::string(32, 'a'));
  std::vector<uint8_t> bytes = writer.serialize(7);

  std::string error;
  auto reader = BundleReader::open(bytes.data(), bytes.size(), 7, &error);
  EXPECT_FALSE(reader.has_value());
  EXPECT_NE(error.find("not native"), std::string::npos) << error;
}

TEST(BundleFormatTest, NativeModuleWithNoNativeTableEntryIsRejected) {
  BundleWriter writer;
  uint32_t entry = writer.addModule(
      "cli.js", ModuleKind::kJavaScript, kRequirable, "bytecode");
  // A real kNative module, deliberately never handed to addNative(): the
  // container claims a module can be dlopen'd but says nothing about where
  // its sidecar is.
  writer.addModule("a.node", ModuleKind::kNative, kRequirable, "");
  writer.setEntry(entry);
  std::vector<uint8_t> bytes = writer.serialize(7);

  std::string error;
  auto reader = BundleReader::open(bytes.data(), bytes.size(), 7, &error);
  EXPECT_FALSE(reader.has_value());
  EXPECT_NE(error.find("no native table entry"), std::string::npos) << error;
}

TEST(BundleFormatTest, NativeModuleWithNonEmptyPayloadIsRejected) {
  BundleWriter writer;
  uint32_t entry = writer.addModule(
      "cli.js", ModuleKind::kJavaScript, kRequirable, "bytecode");
  // addModule's payload parameter is generic -- nothing stops a kNative
  // module from being built with one, which is exactly the malformed
  // container this check exists to reject. A real producer never does
  // this: bundle_build.cpp never calls addModule with a non-empty payload
  // for a native.
  uint32_t addon = writer.addModule(
      "a.node", ModuleKind::kNative, kRequirable, "should not be here");
  writer.setEntry(entry);
  writer.addNative(addon, "a.node", 1, std::string(32, 'a'));
  std::vector<uint8_t> bytes = writer.serialize(7);

  std::string error;
  auto reader = BundleReader::open(bytes.data(), bytes.size(), 7, &error);
  EXPECT_FALSE(reader.has_value());
  EXPECT_NE(error.find("non-empty payload"), std::string::npos) << error;
}

TEST(BundleFormatTest, NativeDigestWrongLengthIsRejected) {
  BundleWriter writer;
  uint32_t entry = writer.addModule(
      "cli.js", ModuleKind::kJavaScript, kRequirable, "bytecode");
  uint32_t addon =
      writer.addModule("a.node", ModuleKind::kNative, kRequirable, "");
  writer.setEntry(entry);
  // 4 bytes, not kNativeDigestBytes (32): a "digest" this short cannot be a
  // SHA-256, and letting it round-trip is what used to leave
  // --dump's digestHex.resize(16) truncation depending on this check for
  // its own correctness (resize() pads a too-short string instead of
  // shrinking it).
  writer.addNative(addon, "a.node", 1, std::string(4, 'a'));
  std::vector<uint8_t> bytes = writer.serialize(7);

  std::string error;
  auto reader = BundleReader::open(bytes.data(), bytes.size(), 7, &error);
  EXPECT_FALSE(reader.has_value());
  EXPECT_NE(error.find("32 bytes"), std::string::npos) << error;
}

TEST(BundleFormatTest, NoNativesCostsNothing) {
  BundleWriter writer;
  uint32_t entry = writer.addModule(
      "cli.js", ModuleKind::kJavaScript, kRequirable, "bytecode");
  writer.setEntry(entry);
  std::vector<uint8_t> bytes = writer.serialize(7);
  std::string error;
  auto reader = BundleReader::open(bytes.data(), bytes.size(), 7, &error);
  ASSERT_TRUE(reader.has_value()) << error;
  EXPECT_EQ(reader->nativeCount(), 0u);
}

TEST(BundleGenerationTest, IsStableWithinOneBuild) {
  EXPECT_EQ(bundleGenerationTag(), bundleGenerationTag());
}

TEST(BundleGenerationTest, IsNonZero) {
  // A zero tag would make an all-zero header look plausible.
  EXPECT_NE(bundleGenerationTag(), 0u);
}

// Pins that bundleGenerationTagFor's composition actually depends on every
// one of its four inputs, and in a fixed order: a future edit that drops a
// field from the fold, reorders the fields, or corrupts the optimize byte
// changes the tag it produces here and fails one of these.
TEST(BundleGenerationTest, SameInputsReproduceTheSameTag) {
  EXPECT_EQ(
      bundleGenerationTagFor("1.2.3", "x86_64", 99, 'O'),
      bundleGenerationTagFor("1.2.3", "x86_64", 99, 'O'));
}

TEST(BundleGenerationTest, EachInputChangesTheTag) {
  uint32_t base = bundleGenerationTagFor("1.2.3", "x86_64", 99, 'O');
  EXPECT_NE(base, bundleGenerationTagFor("1.2.4", "x86_64", 99, 'O'))
      << "version string does not affect the tag";
  EXPECT_NE(base, bundleGenerationTagFor("1.2.3", "arm64", 99, 'O'))
      << "arch string does not affect the tag";
  EXPECT_NE(base, bundleGenerationTagFor("1.2.3", "x86_64", 100, 'O'))
      << "bytecode version does not affect the tag";
  EXPECT_NE(base, bundleGenerationTagFor("1.2.3", "x86_64", 99, 'o'))
      << "optimize byte does not affect the tag";
}

TEST(BundleFormatTest, NativeFileDigestMatchesKnownVector) {
  // SHA-256("abc"), the standard test vector.
  const char kAbcHex[] =
      "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
  TempTree tree;
  std::string path = tree.write("blob.bin", "abc");

  std::string error;
  auto digest = nativeFileDigest(path, &error);
  ASSERT_TRUE(digest.has_value()) << error;
  EXPECT_EQ(digest->byteLength, 3u);
  EXPECT_EQ(digest->raw.size(), 32u);
  EXPECT_EQ(nativeDigestToHex(digest->raw), kAbcHex);
}

TEST(BundleFormatTest, NativeFileDigestOfEmptyFile) {
  const char kEmptyHex[] =
      "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
  TempTree tree;
  std::string path = tree.write("empty.bin", "");
  std::string error;
  auto digest = nativeFileDigest(path, &error);
  ASSERT_TRUE(digest.has_value()) << error;
  EXPECT_EQ(digest->byteLength, 0u);
  EXPECT_EQ(nativeDigestToHex(digest->raw), kEmptyHex);
}

TEST(BundleFormatTest, NativeFileDigestReportsAMissingFile) {
  std::string error;
  auto digest = nativeFileDigest("/nonexistent/nope.node", &error);
  EXPECT_FALSE(digest.has_value());
  EXPECT_FALSE(error.empty());
}

} // namespace
