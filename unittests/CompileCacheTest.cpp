/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <hermes/node-compat/compile-cache/compile_cache.h>

#include <gtest/gtest.h>

using namespace hermes::node_compat;

TEST(CompileCacheTest, Crc32MatchesKnownValue) {
  // The standard CRC-32 of "123456789" is 0xCBF43926.
  EXPECT_EQ(0xCBF43926u, compileCacheCrc32("123456789", 9));
}

TEST(CompileCacheTest, Crc32OfEmptyInputIsZero) {
  EXPECT_EQ(0u, compileCacheCrc32("", 0));
}

TEST(CompileCacheTest, KeyIsStableForSameInputs) {
  EXPECT_EQ(
      compileCacheKey("/a/b/c.js", CompileCacheKind::kCommonJS),
      compileCacheKey("/a/b/c.js", CompileCacheKind::kCommonJS));
}

TEST(CompileCacheTest, KeyDiffersByPath) {
  EXPECT_NE(
      compileCacheKey("/a/b/c.js", CompileCacheKind::kCommonJS),
      compileCacheKey("/a/b/d.js", CompileCacheKind::kCommonJS));
}

TEST(CompileCacheTest, KeyDiffersByKind) {
  // The two entry points hash differently shaped strings for one file, so
  // they must never share a key.
  EXPECT_NE(
      compileCacheKey("/a/b/c.js", CompileCacheKind::kCommonJS),
      compileCacheKey("/a/b/c.js", CompileCacheKind::kLoaderWrapped));
  EXPECT_NE(
      compileCacheKey("/a/b/c.js", CompileCacheKind::kLoaderWrapped),
      compileCacheKey("/a/b/c.js", CompileCacheKind::kLoaderWrappedTS));
}

TEST(CompileCacheTest, GenerationNameIsReadable) {
  EXPECT_EQ(
      "0.3.0-x86_64-bc99-3f9c21ab",
      compileCacheGenerationName("0.3.0", "x86_64", 99, 0x3f9c21ab));
}

TEST(CompileCacheTest, GenerationNamePadsConfigCrc) {
  EXPECT_EQ(
      "0.3.0-arm64-bc99-0000000f",
      compileCacheGenerationName("0.3.0", "arm64", 99, 0xf));
}

TEST(CompileCacheTest, GenerationNameVariesWithEachComponent) {
  std::string base = compileCacheGenerationName("0.3.0", "x86_64", 99, 1);
  EXPECT_NE(base, compileCacheGenerationName("0.3.1", "x86_64", 99, 1));
  EXPECT_NE(base, compileCacheGenerationName("0.3.0", "arm64", 99, 1));
  EXPECT_NE(base, compileCacheGenerationName("0.3.0", "x86_64", 100, 1));
  EXPECT_NE(base, compileCacheGenerationName("0.3.0", "x86_64", 99, 2));
}
