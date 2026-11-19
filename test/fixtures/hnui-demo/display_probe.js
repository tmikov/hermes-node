// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// Stage 2 of the macOS `display` lit feature (test/lit.cfg): open a small
// window, let the platform thread run five frames, and say so. Run with
// HERMES_NODE_UI_TEST_HOOKS=1 (for __waitPlatformFrames) and
// HERMES_NODE_DEBUG_NATIVE=UI, under %with-display; lit.cfg then requires
// exit 0, the sentinel below on stdout, and the Metal backend's
// `present: drawable acquired` on stderr.
//
// The sentinel is assembled from two halves so that it never appears in
// this file as a whole word: a probe that merely echoed its own input
// could otherwise satisfy the search. It is printed only if the five
// frames really ran -- a stricter answer than the trace alone.

const ui = require('hermes-node:ui');

ui.createWindow({width: 160, height: 120, title: 'hermes-node-ui display probe'})
  .catch(() => {});
if (ui.__waitPlatformFrames(5))
  console.log('DISPLAY_PROBE' + '_OK');
process.exit(0);
