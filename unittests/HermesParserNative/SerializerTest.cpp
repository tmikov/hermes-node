/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "HermesParserJSSerializer.h"

#include <cstdint>
#include <cstring>

#include "gtest/gtest.h"
#include "hermes/AST/Context.h"
#include "hermes/Parser/JSParser.h"
#include "llvh/Support/MemoryBuffer.h"

using namespace hermes;

namespace {

/// Parse \p source and serialize it, returning the populated result.
///
/// Mirrors the production entry point
/// (tools/hermes-parser/hermes-parser-wasm.cpp): the parser must be
/// heap-allocated and moved into \c ParseResult::parser_ before \c
/// serialize() runs, since comment/token serialization reads it back out of
/// the result.
std::unique_ptr<ParseResult> parseAndSerialize(const char *source) {
  auto result = std::make_unique<ParseResult>();
  auto context = std::make_shared<Context>();
  auto &sm = context->getSourceErrorManager();

  auto fileBuf = llvh::MemoryBuffer::getMemBuffer(llvh::StringRef{source});
  int fileBufId = sm.addNewSourceBuffer(std::move(fileBuf));

  auto jsParser = std::make_unique<parser::JSParser>(
      *context, fileBufId, parser::FullParse);
  auto parsed = jsParser->parse();
  EXPECT_TRUE(parsed.hasValue());

  result->context_ = context;
  result->parser_ = std::move(jsParser);
  serialize(
      llvh::cast<ESTree::ProgramNode>(parsed.getValue()), &sm, *result, false);

  return result;
}

TEST(SerializerTest, InternsRepeatedIdentifiersOnce) {
  auto result = parseAndSerialize("var foo; foo; foo; foo;");

  uint32_t fooCount = 0;
  for (uint32_t i = 0; i < result->stringTable_.count(); ++i) {
    uint32_t start = result->stringTable_.offsets()[i];
    uint32_t end = result->stringTable_.offsets()[i + 1];
    if (result->stringTable_.data().substr(start, end - start) == "foo") {
      ++fooCount;
    }
  }

  EXPECT_EQ(1u, fooCount) << "identifier must be interned exactly once";
}

TEST(SerializerTest, PadsNumbersToEvenIndex) {
  auto result = parseAndSerialize("1.5;");

  // Locate the IEEE-754 halves of 1.5 and assert the pair starts on an even
  // index, which is what lets a Float64Array view over the region address it.
  double value = 1.5;
  uint64_t bits;
  memcpy(&bits, &value, sizeof(bits));
  const uint32_t lo = (uint32_t)bits;
  const uint32_t hi = (uint32_t)(bits >> 32);

  const auto &buf = result->programBuffer_;
  bool found = false;
  for (size_t i = 0; i + 1 < buf.size(); ++i) {
    if (buf[i] == lo && buf[i + 1] == hi) {
      EXPECT_EQ(0u, i % 2) << "double must start on an even index";
      found = true;
    }
  }
  EXPECT_TRUE(found) << "1.5 must appear in the program buffer";
}

/// \return the table id of \p str in \p table, or UINT32_MAX if absent.
uint32_t findStringId(const NativeStringTable &table, llvh::StringRef str) {
  for (uint32_t i = 0; i < table.count(); ++i) {
    uint32_t start = table.offsets()[i];
    uint32_t end = table.offsets()[i + 1];
    if (llvh::StringRef(table.data()).substr(start, end - start) == str) {
      return i;
    }
  }
  return UINT32_MAX;
}

/// \return the wire word for a non-null node of kind \p kind.
uint32_t kindWord(ESTree::NodeKind kind) {
  return (uint32_t)kind + 1;
}

TEST(SerializerTest, StringIdsAreBiasedByOne) {
  auto result = parseAndSerialize("var foo;");

  const uint32_t varId = findStringId(result->stringTable_, "var");
  const uint32_t fooId = findStringId(result->stringTable_, "foo");
  ASSERT_NE(UINT32_MAX, varId) << "\"var\" must be interned";
  ASSERT_NE(UINT32_MAX, fooId) << "\"foo\" must be interned";
  // VariableDeclaration serializes its `kind` label before its declarations,
  // so "var" takes id 0 and "foo" takes a non-zero id. That matters: with
  // fooId == 0 the biased and unbiased words would be 0 and 1, and 1 occurs
  // all over the buffer, so the assertion below could not distinguish them.
  ASSERT_NE(0u, fooId) << "\"foo\" must not be the first interned string";

  // Walk the program buffer the way HermesParserDeserializer does instead of
  // searching it: every node is a kind word (NodeKind + 1) followed by a loc
  // id, followed by its ESTree.def fields in declaration order. The
  // structural words are asserted along the way so that a layout change
  // fails here loudly rather than silently moving the slot being checked.
  const auto &buf = result->programBuffer_;
  ASSERT_LE(12u, buf.size())
      << "program buffer is too short to hold `var foo;`";

  size_t i = 0;
  ++i; // Program loc id.
  ASSERT_EQ(1u, buf[i++]) << "Program body must hold one statement";

  ASSERT_EQ(kindWord(ESTree::NodeKind::VariableDeclaration), buf[i++]);
  ++i; // VariableDeclaration loc id.
  EXPECT_EQ(varId + 1, buf[i++]) << "VariableDeclaration.kind is \"var\"";
  ASSERT_EQ(1u, buf[i++]) << "must declare exactly one declarator";

  ASSERT_EQ(kindWord(ESTree::NodeKind::VariableDeclarator), buf[i++]);
  ++i; // VariableDeclarator loc id.
  ASSERT_EQ(0u, buf[i++]) << "VariableDeclarator.init is null";

  ASSERT_EQ(kindWord(ESTree::NodeKind::Identifier), buf[i++]);
  ++i; // Identifier loc id.

  // Identifier.name: the one word this test is about.
  EXPECT_EQ(fooId + 1, buf[i])
      << "Identifier.name must be the string-table id biased by one, since "
         "zero denotes a null string";
}

} // namespace
