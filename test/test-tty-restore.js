// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.
//
// A program that puts the terminal in raw mode must leave it as it found it,
// however it exits. Raw mode clears ECHO and ICANON, so getting this wrong
// means the shell the user returns to stops echoing what they type -- the
// terminal looks broken and nothing says why.
//
// The program itself is not required to restore anything: tetris-cli calls
// setRawMode(true) on its fifth line and quits with process.exit(0), never
// clearing it, and it works under node. What restores it there is
// ResetStdio(), which node registers with atexit() and which calls
// uv_tty_reset_mode(). This runtime cannot use atexit -- both exit paths call
// _exit() on purpose, to keep ASAN from reporting the live Hermes runtime as
// thousands of leaks -- so each of them restores explicitly, and these cases
// are what say so.
//
// Each case runs this file on a pty and reads the tty flags back afterwards;
// see fixtures/tty/run-on-pty.py for why the read happens where it does.
//
// RUN: python3 %S/fixtures/tty/run-on-pty.py %hermes-node %s natural | %FileCheck %s --check-prefix=NATURAL
// RUN: python3 %S/fixtures/tty/run-on-pty.py %hermes-node %s exit | %FileCheck %s --check-prefix=EXIT
// RUN: python3 %S/fixtures/tty/run-on-pty.py %hermes-node %s exit-nonzero | %FileCheck %s --check-prefix=EXITNZ
// RUN: python3 %S/fixtures/tty/run-on-pty.py %hermes-node %s throw-async | %FileCheck %s --check-prefix=THROWASYNC

'use strict';

const mode = process.argv[2];

process.stdin.setRawMode(true);

switch (mode) {
  // Falling off the end of the program. This path already worked: step 16 of
  // the bootstrap closes the stdio handles and libuv's uv__tty_close restores
  // termios on the way past. It is here so that a change to that cleanup
  // cannot quietly take it away.
  //
  // NATURAL: exit status: 0
  // NATURAL: RESULT: RESTORED
  case 'natural':
    break;

  // What tetris-cli does.
  //
  // EXIT: exit status: 0
  // EXIT: RESULT: RESTORED
  case 'exit':
    process.exit(0);
    break;

  // The status must survive the restore -- a terminal fix that swallowed the
  // exit code would break every shell script branching on it.
  //
  // EXITNZ: exit status: 3
  // EXITNZ: RESULT: RESTORED
  case 'exit-nonzero':
    process.exit(3);
    break;

  // An exception escaping a timer callback reports and exits 1 through a
  // different _exit() than process.exit() uses, so it needs its own restore
  // and its own case.
  //
  // THROWASYNC: exit status: 1
  // THROWASYNC: RESULT: RESTORED
  case 'throw-async':
    setTimeout(() => {
      throw new Error('boom');
    }, 1);
    break;

  default:
    console.log('unknown mode: ' + mode);
    process.exit(9);
}
