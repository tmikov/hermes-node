// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.
//
// Mirrors tools/hermes-parser/js/babel.config.js from the Hermes branch this
// package was copied from. Kept in sync by hand; see ../README.md.

module.exports = {
  assumptions: {
    constantReexports: true,
    constantSuper: true,
    noClassCalls: true,
    noDocumentAll: true,
    noNewArrows: true,
    setPublicClassFields: true,
  },
  presets: [['@babel/preset-env', {targets: {node: '12.0.0'}}]],
  plugins: [
    ['@babel/plugin-syntax-flow', {enums: true}],
    'babel-plugin-transform-flow-enums',
    ['@babel/plugin-transform-flow-strip-types', {allowDeclareFields: true}],
    '@babel/plugin-proposal-class-properties',
  ],
  // No `overrides` entry adding babel-plugin-syntax-hermes-parser, and that
  // is the one deliberate difference from the sibling hermes-parser-native
  // copy of this file.
  //
  // Upstream's config adds it, but disables it via SKIP_HERMES_PARSER_OVERRIDE
  // for its BOOTSTRAP_PACKAGES -- and hermes-parser is one of those, because
  // the plugin depends on hermes-parser and so cannot be used to build it.
  // regen-dist.sh runs flow-remove-types first for the same reason, which
  // leaves plain JavaScript that stock @babel/parser reads without help.
  //
  // Keeping the override here would not fail; it would quietly change the
  // output. Babel parsing through hermes-parser drops comments that
  // @babel/parser preserves, so every `// $FlowExpectedError[...]` and every
  // license header would vanish from dist/. That was measured, not guessed:
  // with the override on, 52 of 56 files differed from npm's published
  // dist/ by comments alone; without it they match.
};
