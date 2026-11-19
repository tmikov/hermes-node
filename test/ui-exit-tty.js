// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// A windowed process whose runtime returns ends through
// uiBeforeRuntimeCleanup's _exit(), not through runHermesNode's step 16 --
// and step 16 is what restores the terminal on plain hermes-node's natural
// path (closing the stdio handles reaches libuv's uv__tty_close). So that
// _exit() restores the terminal itself, like every other _exit() site in
// this tree (CLAUDE.md, Bootstrap Sequence), and this is what says so.
// Modelled on test-tty-restore.js's `natural` case.
//
// A rejected createWindow is what reaches the path with no display:
// HERMES_NODE_UI_GL_ROUTE=bogus fails startup before sapp_run(), the
// rejection releases the referenced handle, and the script returns with a
// window requested. (Mutation: delete uv_tty_reset_mode() from
// uiBeforeRuntimeCleanup, and RESULT says LEFT-IN-RAW-MODE.)
//
// `timeout --foreground` INSIDE the pty session, rather than around the
// harness: the harness's child calls setsid(), so a timeout outside it
// would not reach the program, and without --foreground timeout moves the
// program into a background process group, where setRawMode()'s tcsetattr
// stops it with SIGTTOU. 25 s, below the harness's own 30 s read deadline:
// a hang then ends at the inner timeout and prints `exit status: 124` and
// `RESULT: LEFT-IN-RAW-MODE` after about 25 s. (With a longer inner timeout
// the harness's deadline expires first, it then waits for the child, and
// the hang prints only `RESULT: HARNESS-FAILED no report from child`.)
// ui-gpu is a non-Apple Unix, where GNU or BSD timeout has --foreground.
//
// REQUIRES: ui, ui-gpu
//
// RUN: python3 %S/fixtures/tty/run-on-pty.py %timeout --foreground 25 env HERMES_NODE_UI_GL_ROUTE=bogus %hermes-node-ui %s | %FileCheck %s
// CHECK: exit status: 0
// CHECK: RESULT: RESTORED

'use strict';

process.stdin.setRawMode(true);
require('hermes-node:ui').createWindow().catch(() => {});
