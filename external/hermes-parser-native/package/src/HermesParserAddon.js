/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 *
 * @flow strict-local
 * @format
 */

'use strict';

const path = require('path');

const SUPPORTED = [
  'linux-x64',
  'linux-arm64',
  'darwin-x64',
  'darwin-arm64',
];

/**
 * Locate and load the prebuilt addon for the running platform.
 *
 * The path can be overridden with HERMES_PARSER_NATIVE_ADDON, which the
 * in-tree test setup uses to point at a freshly built binary.
 *
 * Resolution order:
 *  1. HERMES_PARSER_NATIVE_ADDON, if set.
 *  2. The in-repo development build at
 *     cmake-build-debug/tools/hermes-parser-native/hermes-parser.node,
 *     resolved relative to this package's location. This is checked
 *     *before* the packaged prebuild so that a source checkout always picks
 *     up the binary you are actively iterating on: prebuilds/ is gitignored
 *     and untracked, so nothing rebuilds it automatically, and a stale
 *     prebuild left over from an earlier build can otherwise silently take
 *     precedence over a freshly rebuilt dev addon (this has already caused
 *     jest to keep exercising a pre-fix binary until someone noticed and
 *     manually refreshed the prebuild). This path only exists in a source
 *     checkout of the hermes repo -- not in a published npm package, where
 *     `require()`-ing it always fails -- so it lets the rest of the
 *     workspace's test suite run against a locally built addon without
 *     every test needing to set HERMES_PARSER_NATIVE_ADDON itself, and it
 *     never affects a published package's resolution.
 *  3. prebuilds/<platform>-<arch>/hermes-parser.node, the packaged binary.
 */
function loadAddon() {
  const override = process.env.HERMES_PARSER_NATIVE_ADDON;
  if (override != null && override !== '') {
    /* $FlowFixMe[unsupported-syntax] dynamic require by design */
    return require(path.resolve(override));
  }

  const target = `${process.platform}-${process.arch}`;

  const devBuildPath = path.join(
    __dirname,
    '..', // hermes-parser-native
    '..', // js
    '..', // hermes-parser
    '..', // tools
    '..', // repo root
    'cmake-build-debug',
    'tools',
    'hermes-parser-native',
    'hermes-parser.node',
  );
  const prebuiltPath = path.join(
    __dirname,
    '..',
    'prebuilds',
    target,
    'hermes-parser.node',
  );

  for (const candidate of [devBuildPath, prebuiltPath]) {
    try {
      /* $FlowFixMe[unsupported-syntax] dynamic require by design */
      return require(candidate);
    } catch (e) {
      // Not present at this location; fall through to the next candidate.
    }
  }

  throw new Error(
    `hermes-parser-native: no prebuilt addon for ${target}. ` +
      `Supported platforms: ${SUPPORTED.join(', ')}. Checked the development ` +
      `build fallback ${devBuildPath} and ${prebuiltPath}.`,
  );
}

module.exports = loadAddon;
