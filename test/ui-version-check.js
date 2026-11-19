// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// HNUI_CHECK_VERSION: an addon compares the HNUI_ABI_VERSION and the
// HNUI_SOKOL_COMMIT it was compiled against with the host's, at init.
//
// GOOD     The reference addon, built normally, loads: its init ran the
//          check and it passed.
// BAD      A second build of the same source with
//          -DHNUI_SOKOL_COMMIT_OVERRIDE="deadbee" fails to load, with an
//          error naming both the addon's commit (deadbee) and the host's.
//          (Mutation: make HNUI_CHECK_VERSION a no-op.)
// HOST     hnui_sokol_commit()/hnui_api_version() answer what the addon
//          reports the host to be, and the commit is the pinned one.
//
// Opens no window and needs no display: init needs neither. With no window
// requested nothing holds the event loop, so the script simply returns;
// `timeout` stays because lit has no per-test timeout (dz 01a0d797-e326).
//
// REQUIRES: ui, ui-gpu
// RUN: %timeout 60 %hermes-node-ui %s %hnui_demo_addon %hnui_bad_version_addon > %t.out 2> %t.err; echo "EXITCODE $?" | %FileCheck --check-prefix=CODE %s
// RUN: %FileCheck %s < %t.out

// CODE: EXITCODE 0

// CHECK: GOOD ok
// CHECK: BAD ok
// CHECK: HOST ok

'use strict';

function report(name, ok, detail) {
  console.log(name + ' ' + (ok ? 'ok' : 'WRONG ' + (detail || '')));
}

const good = require(process.argv[2]);
report('GOOD', typeof good.hostSokolCommit === 'function');

const host = good.hostSokolCommit();
let error = null;
try {
  require(process.argv[3]);
} catch (e) {
  error = e;
}
report(
    'BAD',
    error instanceof Error && error.message.includes('deadbee') &&
        error.message.includes(host) && error.message.includes('version'),
    String(error));
report('HOST', /^[0-9a-f]{7,40}$/.test(host) && good.hostApiVersion() === 1, host);
