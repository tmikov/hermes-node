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

// A module body is a function body, so constructs legal only inside a
// function are legal here. Scanning the source as a Program -- which is
// what this did before the scan wrapped it -- rejects this outright, and
// the module runs perfectly from disk.
TEST(RequireScannerTest, AcceptsTopLevelReturn) {
  EXPECT_EQ(
      scan("if (x) { return; }\nrequire('a');"),
      (std::vector<std::string>{"a"}));
}

// A module that declares its own `require` is not talking about the
// module's require, and its specifiers were only ever meaningful inside
// whatever bundle produced it. This is the shape browserify and older
// webpack output ship, and matching on the name alone would send the
// producer looking for these on disk.
TEST(RequireScannerTest, IgnoresShadowedRequire) {
  EXPECT_TRUE(scan("(function (require) { require('inner'); })(f);").empty());
  EXPECT_TRUE(scan("function f(require) { require('inner'); }").empty());
  EXPECT_TRUE(scan("{ let require = f; require('inner'); }").empty());
  // ... while the real one, in the same file, still counts.
  EXPECT_EQ(
      scan("function f(require) { require('inner'); }\nrequire('real');"),
      (std::vector<std::string>{"real"}));
}

// The uses of the module's require the scan cannot follow. Each is a module
// missing from the container, so the producer reports them; see
// lib/bundle/bundle_build.cpp.
std::vector<RequireGap> scanGaps(const char *src) {
  std::vector<std::string> out;
  std::vector<RequireGap> gaps;
  std::string error;
  EXPECT_TRUE(scanRequires(src, false, &out, &error, &gaps)) << error;
  return gaps;
}

TEST(RequireScannerTest, RecordsComputedRequirePositions) {
  // Both shapes the scan gives up on: an argument that is not a literal at
  // all, and a template literal with a substitution in it. The second is on
  // its own line and indented, so a position taken from the wrong node (the
  // argument, say, rather than the call) does not match. Line 1 also pins
  // that the wrapper's columns are converted back: the raw position there
  // is offset by the wrapper prefix.
  auto gaps = scanGaps("require(name);\n  require(`t${x}`);");
  ASSERT_EQ(gaps.size(), 2u);
  EXPECT_EQ(gaps[0].kind, RequireGapKind::kComputedArgument);
  EXPECT_EQ(gaps[0].line, 1u);
  EXPECT_EQ(gaps[0].column, 1u);
  EXPECT_EQ(gaps[1].kind, RequireGapKind::kComputedArgument);
  EXPECT_EQ(gaps[1].line, 2u);
  EXPECT_EQ(gaps[1].column, 3u);
}

TEST(RequireScannerTest, RecordsRequireUsedAsAValue) {
  // What @babel/core does -- endHiddenCallStack(require)(filepath) -- which
  // no amount of looking at call sites can find, because there is no
  // require() call in the source at all.
  auto gaps = scanGaps("var x = 1;\nwrap(require)(p);");
  ASSERT_EQ(gaps.size(), 1u);
  EXPECT_EQ(gaps[0].kind, RequireGapKind::kEscapedValue);
  EXPECT_EQ(gaps[0].line, 2u);
  EXPECT_EQ(gaps[0].column, 6u);
}

TEST(RequireScannerTest, DoesNotRecordAccountedForUses) {
  // A literal require() is followed, so it is not a gap. Reading a property
  // of require loads nothing, so neither is that -- and the wrapper's own
  // `require` parameter is a declaration, not a use, which if reported
  // would fire in every module ever scanned.
  EXPECT_TRUE(scanGaps("require('a'); require(`b`);").empty());
  EXPECT_TRUE(
      scanGaps("require.resolve('x'); require.cache; require.main;").empty());
  EXPECT_TRUE(scanGaps("obj.require(y); require();").empty());
  EXPECT_TRUE(scanGaps("var x = 1;").empty());
}

TEST(RequireScannerTest, DoesNotRecordGapsForAShadowedRequire) {
  // Not our require, so neither its computed calls nor its escapes are
  // holes in our container.
  EXPECT_TRUE(
      scanGaps("function f(require) { require(x); g(require); }").empty());
}

TEST(RequireScannerTest, DoesNotDeduplicateGaps) {
  // Two computed calls are two modules missing from the container even when
  // they compute the same string, and the position is what identifies each.
  EXPECT_EQ(scanGaps("require(x);\nrequire(x);").size(), 2u);
}

TEST(RequireScannerTest, GapsAreOptional) {
  // The out-parameter defaults to null, and the specifier scan is unchanged
  // when a caller does not ask for the positions.
  EXPECT_EQ(
      scan("require(name); require('ok');"), (std::vector<std::string>{"ok"}));
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
  //
  // The operand is an identifier, not a literal. Semantic resolution --
  // which the scan now runs before this visitor, to bind `require` --
  // constant-folds `0+1+1+...` down to a single NumericLiteral, so a chain
  // of literals reaches the visitor already flat and tests nothing.
  std::string src = "require('a');\nvar x = 0";
  for (int i = 0; i < 3000; ++i)
    src += "+y";
  src += ";";

  std::vector<std::string> out;
  std::string error;
  EXPECT_FALSE(scanRequires(src, false, &out, &error));
  EXPECT_FALSE(error.empty());
}

} // namespace
