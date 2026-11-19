// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// The backend-less build's answer to createWindow(): a rejection that says
// there is no window support, not a module that cannot be required.
//
// See ui-module-requirable.js for the defect. The half asserted there --
// that `hermes-node:ui` is requirable everywhere -- runs on every
// configuration. This half can only run where there is no GPU backend,
// because that is the only place ui_module_nowindow.cpp is compiled: with a
// backend present, the same call really does try to open a window.
//
// The gate is therefore `ui` and not-`ui-gpu` -- spelled out below rather
// than quoted here, because lit reads a directive out of ANY line that
// carries its name, prose included, and a second one wins over the real one
// (measured: this file was UNRESOLVED until the quotation came out). It is
// the negation that makes the gate correct rather than merely convenient:
// on Linux this file
// is UNSUPPORTED because the configuration it describes does not exist
// there, which is the one situation test/lit.cfg's comments accept
// UNSUPPORTED for (the platform genuinely has no implementation), rather
// than a probe being unable to tell.
//
// **Honesty about verification**: the gate was checked by forcing `ui-gpu`
// off in lit.cfg and confirming this file RUNS rather than reporting
// UNSUPPORTED -- so it is not dead on arrival. Its expected TEXT has not
// been executed on any machine, because no Linux configuration compiles the
// module it asserts about. If the sentence drifts, this fails loudly on
// macOS CI, which is the right direction for an unverified assertion to
// fail in.
//
// REQUIRES: ui && !ui-gpu
// RUN: %hermes-node-ui %s > %t.out 2> %t.err; echo "EXITCODE $?" | %FileCheck --check-prefix=CODE %s
// RUN: %FileCheck --check-prefix=OUT %s < %t.out

'use strict';

// Bounded from inside the script and not by a `timeout` command on the RUN
// line: this test runs on macOS, where older releases ship none. The timer
// is unref'd, so it never keeps the run alive by itself; it fires only if
// something else holds the loop open, and then fails the run. That is all
// it bounds: it cannot fire while the JS thread is blocked, or once the
// script has finished and native code hangs. lit's missing per-test timeout
// (dz 01a0d797-e326) is the gap there.
setTimeout(() => {
  console.log('WATCHDOG: the run did not end within 60 s');
  process.exit(124);
}, 60000).unref();

const ui = require('hermes-node:ui');

// The real module's other four functions exist here too, and each refuses
// with the same sentence -- not a TypeError about calling undefined.
for (const name of ['requestAnimationFrame', 'cancelAnimationFrame',
                    'writeClipboard', 'readClipboard']) {
  let message = typeof ui[name];
  try {
    ui[name](name === 'writeClipboard' ? 'x' : 1);
    message = 'returned';
  } catch (e) {
    message = 'threw ' + (e && e.message);
  }
  console.log(name + ' ' + message);
}

// A rejected Promise rather than a synchronous throw, matching the shape a
// real createWindow has: a program that awaits it sees a failed request,
// which is what this is, rather than a misuse of the API.
ui.createWindow({width: 320, height: 240, title: 'no window support'}).then(
    () => {
      console.log('RESOLVED');
      process.exit(1);
    },
    (e) => {
      console.log('REJECTED ' + (e && e.message));
      process.exit(0);
    });

// The sentence UiHost::run() prints for the same situation. Matched loosely
// enough that the surrounding explanation can be reworded, and tightly
// enough that a rejection about something else does not satisfy it.
// OUT: requestAnimationFrame threw {{.*}}no window support
// OUT: cancelAnimationFrame threw {{.*}}no window support
// OUT: writeClipboard threw {{.*}}no window support
// OUT: readClipboard threw {{.*}}no window support
// OUT: REJECTED {{.*}}no window support
// CODE: EXITCODE 0
