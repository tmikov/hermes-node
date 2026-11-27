// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// UTF-16 surrogates in CHAR events, end to end. Sokol's macOS backend sends
// each UTF-16 code unit of a keystroke's characters as a CHAR of its own,
// so U+1F600 arrives as CHAR 0xD83D then CHAR 0xDE00; the platform thread's
// converter (SappEventConverter, sapp_event_convert.h) must deliver that as
// ONE `char` event. X11 sends whole code points, so only injection reaches
// this on Linux -- and injection enters at the sapp_event level, above the
// converter, so it exercises the same code a Mac keystroke does.
//
// Each case injects its CHARs and then a KEY_DOWN marker, and collects the
// `char` events delivered before the marker's `keydown`. No barrier is
// needed: the platform thread converts injected events in injection order
// and the JS thread delivers them in that order, so the marker is causally
// behind everything injected ahead of it, whichever drains they land in.
//
//  PAIR     high then low surrogate: exactly one `char`, U+1F600, with the
//           low surrogate's shiftKey.
//  ORPHAN   high surrogate then 'a': U+FFFD, then 'a', as two events.
//  BEFORE   high surrogate then the marker: U+FFFD is delivered, and ahead
//           of the marker's keydown.
//  LOW      a lone low surrogate: U+FFFD.
//
// Mutations, both run: make SappEventConverter::convert call
// convertSappEvent() alone -- PAIR fails with two U+FFFD events; drop the
// orphan instead of delivering it -- ORPHAN fails with 'a' alone.
//
// REQUIRES: ui, ui-gpu
// REQUIRES: display
//
// RUN: %with-display 300 sh -c 'HERMES_NODE_UI_TEST_HOOKS=1 HERMES_NODE_DEBUG_NATIVE=UI %hermes-node-ui %s > %t.out 2> %t.err'; echo "EXITCODE $?" | %FileCheck --check-prefix=CODE %s
// RUN: %FileCheck %s < %t.out
// RUN: %FileCheck --implicit-check-not='[sapp:error' --implicit-check-not='[sokol-' --check-prefix=TRACE %s < %t.err

// CODE: EXITCODE 0

// CHECK: PAIR ok
// CHECK: ORPHAN ok
// CHECK: BEFORE ok
// CHECK: LOW ok
// CHECK: DONE

// TRACE: window ready

'use strict';

const ui = require('hermes-node:ui');

const SAPP_KEYCODE_F1 = 290;
const SAPP_MODIFIER_SHIFT = 0x1;

function hex(s) {
  return JSON.stringify(Array.from(s, (c) => c.codePointAt(0).toString(16)));
}

/**
 * Inject \p chars (each `{charCode, modifiers}`) then the marker, and
 * resolve with the log of what was delivered up to the marker's keydown.
 */
function run(win, chars) {
  return new Promise((resolve) => {
    const log = [];
    const onChar = (e) => log.push(e);
    const onKey = (e) => {
      if (e.keyCode !== SAPP_KEYCODE_F1)
        return;
      win.off('char', onChar);
      win.off('keydown', onKey);
      log.push('marker');
      resolve(log);
    };
    win.on('char', onChar);
    win.on('keydown', onKey);
    for (const c of chars)
      ui.__injectEvent(Object.assign({type: 'CHAR'}, c));
    ui.__injectEvent({type: 'KEY_DOWN', keyCode: SAPP_KEYCODE_F1});
  });
}

function texts(log) {
  return log.map((e) => (e === 'marker' ? e : e.char));
}

function report(name, ok, log) {
  console.log(
      name + ' ' +
      (ok ? 'ok' : 'WRONG ' + JSON.stringify(texts(log).map(
          (t) => (t === 'marker' ? t : hex(t))))));
}

async function main() {
  const win = await ui.createWindow({
    width: 160,
    height: 120,
    title: 'hermes-node-ui surrogates test',
  });

  let log = await run(win, [
    {charCode: 0xD83D},
    {charCode: 0xDE00, modifiers: SAPP_MODIFIER_SHIFT},
  ]);
  report(
      'PAIR',
      log.length === 2 && log[0].char === '\u{1F600}' &&
          log[0].shiftKey === true && log[1] === 'marker',
      log);

  log = await run(win, [{charCode: 0xD83D}, {charCode: 0x61}]);
  report(
      'ORPHAN',
      JSON.stringify(texts(log)) === JSON.stringify(['�', 'a', 'marker']),
      log);

  log = await run(win, [{charCode: 0xD83D}]);
  report(
      'BEFORE',
      JSON.stringify(texts(log)) === JSON.stringify(['�', 'marker']),
      log);

  log = await run(win, [{charCode: 0xDE00}]);
  report(
      'LOW',
      JSON.stringify(texts(log)) === JSON.stringify(['�', 'marker']),
      log);

  console.log('DONE');
  process.exit(0);
}

main().catch((e) => {
  console.log('FAILED ' + (e && e.stack));
  process.exit(1);
});
