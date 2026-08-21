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
  std::vector<RequireSpecifier> out;
  std::string error;
  EXPECT_TRUE(scanRequires(src, ts, &out, &error)) << error;
  std::vector<std::string> texts;
  for (const RequireSpecifier &spec : out)
    texts.push_back(spec.text);
  return texts;
}

/// Like scan(), but keeps the positions -- for the tests below that assert
/// on them rather than just on the text.
std::vector<RequireSpecifier> scanWithPositions(const char *src) {
  std::vector<RequireSpecifier> out;
  std::string error;
  EXPECT_TRUE(scanRequires(src, false, &out, &error)) << error;
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

TEST(RequireScannerTest, RecordsSpecifierPositions) {
  // Line 1 specifically, because that is where the CommonJS wrapper's
  // column offset would show up if unwrapCoords() were not applied to
  // specifiers the same way it is applied to gaps (see
  // RecordsComputedRequirePositions for the same concern on the gap side).
  // "require('b'); require('a'); require('b');" -- the third call repeats
  // the first specifier, so this also pins first-occurrence-wins: the
  // recorded position for "b" is column 1 (its first call), not column 29
  // (its second).
  auto specs = scanWithPositions("require('b'); require('a'); require('b');");
  ASSERT_EQ(specs.size(), 2u);
  EXPECT_EQ(specs[0].text, "b");
  EXPECT_EQ(specs[0].line, 1u);
  EXPECT_EQ(specs[0].column, 1u);
  EXPECT_EQ(specs[1].text, "a");
  EXPECT_EQ(specs[1].line, 1u);
  EXPECT_EQ(specs[1].column, 15u);

  // A specifier on a later, indented line, to check the line/column pair
  // together rather than only ever exercising column 1.
  auto onSecondLine = scanWithPositions("var x = 1;\n  require('dep');");
  ASSERT_EQ(onSecondLine.size(), 1u);
  EXPECT_EQ(onSecondLine[0].text, "dep");
  EXPECT_EQ(onSecondLine[0].line, 2u);
  EXPECT_EQ(onSecondLine[0].column, 3u);

  // require.resolve() gets a position the same way a bare require() does.
  auto viaResolve = scanWithPositions("require.resolve('res');");
  ASSERT_EQ(viaResolve.size(), 1u);
  EXPECT_EQ(viaResolve[0].text, "res");
  EXPECT_EQ(viaResolve[0].line, 1u);
  EXPECT_EQ(viaResolve[0].column, 1u);
}

TEST(RequireScannerTest, IgnoresNonLiteralArguments) {
  EXPECT_EQ(
      scan("require(name); require('ok'); require(`t${x}`);"),
      (std::vector<std::string>{"ok"}));
}

TEST(RequireScannerTest, RequireResolveContributesAnEdgeButNotMemberCalls) {
  // require.resolve() names a real dependency, so it is now an edge like
  // require() itself; obj.require() is still not our require.
  EXPECT_EQ(
      scan("require.resolve('x'); obj.require('y'); require('z');"),
      (std::vector<std::string>{"x", "z"}));
}

TEST(RequireScannerTest, RecordsLiteralRequireResolveTargets) {
  // A resolve is as statically visible as a require, and its target has to
  // be in the container or the call throws at run time with nothing said at
  // build time.
  EXPECT_EQ(
      scan("require.resolve('./data.json');"),
      (std::vector<std::string>{"./data.json"}));
  // Deduplicated against a require() of the same specifier, like any other.
  EXPECT_EQ(
      scan("require('./a'); require.resolve('./a');"),
      (std::vector<std::string>{"./a"}));
  // A computed argument is still invisible, and still not an escape.
  EXPECT_TRUE(scan("require.resolve(name);").empty());
  // Not our require, so not our edge.
  EXPECT_TRUE(scan("function f(require) { require.resolve('./x'); }").empty());
  // Other properties of require load nothing and contribute nothing.
  EXPECT_TRUE(scan("require.cache; require.main; require.extensions;").empty());
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
  std::vector<RequireSpecifier> out;
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

TEST(RequireScannerTest, RecordsComputedRequireResolvePositions) {
  // A computed require.resolve() is a gap for exactly the reason a computed
  // require() is: the target is invisible to the walk, so it may not be in
  // the container, and the call throws at run time with nothing said at
  // build time. yargs's apply-extends.js does this, which is how the shape
  // was found.
  //
  // This is the only assertion that fails if collect()'s record() is
  // narrowed to direct calls, e.g. by gating it on isDirectCall. The
  // specifier-side test (`scan("require.resolve(name);")` is empty) passes
  // either way -- a computed argument contributes no specifier in both
  // worlds -- and every lit test that pins the warning wording triggers it
  // with require(expr).
  auto gaps = scanGaps("var x = 1;\n  require.resolve(name);");
  ASSERT_EQ(gaps.size(), 1u);
  EXPECT_EQ(gaps[0].kind, RequireGapKind::kComputedArgument);
  EXPECT_EQ(gaps[0].line, 2u);
  EXPECT_EQ(gaps[0].column, 3u);

  // The optional spelling is the same call shape and the same gap.
  auto optional = scanGaps("require?.resolve(name);");
  ASSERT_EQ(optional.size(), 1u);
  EXPECT_EQ(optional[0].kind, RequireGapKind::kComputedArgument);
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

TEST(RequireScannerTest, DoesNotRecordGuardsAgainstRequire) {
  // "Am I running under CommonJS?" -- the value is tested, never retained,
  // so nothing can be loaded through it. This idiom is in essentially every
  // UMD-flavored file a package ships, and reporting it made the warning
  // mostly noise: of the five escapes yargs reported before this, four were
  // these and one was real.
  EXPECT_TRUE(scanGaps("if (typeof require === 'undefined') { x(); }").empty());
  EXPECT_TRUE(scanGaps("if (typeof require !== 'function') { x(); }").empty());
  EXPECT_TRUE(
      scanGaps("var a = null === require || void 0 === require;").empty());
  EXPECT_TRUE(scanGaps("if (!require) { x(); }").empty());

  // What yargs actually does with it, and what makes the difference:
  // stored on an object, from which anything can call it later.
  auto gaps = scanGaps("module.exports = { require: require };");
  ASSERT_EQ(gaps.size(), 1u);
  EXPECT_EQ(gaps[0].kind, RequireGapKind::kEscapedValue);

  // Only equality tests are excused. Anything that can hand the value on
  // still counts, whatever it looks like.
  EXPECT_EQ(scanGaps("f(require);").size(), 1u);
  EXPECT_EQ(scanGaps("var r = require;").size(), 1u);
  EXPECT_EQ(scanGaps("var s = '' + require;").size(), 1u);
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
  std::vector<RequireSpecifier> out;
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
  std::vector<RequireSpecifier> out;
  std::string error;
  EXPECT_TRUE(scanRequires("require('a'); var x = 019;", false, &out, &error));
  EXPECT_TRUE(error.empty()) << error;
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out[0].text, "a");
}

TEST(RequireScannerTest, FindsOptionalCallRequire) {
  // require?.('x') parses as OptionalCallExpressionNode, a distinct ESTree
  // kind from a plain CallExpressionNode.
  EXPECT_EQ(scan("require?.('opt');"), (std::vector<std::string>{"opt"}));
}

TEST(RequireScannerTest, FindsOptionalChainRequireResolve) {
  // require?.resolve('x') puts the '?.' between require and .resolve, so
  // the callee is an OptionalMemberExpressionNode wrapped in an ordinary
  // CallExpressionNode -- neither the shape FindsOptionalCallRequire pins
  // (an OptionalCallExpressionNode whose callee is bare `require`) nor the
  // one RecordsLiteralRequireResolveTargets pins (a non-optional
  // MemberExpressionNode). isRequireResolveMember()'s
  // OptionalMemberExpressionNode overload and isRequireResolveCallee()'s
  // matching dispatch branch (require_scanner.cpp) exist for exactly this
  // shape and had no test of their own.
  EXPECT_EQ(
      scan("require?.resolve('optres');"),
      (std::vector<std::string>{"optres"}));
  // The fully optional spelling -- both the member access and the call
  // itself marked -- parses as an OptionalCallExpressionNode whose callee
  // is the same OptionalMemberExpressionNode, exercising the two node
  // kinds together.
  EXPECT_EQ(
      scan("require?.resolve?.('optres2');"),
      (std::vector<std::string>{"optres2"}));
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

  std::vector<RequireSpecifier> out;
  std::string error;
  EXPECT_FALSE(scanRequires(src, false, &out, &error));
  EXPECT_FALSE(error.empty());
}

} // namespace
