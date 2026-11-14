// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// The vendored Sokol commit is written once, in hnui.h, and everything else
// is derived from or checked against it: external/sokol/CMakeLists.txt
// reads it out of the header into HNUI_SOKOL_COMMIT, and CMake hands that
// variable to lit as %sokol_commit. This test pins the variable to the
// commit external/sokol/README.md says is vendored, so a Sokol bump that
// updates one and not the other fails here rather than shipping a host and
// a header that name a commit nobody vendored.
//
// (Mutation: edit the commit in hnui.h without touching the README.)
//
// REQUIRES: ui-enabled
// Opens no window; ends naturally, and keeps `timeout` for the reason
// ui-version-check.js gives.
// RUN: timeout 60 %hermes-node %s %sokol_commit %source_dir | %FileCheck %s

// CHECK: README ok
// CHECK: HEADER ok

'use strict';

const fs = require('fs');
const variable = process.argv[2];
const sourceDir = process.argv[3];

const readme = fs.readFileSync(sourceDir + '/external/sokol/README.md', 'utf8');
const m = /Pinned at commit `([0-9a-f]+)`/.exec(readme);
console.log(
    'README ' +
    (m && m[1] === variable ? 'ok' : 'WRONG README=' + (m && m[1]) + ' CMAKE=' + variable));

const header =
    fs.readFileSync(sourceDir + '/include/hermes/node-compat/ui/hnui.h', 'utf8');
const h = /#define HNUI_SOKOL_COMMIT_PINNED "([0-9a-f]+)"/.exec(header);
console.log(
    'HEADER ' +
    (h && h[1] === variable ? 'ok' : 'WRONG HEADER=' + (h && h[1]) + ' CMAKE=' + variable));
