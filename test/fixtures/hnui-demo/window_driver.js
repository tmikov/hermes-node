// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// The one place a test reaches a REAL window event -- one that enters
// above Sokol rather than below it, as `ui.__injectEvent` does.
//
//  - Linux: a second X client, test/fixtures/hnui-demo/x11_window_driver.c,
//    sends real KeyPress, XResizeWindow and WM_DELETE_WINDOW requests, and
//    reads the window's state back from the server.
//  - macOS: `ui.__driveWindow(command)` (Task 16d), which runs one AppKit
//    call on the main thread -- `[window sendEvent:]`, `setContentSize:`,
//    `performClose:` -- and returns what it read back. A second process
//    cannot drive another's window there without Accessibility or
//    Automation consent, which nothing here assumes.
//
// Both answer with the X11 driver's lines, so a test reads one format:
// `SENT <verb>` for an action, `STATE viewable=<0|1> <w>x<h>` for `state`,
// `ERROR ...` for a refusal. On macOS `onLost()` never fires: nothing there
// watches the window from outside.
//
// The contract this file assumes of `ui.__driveWindow(command)`, which
// Task 16d implements: it is synchronous, takes the same command strings
// the X11 driver does (`key <c>`, `resize <w> <h>`, `close`, `state`, plus
// `miniaturize`, `deminiaturize`, `terminate`), and returns a string --
// for `state`, `viewable=<0|1> <w>x<h>`; for an action, anything not
// starting with `ERROR` (its read-back); a refusal starts with `ERROR`.

'use strict';

const {spawn, spawnSync} = require('child_process');

function verbOf(command) {
  return String(command).trim().split(/\s+/)[0];
}

/** Map one `ui.__driveWindow` answer onto the X11 driver's line. */
function driveLine(ui, command) {
  const answer = String(ui.__driveWindow(command));
  if (answer.startsWith('ERROR'))
    return answer;
  if (verbOf(command) === 'state')
    return 'STATE ' + answer;
  return 'SENT ' + verbOf(command);
}

function missingDriveWindow() {
  return new Error(
      'ui.__driveWindow is not available: the test hooks are off ' +
      '(HERMES_NODE_UI_TEST_HOOKS=1), or this build has no in-process ' +
      'window driver');
}

/**
 * Start the driver for the window titled `title` and wait until it has
 * found it. Resolves with {send, onLost, stop}: send(command) resolves
 * with the answer line; onLost(cb) registers a callback for the driver's
 * asynchronous `LOST ...` report; stop() ends the driver.
 */
function startDriver({ui, driverPath, title}) {
  if (process.platform === 'darwin') {
    if (typeof ui.__driveWindow !== 'function')
      return Promise.reject(missingDriveWindow());
    return Promise.resolve({
      send(command) {
        return Promise.resolve(driveLine(ui, command));
      },
      onLost(callback) {
        void callback;
      },
      stop() {},
    });
  }

  return new Promise((resolve, reject) => {
    const child = spawn(driverPath, [title], {
      env: process.env,
      stdio: ['pipe', 'pipe', 'inherit'],
    });
    const waiting = [];
    const lostCallbacks = [];
    let lost = null;
    let ready = false;
    let buffered = '';
    child.stdout.on('data', (chunk) => {
      buffered += chunk;
      let nl;
      while ((nl = buffered.indexOf('\n')) !== -1) {
        const line = buffered.slice(0, nl);
        buffered = buffered.slice(nl + 1);
        if (!ready && line.startsWith('READY')) {
          ready = true;
          resolve({
            send(command) {
              return new Promise((res) => {
                waiting.push(res);
                child.stdin.write(command + '\n');
              });
            },
            onLost(callback) {
              if (lost !== null)
                callback(lost);
              else
                lostCallbacks.push(callback);
            },
            stop() {
              child.stdin.end();
            },
          });
        } else if (line.startsWith('LOST')) {
          lost = line;
          for (const callback of lostCallbacks.splice(0))
            callback(line);
        } else if (waiting.length > 0) {
          waiting.shift()(line);
        }
      }
    });
    child.on('error', reject);
    child.on('exit', (code) => {
      if (!ready)
        reject(new Error('driver exited with ' + code + ' before READY'));
    });
  });
}

/**
 * One real close request to the window titled `title`, synchronously: on
 * Linux the driver waits for the window, sends WM_DELETE_WINDOW and has
 * exited (XSync'd) when this returns; on macOS `performClose:` has run.
 * Returns true if the request was sent.
 */
function closeOnce({ui, driverPath, title}) {
  if (process.platform === 'darwin') {
    if (typeof ui.__driveWindow !== 'function')
      throw missingDriveWindow();
    return !driveLine(ui, 'close').startsWith('ERROR');
  }
  const r = spawnSync(driverPath, ['--close', title], {stdio: 'inherit'});
  return r.status === 0;
}

module.exports = {startDriver, closeOnce};
