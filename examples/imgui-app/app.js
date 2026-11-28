// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// A docked, multi-panel ImGui application written in JavaScript against
// the hermes-node:imgui binding, with panels backed by Node's fs, os and
// process APIs. Run it with hermes-node-ui (see README.md):
//
//   hermes-node-ui examples/imgui-app/app.js [folder]
//
// This file is the shell: the window, the frame, the main menu, the dock
// space and its default layout, and the two modals. Each panel is a module
// under panels/ exporting {name, open, draw(ctx)}; `open` is a reference
// array, so the window's close button and the View menu both write it.
//
// createApp(options) is also what test/ui-imgui-app.js drives: with
// `driven: true` the application registers no frame callback and the
// caller runs app.frame(), and with `clock: 'fixed'` the time the canvas
// animates by is set with app.setTime().

'use strict';

const nodePath = require('path');
const layout = require('./layout.js');
const {createViewer} = require('./panels/viewer.js');
const files = require('./panels/files.js');
const widgets = require('./panels/widgets.js');
const monitor = require('./panels/monitor.js');
const canvas = require('./panels/canvas.js');
const logPanel = require('./panels/log.js');

const TITLE = 'hermes-node ImGui';

/**
 * Open the window and build the application. Resolves with the app object
 * once the window exists and the default layout is loaded; no frame has
 * been drawn yet.
 */
async function createApp(options = {}) {
  const {
    root = process.cwd(),
    fs = require('fs').promises,
    clock = null,
    driven = false,
  } = options;
  if (clock !== null && clock !== 'fixed')
    throw new TypeError('createApp(): clock must be null or \'fixed\'');

  const ui = require('hermes-node:ui');
  const win = await ui.createWindow(
      {title: TITLE, width: 1280, height: 800, highDpi: true});
  // After the window exists: the addon finds its window when loaded.
  const imgui = require('hermes-node:imgui');

  // Promises of file and directory requests still in flight (app.idle()).
  const inFlight = new Set();
  function track(promise) {
    inFlight.add(promise);
    const done = () => inFlight.delete(promise);
    promise.then(done, (e) => {
      done();
      throw e;
    });
    return promise;
  }

  const log = (text) => logPanel.add(text);
  const viewer = createViewer({fs, log});
  const panels = {files, viewer, widgets, monitor, canvas, log: logPanel};
  const panelList = [files, viewer, widgets, monitor, canvas, logPanel];

  const rects = new Map();
  /**
   * Remember the item just drawn under `key`, for app.rects: a test's
   * read-back, so only when `driven` (in normal use it would cost two
   * binding calls per row for nothing). The map is emptied at the start of
   * every frame, so it holds exactly what the last frame drew.
   */
  function recordRect(key) {
    if (!driven)
      return;
    let r = rects.get(key);
    if (r === undefined) {
      r = {min: [0, 0], max: [0, 0]};
      rects.set(key, r);
    }
    imgui.getItemRectMin(r.min);
    imgui.getItemRectMax(r.max);
  }

  const state = {
    root: nodePath.resolve(root),
    fs,
    viewer,
    recordRect,
    track,
  };
  const ctx = {imgui, ui, win, log, state, time: 0, testMode: driven};

  let fixedTime = clock === 'fixed' ? 0 : null;
  let pendingLayout = null;
  let lastFrameMs = 0;
  let frameCount = 0;
  let styleName = 'dark';
  const showDemo = [false];
  const showMetrics = [false];
  let openFolderRequested = false;
  let aboutRequested = false;
  const folderInput = [''];

  files.setRoot(state.root);
  // Outside any frame, before the first: loadIniSettingsFromMemory throws
  // inside one. The windows are docked from the first frame and tile from
  // the second (docking lays out what was visible in the previous frame).
  imgui.loadIniSettingsFromMemory(layout.DEFAULT);
  monitor.start(() => ({fps: imgui.getFrameRate(), frameMs: lastFrameMs}));
  log('started in ' + state.root);

  function setRoot(dir) {
    state.root = nodePath.resolve(dir);
    files.setRoot(state.root);
    log('folder ' + state.root);
  }

  /** The default view again: every panel open, in the default layout. */
  function resetLayout() {
    for (const p of panelList)
      p.open[0] = true;
    // The layout is applied by the next frame() before its newFrame():
    // this is called from the View menu, inside a frame.
    pendingLayout = layout.DEFAULT;
    log('layout reset');
  }

  function quit() {
    win.close();
  }

  function menu(label) {
    const open = imgui.beginMenu(label);
    recordRect(label);
    return open;
  }

  function item(label, shortcut, selected) {
    const clicked = imgui.menuItem(label, shortcut, selected);
    recordRect(label);
    return clicked;
  }

  function drawMainMenu() {
    if (!imgui.beginMainMenuBar())
      return;
    if (menu('File')) {
      if (item('Open Folder...'))
        openFolderRequested = true;
      imgui.separator();
      if (item('Quit'))
        quit();
      imgui.endMenu();
    }
    if (menu('View')) {
      for (const p of panelList)
        item(p.name, null, p.open);
      imgui.separator();
      if (item('Reset Layout'))
        resetLayout();
      imgui.separator();
      if (item('Dark', null, styleName === 'dark')) {
        imgui.styleColorsDark();
        styleName = 'dark';
      }
      if (item('Light', null, styleName === 'light')) {
        imgui.styleColorsLight();
        styleName = 'light';
      }
      imgui.separator();
      item('ImGui Demo', null, showDemo);
      item('ImGui Metrics', null, showMetrics);
      imgui.endMenu();
    }
    if (menu('Help')) {
      if (item('About'))
        aboutRequested = true;
      imgui.endMenu();
    }
    imgui.endMainMenuBar();
  }

  function drawModals() {
    // Opened here, at the same ID-stack level as the beginPopupModal()
    // below, not from inside the menu.
    if (openFolderRequested) {
      openFolderRequested = false;
      folderInput[0] = state.root;
      imgui.openPopup('Open Folder');
    }
    if (aboutRequested) {
      aboutRequested = false;
      imgui.openPopup('About');
    }
    if (imgui.beginPopupModal('Open Folder', null,
            imgui.WindowFlags_AlwaysAutoResize)) {
      imgui.text('Folder to show in Files:');
      imgui.setNextItemWidth(480);
      const entered = imgui.inputText('##folder', folderInput, 4096,
          imgui.InputTextFlags_EnterReturnsTrue);
      if (imgui.button('Open') || entered) {
        setRoot(folderInput[0]);
        imgui.closeCurrentPopup();
      }
      imgui.sameLine();
      if (imgui.button('Cancel'))
        imgui.closeCurrentPopup();
      imgui.endPopup();
    }
    if (imgui.beginPopupModal('About', null,
            imgui.WindowFlags_AlwaysAutoResize)) {
      imgui.text(TITLE + ': an ImGui application in JavaScript');
      imgui.text('Dear ImGui ' + imgui.version);
      imgui.separatorText('process.versions');
      if (imgui.beginTable('versions', 2,
              imgui.TableFlags_Borders | imgui.TableFlags_RowBg |
                  imgui.TableFlags_SizingFixedFit)) {
        for (const [name, version] of Object.entries(process.versions)) {
          imgui.tableNextRow();
          imgui.tableNextColumn();
          imgui.text(name);
          imgui.tableNextColumn();
          imgui.text(String(version));
        }
        imgui.endTable();
      }
      if (imgui.button('Close'))
        imgui.closeCurrentPopup();
      imgui.endPopup();
    }
  }

  /**
   * One whole frame, newFrame() to render(); returns what render()
   * returned. `timestamp` is the frame callback's, in milliseconds.
   */
  function frame(timestamp) {
    const start = process.hrtime.bigint();
    ++frameCount;
    rects.clear();
    if (pendingLayout !== null) {
      imgui.loadIniSettingsFromMemory(pendingLayout);
      pendingLayout = null;
    }
    if (fixedTime !== null)
      ctx.time = fixedTime;
    else
      ctx.time = (typeof timestamp === 'number' ? timestamp :
                                                  process.uptime() * 1000) /
          1000;
    imgui.newFrame();
    drawMainMenu();
    imgui.dockSpaceOverViewport();
    for (const p of panelList) {
      if (!p.open[0])
        continue;
      if (imgui.begin(p.name, p.open, p.windowFlags || 0))
        p.draw(ctx);
      imgui.end();
    }
    if (showDemo[0])
      imgui.showDemoWindow(showDemo);
    if (showMetrics[0])
      imgui.showMetricsWindow(showMetrics);
    drawModals();
    const drew = imgui.render();
    lastFrameMs = Number(process.hrtime.bigint() - start) / 1e6;
    return drew;
  }

  const app = {
    ui,
    win,
    imgui,
    frame,
    rects,
    panels,
    scale: win.dpiScale,
    setTime(seconds) {
      if (fixedTime === null)
        throw new Error('app.setTime(): the clock is not \'fixed\'');
      fixedTime = seconds;
    },
    resetLayout,
    quit,
    async idle() {
      while (inFlight.size > 0)
        await Promise.allSettled([...inFlight]);
    },
    /** How many times frame() has been called (harness.adopt checks it). */
    get frameCount() {
      return frameCount;
    },
    /** The JavaScript time the last whole frame took, in milliseconds. */
    get lastFrameMs() {
      return lastFrameMs;
    },
  };

  if (!driven) {
    const loop = (timestamp) => {
      // Registered first, so a frame that throws (and is survived) does
      // not stop the loop.
      ui.requestAnimationFrame(loop);
      frame(timestamp);
    };
    ui.requestAnimationFrame(loop);
  }
  return app;
}

module.exports = {createApp};

if (require.main === module) {
  createApp({root: process.argv[2] || process.cwd()}).catch((e) => {
    console.error(e);
    process.exit(1);
  });
}
