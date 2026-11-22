// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// A Sokol panic terminates the process, on the drawing thread too. All
// three Sokol loggers -- sokol_app's and both sokol_gfx instances' -- go
// through uiSokolLog (lib/compositor-gpu/sokol_log.h), which prints the
// item and, at level 0, restores the terminal and exits 1. Sokol's code
// after `_SG_PANIC` assumes it does not return, and the producer's logger
// used to return: a validation failure in an addon then published a black
// frame and carried on.
//
//  LOGGER      the producing instance's own logger, called with a panic
//              item through sg_query_desc(), does not return. Every build:
//              it calls the logger directly rather than relying on a
//              validation layer.
//  VALIDATION  a real one: an addon begins a pass with the target's colour
//              and resolve views swapped, on the drawing thread, and the
//              process exits 1 with Sokol's validation messages on stderr
//              and nothing printed after sg_begin_pass. Needs a sokol_gfx
//              compiled with validation (`sokol-validation`, decided by
//              running the binary); its own RUN lines say so.
//
// Mutation: make the producer's logger return again (in gl_backend.cpp,
// call uiSokolLog at a level other than 0, or return before it). LOGGER
// prints RETURNED and exits 0; VALIDATION prints PASS BEGAN.
//
// REQUIRES: ui, ui-gpu
// REQUIRES: display
//
// RUN-line shape as in ui-events.js: redirections inside `sh -c`, so the
// files are the binary's own streams whatever the display wrapper does, and
// `timeout` because lit has no per-test timeout (dz 01a0d797-e326).
//
// RUN: %with-display 120 sh -c '%hermes-node-ui %s %hnui_demo_addon logger > %t.logger.out 2> %t.logger.err'; echo "EXITCODE $?" | %FileCheck --check-prefix=CODE %s
// RUN: %FileCheck --check-prefix=LOGGER %s < %t.logger.out
// RUN: %FileCheck --check-prefix=LOGGERERR %s < %t.logger.err
//
// The VALIDATION case lives in test/ui-sokol-validation.js, because lit
// gates whole files and only that one needs `sokol-validation`.

// CODE: EXITCODE 1

// LOGGER: CALLING
// LOGGER-NOT: RETURNED
// LOGGER-NOT: EXIT listener

// LOGGERERR: [sokol-produce:panic] item {{[0-9]+}}: raised deliberately by hnui_demo_addon
// LOGGERERR-NEXT: hermes-node-ui: the producing sokol_gfx instance -- the one addons draw with -- panicked.

'use strict';

const ui = require('hermes-node:ui');
const addonPath = process.argv[2];
const mode = process.argv[3];

async function main() {
  await ui.createWindow(
      {width: 200, height: 150, title: 'hermes-node-ui sokol panic test'});
  const addon = require(addonPath);
  // Not run: a panic is an _exit(1), deliberately below JavaScript, and
  // the line proves it.
  process.on('exit', (code) => console.log('EXIT listener code=' + code));

  if (mode === 'logger') {
    console.log('CALLING');
    console.log(addon.panicThroughProducerLogger());
    process.exit(0);
  } else if (mode === 'validation') {
    console.log('REQUESTING');
    addon.requestInvalidPassFrame();
    // If the pass is accepted, the frame completes and this exits 0.
    requestAnimationFrame(() => requestAnimationFrame(() => {
      console.log('FRAME AFTER');
      process.exit(0);
    }));
  }
}

main().catch((e) => {
  console.log('FAILED ' + (e && e.stack));
  process.exit(2);
});
