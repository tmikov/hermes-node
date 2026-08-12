// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.
//
// Mirrors tools/hermes-parser/js/babel.config.js from the Hermes fork this
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
  // hermes-parser as Babel's parser, so newer Flow syntax (e.g. `as` casts)
  // beyond what @babel/parser supports is understood. Applied via
  // `overrides` (unconditionally, unlike the fork's config, since the
  // plugin here always comes from npm rather than a local build) rather
  // than the top-level `plugins` array: the two are not equivalent. A
  // plugin listed in `overrides` runs in a separate merged pass from
  // top-level `plugins`/`presets`, which changes how
  // @babel/plugin-transform-modules-commonjs (pulled in by
  // @babel/preset-env) decides whether an exported `const`/`export
  // default` binding is safe to fold into a single `const x = exports.x =
  // value` statement. Putting the plugin directly in `plugins` produces
  // Babel output that parses and runs identically but is not byte-for-byte
  // identical to the committed dist/, which is what Step 6 checks.
  overrides: [
    {
      plugins: ['babel-plugin-syntax-hermes-parser'],
    },
  ],
};
