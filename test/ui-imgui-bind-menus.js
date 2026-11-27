// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// The Menus group: the main menu bar, a menu opened by a click, menu items
// returning true on a click in both forms, and a window's own menu bar;
// and the group's coverage. Each menu, and the main menu bar, is an ImGui
// window of its own, so `__test.state().windows` is the read-back of what
// is showing: `##MainMenuBar` (imgui_widgets.cpp:9466) and
// `<label>###Menu_<depth>` (BeginPopupMenuEx, imgui.cpp:13466).
//
// Cases, in the order they run, one output line each (`<CASE> ok`; any
// failure prints its own line and exits 1):
//
//  MAINBAR   beginMainMenuBar() returns true and `##MainMenuBar` is
//            active; beginMenu('File') returns false, and no `File###Menu_00`
//            window is active, until a click on it.
//  CLOSERS   with the menu open, the current window (getWindowPos()) after
//            endMenu() is the main menu bar again, and after
//            endMainMenuBar() the window current before beginMainMenuBar();
//            the menu and the bar are each at a position of their own.
//  MENUITEM  in the open menu, menuItem('Open', 'Ctrl+O', false) returns
//            true on a click on it, and the menu closes (its window is no
//            longer active).
//  MENUREF   menuItem('Check', null, p) with p [false], in the menu opened
//            again: a click sets p[0] to true, and it returned true.
//  MENUBAR   beginMenuBar() in a window with WindowFlags_MenuBar returns
//            true, and endMenuBar() is accepted; in a window without that
//            flag it returns false.
//  COVERAGE  every Menus function has reached ImGui.
//
// THE MUTATIONS THIS MUST BREAK (recorded in progress-imgui-binding.md):
//  ImGui::BeginMenu() replaced by false, enterImGui() kept: MAINBAR fails;
//  enterImGui() removed from endMainMenuBar: COVERAGE fails;
//  menuItem(label, shortcut, p)'s write-back removed: MENUREF fails;
//  ImGui::EndMenu() or ImGui::EndMainMenuBar() removed, enterImGui()
//  kept: CLOSERS fails.
//
// REQUIRES: ui, ui-gpu, ui-imgui
// REQUIRES: display
//
// RUN: %with-display 300 sh -c 'HERMES_NODE_UI_TEST_HOOKS=1 HERMES_NODE_DEBUG_NATIVE=UI %hermes-node-ui %s > %t.out 2> %t.err'; echo "EXITCODE $?" | %FileCheck --check-prefix=CODE %s
// RUN: %FileCheck %s < %t.out
// RUN: %FileCheck --implicit-check-not='[sapp:error' --implicit-check-not='[sokol-' --implicit-check-not='imgui: usage error' --check-prefix=TRACE %s < %t.err

'use strict';

const harness = require('./fixtures/imgui/harness.js');

const MENU = 'File###Menu_00';

function fail(line) {
  console.log(line);
  process.exit(1);
}

function active(imgui, name) {
  return imgui.__test.state().windows.some((w) => w.name === name && w.active);
}

async function main() {
  const h = await harness.open();
  const {imgui} = h;
  const rect = () => harness.rect(imgui);

  // The main menu bar, its one menu, and two items in it. What each call
  // returned is accumulated, since a click's frame is somewhere inside the
  // harness's waits.
  const p = [false];
  let barShown = false, menuOpen = false;
  let menuRect = null, openRect = null, checkRect = null;
  let openPresses = 0, checkPresses = 0;
  // getWindowPos() of the current window around each closer: the window
  // that endMenu() and endMainMenuBar() return to is read back.
  const pos = {};
  const draw = () => {
    pos.before = imgui.getWindowPos();
    barShown = imgui.beginMainMenuBar();
    if (!barShown)
      return;
    pos.inBar = imgui.getWindowPos();
    menuOpen = imgui.beginMenu('File');
    menuRect = rect();
    if (menuOpen) {
      pos.inMenu = imgui.getWindowPos();
      if (imgui.menuItem('Open', 'Ctrl+O', false))
        ++openPresses;
      openRect = rect();
      if (imgui.menuItem('Check', null, p))
        ++checkPresses;
      checkRect = rect();
      imgui.endMenu();
      pos.afterMenu = imgui.getWindowPos();
    }
    imgui.endMainMenuBar();
    pos.afterBar = imgui.getWindowPos();
  };

  async function openMenu(label) {
    await harness.until(h, draw, () => menuRect !== null, label + ' drawn');
    await harness.clickAt(h, draw, harness.center(menuRect));
    await harness.until(h, draw, () => menuOpen && active(imgui, MENU),
                        label + ' open');
    // One more frame, so that the items' rectangles are the open menu's.
    await h.run(draw);
  }

  // --- MAINBAR ---------------------------------------------------------
  {
    await h.run(draw);
    await h.run(draw);
    const bar = active(imgui, '##MainMenuBar');
    const closedBefore = !menuOpen && !active(imgui, MENU);
    await openMenu('MAINBAR');
    const ok = barShown && bar && closedBefore;
    console.log(ok ? 'MAINBAR ok' :
                     'MAINBAR WRONG ' + barShown + ' ' + bar + ' ' +
                         closedBefore);
  }

  // --- CLOSERS ---------------------------------------------------------
  {
    // The last frame drew the open menu. The menu is a window of its own,
    // placed below the bar; endMenu() returns to the bar, and
    // endMainMenuBar() to the window current before beginMainMenuBar().
    const same = (a, b) => a[0] === b[0] && a[1] === b[1];
    const ok = !same(pos.inMenu, pos.inBar) && same(pos.afterMenu, pos.inBar) &&
        !same(pos.inBar, pos.before) && same(pos.afterBar, pos.before);
    console.log(ok ? 'CLOSERS ok' : 'CLOSERS WRONG ' + JSON.stringify(pos));
  }

  // --- MENUITEM --------------------------------------------------------
  {
    await harness.clickAt(h, draw, harness.center(openRect));
    await harness.until(h, draw, () => !menuOpen && !active(imgui, MENU),
                        'MENUITEM closed');
    console.log(openPresses === 1 && checkPresses === 0 ?
                    'MENUITEM ok' :
                    'MENUITEM WRONG ' + openPresses + ' ' + checkPresses);
  }

  // --- MENUREF ---------------------------------------------------------
  {
    await openMenu('MENUREF');
    await harness.clickAt(h, draw, harness.center(checkRect));
    await harness.until(h, draw, () => p[0] === true, 'MENUREF checked');
    console.log(checkPresses === 1 && openPresses === 1 ?
                    'MENUREF ok' :
                    'MENUREF WRONG ' + checkPresses + ' ' + openPresses);
  }

  // --- MENUBAR ---------------------------------------------------------
  {
    let withFlag, withoutFlag, ended = 0;
    await h.run(() => {
      imgui.setNextWindowPos([40, 60]);
      imgui.setNextWindowSize([300, 200]);
      imgui.begin('with menu bar', null, imgui.WindowFlags_MenuBar);
      withFlag = imgui.beginMenuBar();
      if (withFlag) {
        imgui.text('in the bar');
        const before = harness.calls(h, 'endMenuBar');
        imgui.endMenuBar();
        ended = harness.calls(h, 'endMenuBar') - before;
      }
      imgui.end();
      imgui.setNextWindowPos([360, 60]);
      imgui.setNextWindowSize([200, 200]);
      imgui.begin('without menu bar');
      withoutFlag = imgui.beginMenuBar();
      if (withoutFlag)
        imgui.endMenuBar();
      imgui.end();
    });
    const ok = withFlag === true && ended === 1 && withoutFlag === false;
    console.log(ok ? 'MENUBAR ok' :
                     'MENUBAR WRONG ' + withFlag + ' ' + ended + ' ' +
                         withoutFlag);
  }

  harness.checkGroupCoverage(h, 'Menus');
  process.exit(0);
}

main().catch((e) => {
  console.log('FAILED ' + (e && e.stack));
  process.exit(1);
});

// CHECK: {{^}}MAINBAR ok{{$}}
// CHECK-NEXT: {{^}}CLOSERS ok{{$}}
// CHECK-NEXT: {{^}}MENUITEM ok{{$}}
// CHECK-NEXT: {{^}}MENUREF ok{{$}}
// CHECK-NEXT: {{^}}MENUBAR ok{{$}}
// CHECK-NEXT: {{^}}COVERAGE Menus ok{{$}}
// CODE: EXITCODE 0

// No case here is a usage error, so none is traced (the
// --implicit-check-not on the TRACE run).
// TRACE: {{^}}[ui] imgui: context created
