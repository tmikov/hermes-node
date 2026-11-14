// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// Reading the clipboard, with a real status, both ways: the JS Promise
// (`ui.readClipboard()`, answered through the input queue) and the
// synchronous native read (`hnui_read_clipboard_sync`, through the
// reference addon, answered straight into its slot by the platform thread).
//
// The selection's owner is varied with a fixture,
// test/fixtures/hnui-demo/x11_selection_owner.c, run as a child process.
// One owner at a time: each is ended by closing its stdin and its `exit` is
// awaited before the next starts, and the fixture releases the selection
// (and XSyncs) before it exits -- so two owners never race for CLIPBOARD,
// and once one has exited the selection has no owner.
//
//   OWN        our window owns it ('first', written by writeClipboard)
//   FOREIGN    the fixture answers 'second'
//   MUTE       the fixture never answers: a timeout, and in particular
//              NEITHER 'first' NOR 'second' -- Sokol's own getter would
//              have returned its previous buffer here, which is the defect
//              this whole read exists to remove (spec: Clipboard read)
//   FAIL-PENDING  (under MUTE) a Promise read still outstanding is
//              rejected with ERR_CLIPBOARD_FAILED by the test hook standing
//              in for Task 15's shutdown (uiFailPendingClipboardReads), and
//              the platform thread's reply that follows settles nothing
//   LATE       the fixture answers read N only when read N+1's request
//              arrives, with 'reply-1'. Read N times out; read N+1 receives
//              N's SelectionNotify while it waits, recognises it by its
//              timestamp as an earlier conversion's, discards it, and times
//              out itself -- it is NOT answered by N's reply
//   LATE-REFUSE  the same with a late REFUSAL, which names no property, so
//              only its timestamp says it is not this read's: accepting it
//              would report an empty clipboard ('') for read N+1
//   INCR       the fixture answers with an INCR property: too large
//   NONE       nothing owns it: '' and HNUI_UNAVAILABLE
//   ABANDONED  a sync read with a zero deadline times out at once, the
//              platform thread skips (or discards the reply for) it, and
//              the next read still works
//
// The `paste` event reads through the same reader. Sokol emits
// CLIPBOARD_PASTED on X11 for a real Ctrl+V (sokol_app.h:13562-13570), but
// nothing here presses keys on the X server, so the test injects one
// WITHOUT a substituted `clipboard`, which leaves the real read in place: under OWN
// it carries 'first'; under MUTE the read fails and the paste is not
// forwarded at all (PASTE-MUTE: a marker key injected after it arrives,
// and no paste before it).
//
// Each lit run starts its own Xvfb (xvfb-run -a picks a free display
// number), and a selection belongs to one X server, so parallel runs of
// this test do not share a CLIPBOARD. Were they to, another run's owner
// would break OWN, FOREIGN or NONE here.
//
// Mutations (each must turn a named line below red):
//   on timeout, fall back to sapp_get_clipboard_string()  -> MUTE (both)
//   resolve kUnavailable as a rejection                    -> NONE promise
//   paste source back to sapp_get_clipboard_string()       -> PASTE-MUTE
//   accept any CLIPBOARD SelectionNotify regardless of time -> LATE second
//   (it takes N's reply, which names N's property, and fails) and
//   LATE-REFUSE second (it takes the refusal and resolves '')
//   make uiFailPendingClipboardReads a no-op               -> FAIL-PENDING
//   (the platform thread's reply settles it instead, as a timeout)
//   leave the failed reads in the pending map              -> CODE (the
//   late reply settles the already-settled deferred again, and the process
//   dies under ASAN before "reply ignored" could be traced)
// Removing the platform thread's deadline skip is NOT one: the read then
// runs and its reply is discarded, which is the other outcome ABANDONED
// accepts. Both are correct; what ABANDONED pins is that the abandoned
// request neither hangs the waiter nor disturbs the read after it.
//
// REQUIRES: ui, ui-gpu
// REQUIRES: display
//
// RUN-line shape as in ui-events.js: redirections inside `sh -c` because
// xvfb-run merges its child's stderr into stdout, and `timeout` because lit
// has no per-test timeout (dz 01a0d797-e326). Ends through process.exit()
// (Ruling 59): closing the window still hangs (dz 01a0d88a-6518).
//
// RUN: timeout 300 xvfb-run -a sh -c 'HERMES_NODE_UI_TEST_HOOKS=1 HERMES_NODE_DEBUG_NATIVE=UI %hermes-node-ui %s %hnui_demo_addon %x11_selection_owner > %t.out 2> %t.err'; echo "EXITCODE $?" | %FileCheck --check-prefix=CODE %s
// RUN: %FileCheck %s < %t.out
// RUN: %FileCheck --check-prefix=TRACE %s < %t.err

// CODE: EXITCODE 0

// CHECK: NO-WINDOW ok
// CHECK: OWN promise ok
// CHECK: OWN sync ok
// CHECK: PASTE-OWN ok
// CHECK: FOREIGN promise ok
// CHECK: FOREIGN sync ok
// CHECK: MUTE promise ok
// CHECK: FAIL-PENDING ok
// CHECK: MUTE sync ok
// CHECK: PASTE-MUTE ok
// CHECK: LATE first ok
// CHECK: LATE second ok
// CHECK: LATE-REFUSE first ok
// CHECK: LATE-REFUSE second ok
// CHECK: FAIL-PENDING settled once ok
// CHECK: INCR promise ok
// CHECK: INCR sync ok
// CHECK: NONE promise ok
// CHECK: NONE sync ok
// CHECK: ABANDONED sync ok
// CHECK: ABANDONED after ok
// CHECK: DONE

// What the platform thread actually did, in order. `#N` is the request id
// within its own path (Promise ids and sync ids are separate spaces).
// TRACE: clipboard read: own selection
// TRACE: clipboard read promise #1: status=ok bytes=5
// TRACE: clipboard read: own selection
// TRACE: clipboard read sync #1: status=ok bytes=5
// TRACE: clipboard read: own selection
// TRACE: clipboard read: 6 bytes from the owner
// TRACE: clipboard read promise #2: status=ok bytes=6
// TRACE: clipboard read: 6 bytes from the owner
// TRACE: clipboard read sync #2: status=ok bytes=6
// TRACE: clipboard read: no SelectionNotify within {{[0-9]+}} ms; timed out
// TRACE: clipboard read promise #3: status=timeout bytes=0
// FAIL-PENDING: the JS thread's rejection and the platform thread's read of
// #4 run concurrently, so only their order relative to the lines around
// them is fixed. The ignored reply is delivered once the JS thread is
// free, which is after the sync read behind #4 returns.
// TRACE-DAG: clipboard read promise #4: failed by uiFailPendingClipboardReads
// TRACE-DAG: clipboard read promise #4: status=timeout bytes=0
// TRACE: clipboard read: no SelectionNotify within 2000 ms; timed out
// TRACE: clipboard read sync #3: status=timeout bytes=0
// TRACE: clipboard read promise #4: no such pending read; reply ignored
// TRACE: clipboard read: no SelectionNotify within 1000 ms; timed out
// TRACE: paste: clipboard read timeout; not forwarded
// TRACE: clipboard read promise #5: status=timeout bytes=0
// TRACE: clipboard read: discarded a late SelectionNotify (time {{[0-9]+}}, this read's {{[0-9]+}})
// TRACE: clipboard read: no SelectionNotify within {{[0-9]+}} ms; timed out
// TRACE: clipboard read promise #6: status=timeout bytes=0
// TRACE: clipboard read promise #7: status=timeout bytes=0
// TRACE: clipboard read: discarded a late SelectionNotify (time {{[0-9]+}}, this read's {{[0-9]+}})
// TRACE: clipboard read: no SelectionNotify within {{[0-9]+}} ms; timed out
// TRACE: clipboard read promise #8: status=timeout bytes=0
// TRACE: clipboard read: INCR reply; too large
// TRACE: clipboard read promise #9: status=too-large bytes=0
// TRACE: clipboard read: INCR reply; too large
// TRACE: clipboard read sync #4: status=too-large bytes=0
// TRACE: clipboard read: no owner
// TRACE: clipboard read promise #10: status=unavailable bytes=0
// TRACE: clipboard read: no owner
// TRACE: clipboard read sync #5: status=unavailable bytes=0
// TRACE: clipboard read sync #6: {{deadline passed; skipped|waiter gone; reply discarded}}
// TRACE: clipboard read: own selection
// TRACE: clipboard read sync #7: status=ok bytes=5

'use strict';

const {spawn} = require('child_process');
const ui = require('hermes-node:ui');

const addonPath = process.argv[2];
const ownerPath = process.argv[3];

// Long enough that a responsive owner is never mistaken for a slow one
// under a 16-way parallel ASAN run.
const SYNC_TIMEOUT_MS = 3000;
// LONGER than the platform thread's own cap on a selection wait
// (kClipboardReadCapMs, 2000 ms, in ui_module.cpp), deliberately: the
// platform thread then gives up first and its answer is the one this read
// receives. With a shorter deadline the waiter gives up first, the
// platform's answer is discarded, and the sync half of MUTE would pass
// whatever that answer said -- including a stale 'first'.
//
// 5000 rather than just over the cap because, in MUTE, this read queues
// behind FAIL-PENDING's Promise read, which holds the platform thread for
// its own 1000 ms first.
const MUTE_SYNC_TIMEOUT_MS = 5000;

function report(name, ok, detail) {
  console.log(name + ' ' + (ok ? 'ok' : 'WRONG ' + (detail || '')));
}

/// Start an owner and wait for READY. Resolves with a handle whose
/// `stop()` closes the owner's stdin and waits for it to exit.
function startOwner(args) {
  return new Promise((resolve, reject) => {
    const child = spawn(ownerPath, args, {
      env: process.env,
      stdio: ['pipe', 'pipe', 'inherit'],
    });
    const exited = new Promise((res) => child.on('exit', res));
    let out = '';
    let ready = false;
    child.stdout.on('data', (chunk) => {
      out += chunk;
      if (!ready && out.includes('READY\n')) {
        ready = true;
        resolve({
          stop() {
            child.stdin.end();
            return exited;
          },
        });
      }
    });
    child.on('error', reject);
    child.on('exit', (code) => {
      if (!ready)
        reject(new Error('owner exited with ' + code + ' before READY'));
    });
  });
}

async function readPromise() {
  try {
    return {resolved: true, value: await ui.readClipboard()};
  } catch (e) {
    return {resolved: false, code: e && e.code, error: e};
  }
}

function show(x) {
  return JSON.stringify(x);
}

const SAPP_KEYCODE_F3 = 292;

/// Inject a CLIPBOARD_PASTED with no substituted clipboard, then a marker
/// key. Resolves with the paste texts seen before the marker arrived: the
/// platform thread handles the two in order, so a paste that was going to
/// be forwarded has been delivered by then.
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

  // Before any window has been requested nothing would drain the command,
  // so the Promise could never settle: refused.
  let early = null;
  try {
    ui.readClipboard();
  } catch (e) {
    early = e;
  }
  report(
      'NO-WINDOW', early instanceof Error && /createWindow/.test(early.message),
      String(early));

  const win =
      await ui.createWindow({width: 160, height: 120, title: 'clipboard test'});

  // OWN
  ui.writeClipboard('first');
  let p = await readPromise();
  report('OWN promise', p.resolved && p.value === 'first', show(p));
  let s = addon.readClipboardSync(SYNC_TIMEOUT_MS);
  report('OWN sync', s.status === 'HNUI_OK' && s.text === 'first', show(s));
  let pastes = await injectPaste(win);
  report(
      'PASTE-OWN', pastes.length === 1 && pastes[0] === 'first', show(pastes));

  // FOREIGN
  let owner = await startOwner(['text', 'second']);
  p = await readPromise();
  report('FOREIGN promise', p.resolved && p.value === 'second', show(p));
  s = addon.readClipboardSync(SYNC_TIMEOUT_MS);
  report(
      'FOREIGN sync', s.status === 'HNUI_OK' && s.text === 'second', show(s));
  await owner.stop();

  // MUTE: the one the spec names. A failed read must not hand back an
  // earlier read's text, ours or anyone's.
  owner = await startOwner(['mute']);
  p = await readPromise();
  report(
      'MUTE promise',
      !p.resolved && p.code === 'ERR_CLIPBOARD_TIMEOUT' &&
          p.value !== 'first' && p.value !== 'second',
      show(p));
  // FAIL-PENDING: a read the platform thread will answer (with a timeout,
  // a second from now) is failed first. The sync read below waits behind it
  // on the platform thread, so by the time it returns the late reply is
  // queued for delivery.
  let failSettled = 0;
  const failOutcome = ui.readClipboard().then(
      (value) => {
        ++failSettled;
        return {resolved: true, value};
      },
      (e) => {
        ++failSettled;
        return {resolved: false, code: e && e.code};
      });
  ui.__failPendingClipboardReads();
  const failed = await failOutcome;
  report(
      'FAIL-PENDING',
      !failed.resolved && failed.code === 'ERR_CLIPBOARD_FAILED' &&
          failSettled === 1,
      show(failed));

  s = addon.readClipboardSync(MUTE_SYNC_TIMEOUT_MS);
  report(
      'MUTE sync',
      s.status === 'HNUI_TIMEOUT' && s.text === null && s.text !== 'first' &&
          s.text !== 'second',
      show(s));
  pastes = await injectPaste(win);
  report('PASTE-MUTE', pastes.length === 0, show(pastes));
  await owner.stop();

  // LATE
  owner = await startOwner(['late']);
  p = await readPromise();
  report(
      'LATE first', !p.resolved && p.code === 'ERR_CLIPBOARD_TIMEOUT', show(p));
  p = await readPromise();
  report(
      'LATE second',
      !p.resolved && p.code === 'ERR_CLIPBOARD_TIMEOUT' &&
          p.value !== 'reply-1',
      show(p));
  await owner.stop();

  // LATE-REFUSE
  owner = await startOwner(['late-refuse']);
  p = await readPromise();
  report(
      'LATE-REFUSE first', !p.resolved && p.code === 'ERR_CLIPBOARD_TIMEOUT',
      show(p));
  p = await readPromise();
  report(
      'LATE-REFUSE second',
      !p.resolved && p.code === 'ERR_CLIPBOARD_TIMEOUT', show(p));
  await owner.stop();

  // By now FAIL-PENDING's late reply has long been delivered; it must not
  // have settled the Promise a second time.
  report('FAIL-PENDING settled once', failSettled === 1, String(failSettled));

  // INCR
  owner = await startOwner(['incr']);
  p = await readPromise();
  report(
      'INCR promise', !p.resolved && p.code === 'ERR_CLIPBOARD_TOO_LARGE',
      show(p));
  s = addon.readClipboardSync(SYNC_TIMEOUT_MS);
  report(
      'INCR sync', s.status === 'HNUI_TOO_LARGE' && s.text === null, show(s));
  await owner.stop();

  // NONE: the last owner released the selection and exited.
  p = await readPromise();
  report('NONE promise', p.resolved && p.value === '', show(p));
  s = addon.readClipboardSync(SYNC_TIMEOUT_MS);
  report(
      'NONE sync', s.status === 'HNUI_UNAVAILABLE' && s.text === null,
      show(s));

  // ABANDONED: a zero deadline has passed before the platform thread can
  // reach the request, and the waiter does not wait.
  s = addon.readClipboardSync(0);
  report(
      'ABANDONED sync', s.status === 'HNUI_TIMEOUT' && s.text === null,
      show(s));
  ui.writeClipboard('third');
  s = addon.readClipboardSync(SYNC_TIMEOUT_MS);
  report(
      'ABANDONED after', s.status === 'HNUI_OK' && s.text === 'third',
      show(s));

  console.log('DONE');
  process.exit(0);
}

main().catch((e) => {
  console.log('FAILED ' + (e && e.stack ? e.stack : e));
  process.exit(1);
});
