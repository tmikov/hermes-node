/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <hermes/node-compat/bundle/require_scanner.h>

#include <gtest/gtest.h>

using namespace hermes::node_compat;

namespace {

std::vector<std::string> scan(const char *src, bool ts = false) {
  std::vector<std::string> out;
  std::string error;
  EXPECT_TRUE(scanRequires(src, ts, &out, &error)) << error;
  return out;
}

TEST(RequireScannerTest, FindsTopLevelRequires) {
  EXPECT_EQ(
      scan("const a = require('a');\nconst b = require(\"b\");"),
      (std::vector<std::string>{"a", "b"}));
}

TEST(RequireScannerTest, FindsNestedRequires) {
  EXPECT_EQ(
      scan("function f() { if (x) { return require('deep'); } }"),
      (std::vector<std::string>{"deep"}));
}

TEST(RequireScannerTest, DeduplicatesPreservingOrder) {
  EXPECT_EQ(
      scan("require('b'); require('a'); require('b');"),
      (std::vector<std::string>{"b", "a"}));
}

TEST(RequireScannerTest, IgnoresNonLiteralArguments) {
  EXPECT_EQ(
      scan("require(name); require('ok'); require(`t${x}`);"),
      (std::vector<std::string>{"ok"}));
}

TEST(RequireScannerTest, IgnoresRequireResolveAndMemberCalls) {
  // require.resolve() is not a module load; obj.require() is not our require.
  EXPECT_EQ(
      scan("require.resolve('x'); obj.require('y'); require('z');"),
      (std::vector<std::string>{"z"}));
}

TEST(RequireScannerTest, IgnoresRequireWithNoArguments) {
  EXPECT_EQ(scan("require(); require('a');"), (std::vector<std::string>{"a"}));
}

TEST(RequireScannerTest, AcceptsTemplateLiteralWithNoSubstitutions) {
  // `foo` is a literal string; treating it as one avoids a silent miss.
  EXPECT_EQ(scan("require(`foo`);"), (std::vector<std::string>{"foo"}));
}

TEST(RequireScannerTest, ParsesTypeScriptWhenEnabled) {
  EXPECT_EQ(
      scan("const x: string = require('ts-dep');", /*ts*/ true),
      (std::vector<std::string>{"ts-dep"}));
}

TEST(RequireScannerTest, ReportsParseError) {
  std::vector<std::string> out;
  std::string error;
  EXPECT_FALSE(scanRequires("function (", false, &out, &error));
  EXPECT_FALSE(error.empty());
}

TEST(RequireScannerTest, WarningsDoNotLeakIntoErrorOnSuccess) {
  // `019` triggers SourceErrorManager's default-enabled "looks like an
  // octal" warning. The source is otherwise valid: the scan must still
  // succeed, find the require(), and leave `error` empty -- a warning is
  // not a parse error, and a caller checking "error.empty() == success"
  // must not be misled.
  std::vector<std::string> out;
  std::string error;
  EXPECT_TRUE(scanRequires("require('a'); var x = 019;", false, &out, &error));
  EXPECT_TRUE(error.empty()) << error;
  EXPECT_EQ(out, (std::vector<std::string>{"a"}));
}

TEST(RequireScannerTest, FindsOptionalCallRequire) {
  // require?.('x') parses as OptionalCallExpressionNode, a distinct ESTree
  // kind from a plain CallExpressionNode.
  EXPECT_EQ(scan("require?.('opt');"), (std::vector<std::string>{"opt"}));
}

TEST(RequireScannerTest, FindsRequireNestedInsideTemplateSubstitution) {
  // The outer require() has a template-literal argument with a
  // substitution, so it is not itself a literal call and must be skipped;
  // the require() inside the substitution must still be found by the
  // ordinary recursive walk.
  EXPECT_EQ(
      scan("require(`x-${require('nested')}`);"),
      (std::vector<std::string>{"nested"}));
}

TEST(RequireScannerTest, ReportsErrorOnExcessiveNesting) {
  // Hermes's own recursive-descent parser handles chains of binary
  // operators with a small, fixed-size precedence stack rather than one
  // C++ call per operand (see JSParserImpl::parseBinaryExpression), so a
  // long "+1" chain parses without tripping the parser's own recursion
  // guard while still producing a BinaryExpressionNode chain nested as
  // deep as the chain is long. That is exactly the shape the visitor's
  // stack-overflow guard exists for: walking it with an unguarded
  // recursive visitor would itself overflow the C++ stack.
  std::string src = "require('a');\nvar x = 0";
  for (int i = 0; i < 3000; ++i)
    src += "+1";
  src += ";";

  std::vector<std::string> out;
  std::string error;
  EXPECT_FALSE(scanRequires(src, false, &out, &error));
  EXPECT_FALSE(error.empty());
}

} // namespace
