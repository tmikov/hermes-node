// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// The reference addon's entry script, and **a smoke test a person can
// run** (spec: The reference addon, job 1):
//
//   hermes-node-ui test/fixtures/hnui-demo/demo.js <addon path> [mode]
//
// With no mode it shows a window with a rotating triangle, drawn by the
// addon with sokol_gl; pressing any key toggles the background between
// white and blue. Close the window to quit (or press Ctrl+C). The modes:
//
//   js       (the default) the triangle, driven by an ordinary JavaScript
//            requestAnimationFrame loop that calls into the addon -- the
//            common shape, so it is the one demonstrated by default.
//   native   the same triangle, with the loop handed to the addon itself
//            through hnui_request_animation_frame: the path an ImGui-style
//            renderer would use. JavaScript is not involved in any frame
//            after the first call.
//   pattern  the addon's fixed pattern -- a blue surface with a yellow
//            top-left quadrant -- which is what the capture tests draw
//            (test/ui-present.js), because a rotating picture cannot be
//            compared.
//   conformance
//            textual, and for lit rather than for a person (spec: "This
//            mode is textual and runs under lit"): calls each hnui_* entry
//            point the visual demo does not, and prints one line per
//            observation. It needs HERMES_NODE_UI_TEST_HOOKS=1, because it
//            injects its own events and captures its own frame, and two
//            more arguments, and the GPU backend:
//              demo.js <addon> conformance <x11_selection_owner> <out.ppm>
//                  <gl|metal>
//            test/ui-conformance.js runs it, passing %ui-backend. On `gl`
//            nothing is skipped.
//
// This file stays free of test scaffolding in the visual modes:
// test/ui-demo-visual.js starts them through startDemo() below and adds its
// frozen rotation, injected key and captures from the outside.

'use strict';

const ui = require('hermes-node:ui');

const VISUAL_MODES = ['js', 'native', 'pattern'];

/**
 * Create the window, load the addon and start the loop for \p mode, one of
 * VISUAL_MODES. Resolves with `{win, addon}`.
 */
async function startDemo(addonPath, mode) {
  const win = await ui.createWindow({
    width: 640,
    height: 480,
    title: 'hermes-node-ui demo',
  });
  console.log(
      'WINDOW ' + win.framebufferWidth + 'x' + win.framebufferHeight);

  // Loaded AFTER the window exists, deliberately: hnui_get_window(env)
  // answers null until then, and an addon that cached that null at load
  // time would never find the window. Saying so here is cheaper than
  // every addon discovering it. Resolved first, because require() would
  // read a relative path such as `build/hnui_demo_addon.node` as a package
  // name.
  const addon = require(require('path').resolve(addonPath));
  console.log('ADDON hasWindow=' + addon.hasWindow());

  if (mode === 'pattern') {
    const frame = () => {
      addon.drawFixedPattern();
      ui.requestAnimationFrame(frame);
    };
    ui.requestAnimationFrame(frame);
  } else {
    // The key listener, registered natively through
    // hnui_add_event_listener -- the input path end to end.
    addon.startDemo();
    if (mode === 'native') {
      // The addon re-registers itself each tick, so one call starts the
      // loop.
      addon.startNativeDemoFrames();
    } else {
      const frame = (timestamp) => {
        addon.drawDemoFrame(timestamp);
        ui.requestAnimationFrame(frame);
      };
      ui.requestAnimationFrame(frame);
    }
  }
  console.log('MODE ' + mode);
  return {win, addon};
}

// --- Conformance mode --------------------------------------------------

/** Parse the binary PPM (P6) the capture hook writes. */
function readPpm(file) {
  const buf = require('fs').readFileSync(file);
  let offset = 0;
  function token() {
    while (offset < buf.length && /\s/.test(String.fromCharCode(buf[offset])))
      offset++;
    const start = offset;
    while (offset < buf.length && !/\s/.test(String.fromCharCode(buf[offset])))
      offset++;
    return buf.toString('ascii', start, offset);
  }
  const magic = token();
  const width = parseInt(token(), 10);
  const height = parseInt(token(), 10);
  const maxValue = parseInt(token(), 10);
  offset++;
  if (magic !== 'P6' || maxValue !== 255)
    throw new Error('not a P6/255 PPM: ' + magic + ' ' + maxValue);
  return {width, height, data: buf.slice(offset)};
}

function pixel(image, x, y) {
  const i = (y * image.width + x) * 3;
  return '(' + image.data[i] + ',' + image.data[i + 1] + ',' +
      image.data[i + 2] + ')';
}

/**
 * Start the x11_selection_owner fixture with \p args and wait for READY;
 * the handle's stop() closes its stdin and waits for it to exit (the shape
 * test/ui-clipboard.js uses, for the reasons given there).
 */
function startOwner(ownerPath, args) {
  const {spawn} = require('child_process');
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

const SAPP_KEYCODE_F4 = 293;
const SAPP_KEYCODE_F5 = 294;
// Long enough that a responsive answer is never mistaken for a slow one
// under a parallel ASAN run, and longer than the platform thread's own cap
// on a selection wait, so that for the owner that never answers the
// timeout reported is the platform thread's.
const CLIPBOARD_TIMEOUT_MS = 3000;

async function runConformance(addonPath, ownerPath, capturePath, backend) {
  if (typeof ui.__injectEvent !== 'function' || !ownerPath || !capturePath ||
      (backend !== 'gl' && backend !== 'metal')) {
    console.log(
        'conformance mode needs HERMES_NODE_UI_TEST_HOOKS=1 and three more ' +
        'arguments: <x11_selection_owner> <capture.ppm> <gl|metal>');
    process.exit(2);
  }
  const win = await ui.createWindow(
      {width: 320, height: 240, title: 'hnui conformance'});
  const addon = require(require('path').resolve(addonPath));
  const nextFrame = () => new Promise((r) => ui.requestAnimationFrame(r));
  const print = (lines) => {
    for (const line of [].concat(lines))
      console.log(line);
  };

  print(addon.conformanceApiVersion());
  print(addon.conformanceWindowState());
  print(
      'hnui_get_window_state js mirror width=' + win.width + ' height=' +
      win.height + ' framebuffer=' + win.framebufferWidth + 'x' +
      win.framebufferHeight + ' focused=' + (win.focused ? 1 : 0));

  // The MSAA frame comes first, so that it is publication 1: nothing has
  // drawn before it. The capture request is installed before the frame is.
  const captured = ui.__captureFrame({
    path: capturePath,
    publicationId: 1,
    giveUpAfterFrames: 900,
    generation: 1,
    targetWidth: win.framebufferWidth,
    targetHeight: win.framebufferHeight,
    framebufferWidth: win.framebufferWidth,
    framebufferHeight: win.framebufferHeight,
    dpiScale: win.dpiScale,
  });
  addon.conformanceRequestMsaaFrame();
  const capture = await captured;
  print(addon.conformanceTakeLines());
  print(
      'hnui_frame_target capture matched=' + capture.matched +
      ' publication=' + capture.publicationId);
  if (capture.matched) {
    const image = readPpm(capturePath);
    const m = 8;
    const right = image.width - 1 - m;
    const bottom = image.height - 1 - m;
    print(
        'hnui_frame_target capture top-left=' + pixel(image, m, m) +
        ' top-right=' + pixel(image, right, m) +
        ' bottom-left=' + pixel(image, m, bottom) +
        ' bottom-right=' + pixel(image, right, bottom) +
        ' centre=' + pixel(image, image.width >> 1, image.height >> 1));
  }

  // Cancellation: the native callbacks run in the next tick, ahead of this
  // later-registered JS one.
  print(addon.conformanceStartCancel());
  await nextFrame();
  print(addon.conformanceCancelResult());

  // Removal during dispatch. The native listeners run before any JS one,
  // so once JS has seen the second key both have been delivered natively.
  addon.conformanceStartListeners();
  await new Promise((resolve) => {
    const onKey = (e) => {
      if (e.keyCode !== SAPP_KEYCODE_F5)
        return;
      win.off('keydown', onKey);
      resolve();
    };
    win.on('keydown', onKey);
    ui.__injectEvent({type: 'KEY_DOWN', keyCode: SAPP_KEYCODE_F4});
    ui.__injectEvent({type: 'KEY_DOWN', keyCode: SAPP_KEYCODE_F5});
  });
  addon.conformanceRequestQuiescenceFrame();
  await nextFrame();
  print(addon.conformanceTakeLines());

  // hnui_cancel_close: false outside any event, true from a native listener
  // during a close request -- which it then cancels, so the run carries on.
  addon.conformanceStartCloseCancel();
  await new Promise((resolve) => {
    win.once('close', resolve);
    ui.__injectEvent({type: 'QUIT_REQUESTED'});
  });
  print(addon.conformanceCloseCancelResult());

  print(addon.conformanceCommands());

  // A normal read, from our own selection; then a timeout. On X11 the
  // timeout is reached against an owner that takes the selection and never
  // answers. The macOS pasteboard has no owner to go mute, so the Metal
  // backend reaches it by a zero deadline instead (Task 16e).
  print(addon.conformanceWriteClipboard('hnui conformance'));
  print(addon.conformanceReadClipboard(CLIPBOARD_TIMEOUT_MS, ''));
  if (backend === 'gl') {
    const owner = await startOwner(ownerPath, ['mute']);
    print(addon.conformanceReadClipboard(
        CLIPBOARD_TIMEOUT_MS, ' (owner never answers)'));
    await owner.stop();
  }

  print('DONE');
  // Ends the way a program would: closing the window ends the process, with
  // process.exitCode -- unset here -- as its status.
  win.close();
}

module.exports = {startDemo};

if (require.main === module) {
  const addonPath = process.argv[2];
  const mode = process.argv[3] || 'js';
  if (!addonPath) {
    console.log(
        'usage: hermes-node-ui demo.js <path to hnui_demo_addon.node> ' +
        '[js|native|pattern|conformance]');
    process.exit(2);
  }
  let started;
  if (mode === 'conformance') {
    started = runConformance(
        addonPath, process.argv[4], process.argv[5], process.argv[6]);
  } else if (VISUAL_MODES.includes(mode)) {
    started = startDemo(addonPath, mode);
  } else {
    console.log(
        'unknown mode: ' + mode + ' (expected js, native, pattern or ' +
        'conformance)');
    process.exit(2);
  }
  started.catch((e) => {
    console.log('FAILED ' + (e && e.stack ? e.stack : e));
    process.exit(1);
  });
}
