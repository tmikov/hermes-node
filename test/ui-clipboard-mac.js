// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// Reading and writing the pasteboard on macOS (Task 16e), both read paths
// for each case: the JS Promise (`ui.readClipboard()`) and the synchronous
// native read (`hnui_read_clipboard_sync`, through the reference addon).
// The pasteboard's contents are varied by a second process,
// test/fixtures/hnui-demo/mac_pasteboard_tool.m, spawned and awaited
// before each read, so a foreign write is complete when the read starts.
//
//   OWN        our own write, 'first'
//   PASTE      an injected CLIPBOARD_PASTED with no substituted text,
//              under OWN, carries 'first': the paste reads the pasteboard
//              through the same read
//   FOREIGN    the tool writes 'second'
//   NONE       the tool clears the pasteboard: no types at all -- '' and
//              HNUI_UNAVAILABLE
//   NONSTRING  the tool leaves one non-string type (PNG): unavailable, and
//              in particular not 'second' -- a reader that remembered its
//              last string would hand that back here
//   BIG        the tool writes 1048577 bytes, past the ABI's limit: too
//              large, both ways
//   BADDATA    the tool leaves string-type data AppKit cannot decode (a
//              lone UTF-16 surrogate): stringForType: answers nil, and the
//              read fails -- ERR_CLIPBOARD_FAILED and HNUI_ERROR -- rather
//              than passing for an empty clipboard. (The other failure the
//              read checks for, a string UTF-8 cannot encode, no test can
//              produce: AppKit stores such a string as an empty one.)
//   ABANDONED  a sync read with a zero deadline times out at once, and the
//              read after it still works
//
// **The pasteboard is never the general one.** The test wrapper
// (utils/with-display.py) names a per-run pasteboard in
// HERMES_NODE_UI_TEST_PASTEBOARD; hermes-node-ui reads and writes only that
// one, traced once as `clipboard: pasteboard '<name>'`, and the script
// hands the same name to the tool. The tool refuses any name not starting
// `hnui-test-`, and so does this script, before writing anything: run
// without the wrapper, it exits 2 instead of writing the general pasteboard.
// The wrapper releases the pasteboard when the run ends.
//
// Mutations (each must turn a named line below red):
//   cache the last string and return it when no string type is present
//                                    -> NONE and NONSTRING
//   ignore HERMES_NODE_UI_TEST_PASTEBOARD (the general pasteboard)
//                                    -> TRACE's pasteboard line (and the
//                                       run wrote the real clipboard:
//                                       restore it by hand)
//   no size check                    -> BIG
//
// REQUIRES: ui, ui-backend-metal, display
//
// RUN: %with-display 120 sh -c 'HERMES_NODE_UI_TEST_HOOKS=1 HERMES_NODE_DEBUG_NATIVE=UI %hermes-node-ui %s %hnui_demo_addon %mac_pasteboard_tool > %t.out 2> %t.err'; echo "EXITCODE $?" | %FileCheck --check-prefix=CODE %s
// RUN: %FileCheck %s < %t.out
// RUN: %FileCheck --implicit-check-not='[sapp:error' --implicit-check-not='[sokol-' --implicit-check-not='status=error' --check-prefix=TRACE %s < %t.err

// CODE: EXITCODE 0

// CHECK: OWN promise ok
// CHECK-NEXT: OWN sync ok
// CHECK-NEXT: PASTE ok
// CHECK-NEXT: FOREIGN promise ok
// CHECK-NEXT: FOREIGN sync ok
// CHECK-NEXT: NONE promise ok
// CHECK-NEXT: NONE sync ok
// CHECK-NEXT: NONSTRING promise ok
// CHECK-NEXT: NONSTRING sync ok
// CHECK-NEXT: BIG promise ok
// CHECK-NEXT: BIG sync ok
// CHECK-NEXT: BADDATA promise ok
// CHECK-NEXT: BADDATA sync ok
// CHECK-NEXT: ABANDONED sync ok
// CHECK-NEXT: ABANDONED after ok
// CHECK-NEXT: RELEASED ok
// CHECK-NEXT: DONE

// TRACE: [ui] clipboard: pasteboard 'hnui-test-
// TRACE: clipboard read promise #1: status=ok bytes=5
// TRACE: clipboard read sync #1: status=ok bytes=5
// TRACE: clipboard read promise #2: status=ok bytes=6
// TRACE: clipboard read sync #2: status=ok bytes=6
// TRACE: clipboard read promise #3: status=unavailable bytes=0
// TRACE: clipboard read sync #3: status=unavailable bytes=0
// TRACE: clipboard read promise #4: status=unavailable bytes=0
// TRACE: clipboard read sync #4: status=unavailable bytes=0
// TRACE: clipboard read promise #5: status=too-large bytes=0
// TRACE: clipboard read sync #5: status=too-large bytes=0
// TRACE: clipboard read promise #6: status=error bytes=0
// TRACE: clipboard read sync #6: status=error bytes=0
// TRACE: clipboard read sync #7: deadline passed; skipped
// TRACE: clipboard read sync #8: status=ok bytes=5

'use strict';

const {spawnSync} = require('child_process');
const ui = require('hermes-node:ui');

const addonPath = process.argv[2];
const toolPath = process.argv[3];
const pasteboard = process.env.HERMES_NODE_UI_TEST_PASTEBOARD || '';

// macOS writes the clipboard to the pasteboard HERMES_NODE_UI_TEST_PASTEBOARD
// names, and to the general one -- the clipboard of whoever is using the
// Mac -- without it. utils/with-display.py sets a private hnui-test-* name;
// a run without the wrapper is refused before anything is written.
if (process.platform === 'darwin' &&
    !(process.env.HERMES_NODE_UI_TEST_PASTEBOARD || '').startsWith(
        'hnui-test-')) {
  console.log(
      'REFUSED: HERMES_NODE_UI_TEST_PASTEBOARD does not name a hnui-test-* ' +
      'pasteboard, so this run would write the general pasteboard. Run it ' +
      'under utils/with-display.py.');
  process.exit(2);
}

const SYNC_TIMEOUT_MS = 2000;
const SAPP_KEYCODE_F3 = 292;

function report(name, ok, detail) {
  console.log(name + ' ' + (ok ? 'ok' : 'WRONG ' + (detail || '')));
}

function show(x) {
  return JSON.stringify(x);
}

/** Run the tool to completion; throws unless it printed DONE. */
function tool(...args) {
  const r = spawnSync(toolPath, [pasteboard, ...args], {encoding: 'utf8'});
  if (r.status !== 0 || !/^DONE /.test(r.stdout))
    throw new Error(
        'mac_pasteboard_tool ' + args.join(' ') + ': status ' + r.status +
        ' ' + show(r.stdout) + ' ' + show(r.stderr));
  return r.stdout.trim();
}

async function readPromise() {
  try {
    return {resolved: true, value: await ui.readClipboard()};
  } catch (e) {
    return {resolved: false, code: e && e.code};
  }
}

/// Inject a CLIPBOARD_PASTED with no substituted clipboard, then a marker
/// key. Resolves with the paste texts seen before the marker arrived: the
/// platform thread handles the two in order (ui-clipboard.js).
function injectPaste(win) {
  return new Promise((resolve) => {
    const pastes = [];
    const onPaste = (e) => pastes.push(e.text);
    const onKey = (e) => {
      if (e.keyCode !== SAPP_KEYCODE_F3)
        return;
      win.off('paste', onPaste);
      win.off('keydown', onKey);
      resolve(pastes);
    };
    win.on('paste', onPaste);
    win.on('keydown', onKey);
    ui.__injectEvent({type: 'CLIPBOARD_PASTED'});
    ui.__injectEvent({type: 'KEY_DOWN', keyCode: SAPP_KEYCODE_F3});
  });
}

async function main() {
  const addon = require(addonPath);
  const win =
      await ui.createWindow({width: 160, height: 120, title: 'clipboard mac'});

  // OWN
  ui.writeClipboard('first');
  let p = await readPromise();
  report('OWN promise', p.resolved && p.value === 'first', show(p));
  let s = addon.readClipboardSync(SYNC_TIMEOUT_MS);
  report('OWN sync', s.status === 'HNUI_OK' && s.text === 'first', show(s));

  // PASTE
  const pastes = await injectPaste(win);
  report('PASTE', pastes.length === 1 && pastes[0] === 'first', show(pastes));

  // FOREIGN
  tool('set', 'second');
  p = await readPromise();
  report('FOREIGN promise', p.resolved && p.value === 'second', show(p));
  s = addon.readClipboardSync(SYNC_TIMEOUT_MS);
  report(
      'FOREIGN sync', s.status === 'HNUI_OK' && s.text === 'second', show(s));

  // NONE
  tool('clear');
  p = await readPromise();
  report('NONE promise', p.resolved && p.value === '', show(p));
  s = addon.readClipboardSync(SYNC_TIMEOUT_MS);
  report(
      'NONE sync', s.status === 'HNUI_UNAVAILABLE' && s.text === null,
      show(s));

  // NONSTRING
  tool('png');
  p = await readPromise();
  report('NONSTRING promise', p.resolved && p.value === '', show(p));
  s = addon.readClipboardSync(SYNC_TIMEOUT_MS);
  report(
      'NONSTRING sync', s.status === 'HNUI_UNAVAILABLE' && s.text === null,
      show(s));

  // BIG
  tool('big', '1048577');
  p = await readPromise();
  report(
      'BIG promise', !p.resolved && p.code === 'ERR_CLIPBOARD_TOO_LARGE',
      show(p));
  s = addon.readClipboardSync(SYNC_TIMEOUT_MS);
  report('BIG sync', s.status === 'HNUI_TOO_LARGE' && s.text === null, show(s));

  // BADDATA
  tool('surrogate');
  p = await readPromise();
  report(
      'BADDATA promise', !p.resolved && p.code === 'ERR_CLIPBOARD_FAILED',
      show(p));
  s = addon.readClipboardSync(SYNC_TIMEOUT_MS);
  report(
      'BADDATA sync', s.status === 'HNUI_ERROR' && s.text === null, show(s));

  // ABANDONED
  s = addon.readClipboardSync(0);
  report(
      'ABANDONED sync', s.status === 'HNUI_TIMEOUT' && s.text === null,
      show(s));
  ui.writeClipboard('third');
  s = addon.readClipboardSync(SYNC_TIMEOUT_MS);
  report(
      'ABANDONED after', s.status === 'HNUI_OK' && s.text === 'third',
      show(s));

  report('RELEASED', tool('release') === 'DONE 0');
  console.log('DONE');
  process.exit(0);
}

main().catch((e) => {
  console.log('FAILED ' + (e && e.stack ? e.stack : e));
  process.exit(1);
});
