// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// The binding's error model: usage errors ImGui reports and recovers from
// become ERR_IMGUI_USAGE, from the call or from render(); the closers ImGui
// does not check recoverably are refused by the binding's guards; and a
// throwing render() contributes nothing to its tick.
//
// Cases, in the order they run, one output line each (`<CASE> ok`; any
// failure prints its own line and exits 1):
//
//  UNMATCHED_END  end() with only the implicit window: ERR_IMGUI_USAGE from
//                 the call; the frame continues and render() returns true.
//  LEFT_OPEN      begin('w') with no end(): render() throws ERR_IMGUI_USAGE,
//                 the frame is closed, nothing was published (h.published
//                 re-read from the host is unchanged), and the next frame
//                 draws.
//  PUSHID_LEFT    pushID('a') unpopped inside a window: that window's end()
//                 throws.
//  MIXED_TICK     one tick, two callbacks: a complete frame filling the
//                 window red, then a frame whose render() throws. The
//                 tick's picture is the first callback's, red everywhere,
//                 the second callback's window included.
//  NO_TARGET      a frame left open, rendered outside any frame callback:
//                 ERR_IMGUI_USAGE rather than the `false` return.
//  GUARD_GROUP    endGroup() with no group: refused, not called. A group
//                 ImGui closes itself inside end() (which throws), then
//                 endGroup(): refused, not an abort. A group begun in the
//                 implicit window, then endGroup() inside another window:
//                 refused as the wrong window.
//  GUARD_CHILD    endChild() in a top-level window: refused. A child ImGui
//                 clips (placed far below its scrolling parent's visible
//                 area), whose beginChild() returned false (`SETUP child
//                 clipped`, read back), then endChild(): accepted.
//  GUARD_LABELID  button('') and beginChild('') at a window's root:
//                 refused, not called (their item ID would be the window's
//                 own, which ImGui asserts plainly); button('') inside
//                 pushID('p'): accepted. treeNode('') likewise refused.
//  GUARD_TREE     treePop() with no node open; treePop() inside a node that
//                 keeps stack data (TreeNodeFlags_DrawLinesFull) with a
//                 pushID left unpopped; treePop() after the node's own ID
//                 was popped with popID(): each refused, not called, and
//                 the frame then balanced by hand.
//  GUARD_TABITEM  endTabItem() inside a tab bar before any beginTabItem:
//                 refused.
//  GUARD_MENUBAR  endMenuBar() in a window without WindowFlags_MenuBar; in
//                 one with it but no menu bar begun; after the menu bar's
//                 own group was ended with endGroup(), with no group left
//                 and with another window's group on top: each refused.
//  GUARD_BEGINMENUBAR a second beginMenuBar() in one window before
//                 endMenuBar(): refused.
//  GUARD_MAINMENUBAR endMainMenuBar() after the main menu bar's own group
//                 was ended with endGroup(), with no group left and with
//                 the implicit window's group on top: each refused (ImGui
//                 checks only that a menu bar is open).
//  GUARD_TOOLTIP  endTooltip() in an ordinary window: refused.
//  GUARD_LISTBOX  endListBox() after a beginListBox that returned false
//                 (placed below its parent's visible area, `SETUP listbox
//                 clipped` read back); inside a plain beginChild() with no
//                 group open; inside one whose parent's top group is
//                 another window's: each refused.
//  GUARD_CONTEXTITEM beginPopupContextItem() with no strId after text(),
//                 an item with no ID: refused; with a strId: accepted.
//  GUARD_COMBO    a combo opened by a click shows as one active combo
//                 popup window, `##Combo_<depth>`. endCombo() with no combo
//                 open, and endCombo() from a child window begun inside
//                 the combo: each refused. In that same frame the combo is
//                 still active under the same name, and no other combo popup
//                 window is.
//  COMBO_LEFT_OPEN a frame leaves the open combo's endCombo() out
//                 (render() throws); in the next frame the combo is active
//                 under the same name as before, because newFrame() resets
//                 g.BeginComboDepth.
//  ENDMENU, ENDPOPUP, ENDTABBAR  endMenu(), endPopup() and endTabBar() with
//                 nothing to end: ImGui's own recoverable checks, so
//                 ERR_IMGUI_USAGE from the call, which did reach ImGui.
//  TABLE_OUTSIDE_GUARDED tableNextRow() and tableSetColumnIndex(0) with no
//                 table: refused, not called (ImGui dereferences the table
//                 in the first; the second is refused by ruling 12).
//  TABLE_OUTSIDE_FALSE tableNextColumn() with no table: false, as ImGui
//                 returns, and called.
//  TABLE_OUTSIDE_RECOVERED tableSetupColumn(), tableHeadersRow() and
//                 tableSetupScrollFreeze() with no table: ImGui's own
//                 recoverable checks, ERR_IMGUI_USAGE after the call.
//  ENDTABLE       endTable() with no table: likewise.
//  TABLE_NO_ROW   tableSetColumnIndex(0) in a table before any row: refused.
//  TABLE_NEGATIVE tableSetColumnIndex(-1) straight after tableNextRow():
//                 ERR_OUT_OF_RANGE, not called.
//  TABLE_INDEX_HIGH tableSetColumnIndex(2) in a 2-column table: ImGui's
//                 recoverable check, called; the row still takes index 1.
//  TABLE_FREEZE_LATE tableSetupScrollFreeze() after the first row: refused.
//  TABLE_WRONG_WINDOW a scrolling table's outer window appended to from
//                 inside a cell (`SETUP wrong window` reads back, through
//                 __test.tableScope(), that the table is current and that
//                 its inner window is not the current one):
//                 tableNextColumn() inside the row is accepted; once the
//                 row is full, tableNextColumn(), tableNextRow(),
//                 tableHeadersRow() and endTable() are each refused.
//  TABLE_CURSOR   endTable() with the cursor moved before the first row:
//                 refused; with it put back, accepted.
//  TABLE_NAMES    tableSetupColumn() labels whose offsets in the table's
//                 names buffer reach 32767 accepted, one past it refused,
//                 an empty label accepted; the header row then draws.
//  TABLE_REGION   endChild(), end() and endListBox() inside a scrolling
//                 table's cell (its region, a child window, with a group
//                 open in the outer window for endListBox): each refused.
//  DISCARD       newFrame(); begin('w'); newFrame(): no throw, `frame
//                 discarded` traced, the frame that follows renders.
//  GUARDS         every guard condition registered has refused at least
//                 once (harness.checkGuardCoverage).
//  CLOSING        (a run of its own) from an 'exit' listener, newFrame();
//                 begin('w'); render(): ERR_IMGUI_USAGE; the exit status of
//                 process.exit(5) is preserved.
//
// THE MUTATIONS THIS MUST BREAK (recorded in progress-imgui-binding.md):
//  configureContext() not called from ensureContext: ImGui asserts on the
//  first usage error (abort), or the cases fail;
//  the usage-error check moved after hnui_frame_target in render():
//  LEFT_OPEN's publication check fails;
//  canEndGroup() returning nullptr always: GUARD_GROUP aborts;
//  canEndChild() requiring a true beginChild: GUARD_CHILD's accepted half
//  fails;
//  labelIdIsWindowId() returning false always: GUARD_LABELID aborts
//  (ImGui's plain assertion, imgui.cpp:12319);
//  each of canTreePop, canEndTabItem, canEndMenuBar, canEndMainMenuBar,
//  canBeginMenuBar, canEndTooltip, canEndListBox, canBeginPopupContextItem
//  and canEndCombo
//  returning nullptr always: its case aborts or fails (which, per guard,
//  is recorded in the progress file);
//  each of canTableRowCall, canTableSetColumnIndex,
//  canTableSetupScrollFreeze, canEndTable, canTableSetupColumn, canEnd, and
//  the table-region conditions of canEndChild and canEndListBox, returning
//  nullptr always: its TABLE_* case aborts or fails;
//  the reset of g.BeginComboDepth removed from guard::beginFrame():
//  COMBO_LEFT_OPEN fails;
//  a guard condition registered and never refused: GUARDS fails
//  (`UNREFUSED <name>`);
//  every usage error traced twice (throwUsage() tracing too): ONCE fails.
//
// REQUIRES: ui, ui-gpu, ui-imgui
// REQUIRES: display
//
// RUN: %with-display 300 sh -c 'HERMES_NODE_UI_TEST_HOOKS=1 HERMES_NODE_DEBUG_NATIVE=UI %hermes-node-ui %s main %t.ppm > %t.out 2> %t.err'; echo "EXITCODE $?" | %FileCheck --check-prefix=CODE %s
// RUN: %FileCheck %s < %t.out
// RUN: %FileCheck --implicit-check-not='[sapp:error' --implicit-check-not='[sokol-' --check-prefix=TRACE %s < %t.err
// RUN: sh -c 'echo BEGIN; grep -E "imgui: (usage error|frame discarded)" %t.err; echo END' | %FileCheck --implicit-check-not='[sapp:error' --implicit-check-not='[sokol-' --check-prefix=ONCE %s
// RUN: %with-display 300 sh -c 'HERMES_NODE_UI_TEST_HOOKS=1 HERMES_NODE_DEBUG_NATIVE=UI %hermes-node-ui %s closing > %t.closing.out 2> %t.closing.err'; echo "EXITCODE $?" | %FileCheck --check-prefix=CLOSINGCODE %s
// RUN: %FileCheck --check-prefix=CLOSING %s < %t.closing.out
// RUN: %FileCheck --implicit-check-not='[sapp:error' --implicit-check-not='[sokol-' --check-prefix=CLOSINGTRACE %s < %t.closing.err
// RUN: sh -c 'echo BEGIN; grep -E "imgui: (usage error|frame discarded)" %t.closing.err; echo END' | %FileCheck --implicit-check-not='[sapp:error' --implicit-check-not='[sokol-' --check-prefix=CLOSINGONCE %s

'use strict';

const harness = require('./fixtures/imgui/harness.js');

const mode = process.argv[2];
const capturePath = process.argv[3];

function fail(line) {
  console.log(line);
  process.exit(1);
}

function sameColor(a, b) {
  return a[0] === b[0] && a[1] === b[1] && a[2] === b[2];
}

async function closing() {
  const h = await harness.open();
  const {ui, imgui} = h;
  await harness.frame(h, () => {});
  process.on('exit', () => {
    imgui.newFrame();
    imgui.begin('w');
    try {
      imgui.render();
      console.log('CLOSING no throw');
    } catch (e) {
      console.log('CLOSING ' + e.code);
    }
  });
  ui.requestAnimationFrame(() => process.exit(5));
}

async function main() {
  const h = await harness.open();
  const {imgui} = h;
  const calls = (name) => harness.calls(h, name);
  const usage = (fn) => harness.expectThrow(fn, 'ERR_IMGUI_USAGE');

  /** `fn` must be refused with ERR_IMGUI_USAGE, `name` not called. */
  function refused(name, fn) {
    const before = calls(name);
    const e = usage(fn);
    if (calls(name) !== before)
      fail('CALLED ' + name + ': ' + e.message);
    return e;
  }

  // --- UNMATCHED_END ---------------------------------------------------
  {
    let message;
    const drew = await harness.frame(h, () => {
      message = usage(() => imgui.end()).message;
      imgui.text('after');
    });
    console.log(drew ? 'UNMATCHED_END ok' : 'UNMATCHED_END render=' + drew);
    console.log('UNMATCHED_END message ' + message);
  }

  // --- LEFT_OPEN -------------------------------------------------------
  {
    await harness.resync(h);
    const before = h.published;
    let code = 'none';
    await harness.rawTick(h, () => {
      imgui.newFrame();
      imgui.begin('left open');
      try {
        imgui.render();
      } catch (e) {
        code = e.code;
      }
      return false;
    });
    const frameOpen = imgui.__test.state().frameOpen;
    await harness.resync(h);
    const after = h.published;
    const next = await harness.frame(h, () => imgui.text('next'));
    const ok = code === 'ERR_IMGUI_USAGE' && !frameOpen && after === before &&
        next === true;
    console.log(
        ok ? 'LEFT_OPEN ok' :
             'LEFT_OPEN WRONG code=' + code + ' frameOpen=' + frameOpen +
                 ' published=' + before + '->' + after + ' next=' + next);
  }

  // --- PUSHID_LEFT -----------------------------------------------------
  {
    let message;
    const drew = await harness.frame(h, () => {
      imgui.begin('pushid');
      imgui.pushID('a');
      message = usage(() => imgui.end()).message;
    });
    console.log(drew ? 'PUSHID_LEFT ok' : 'PUSHID_LEFT render=' + drew);
    console.log('PUSHID_LEFT message ' + message);
  }

  // --- MIXED_TICK ------------------------------------------------------
  {
    let code = 'none';
    const image = await harness.captureTick(h, [
      () => {
        imgui.newFrame();
        imgui.__test.scene('fill');
        return imgui.render();
      },
      () => {
        imgui.newFrame();
        imgui.setNextWindowPos([100, 100]);
        imgui.setNextWindowSize([200, 150]);
        imgui.begin('mixed');
        try {
          imgui.render();
        } catch (e) {
          code = e.code;
        }
        return false;
      },
    ], capturePath);
    // Inside the second callback's window, and elsewhere.
    const points = [[150, 150], [250, 200], [20, 20], [600, 440]];
    const wrong = points.filter(
        ([x, y]) => !sameColor(harness.pixelAt(image, x, y), [255, 0, 0]));
    const ok = code === 'ERR_IMGUI_USAGE' && wrong.length === 0;
    console.log(
        ok ? 'MIXED_TICK ok' :
             'MIXED_TICK WRONG code=' + code + ' ' +
                 wrong
                     .map(([x, y]) =>
                         x + ',' + y + '=' + harness.pixelAt(image, x, y))
                     .join(' '));
  }

  // --- NO_TARGET -------------------------------------------------------
  {
    imgui.newFrame();
    imgui.begin('no target');
    usage(() => imgui.render());
    const frameOpen = imgui.__test.state().frameOpen;
    console.log(frameOpen ? 'NO_TARGET frame still open' : 'NO_TARGET ok');
  }

  // --- GUARD_GROUP -----------------------------------------------------
  {
    let recovered = 'none';
    const drew = await harness.frame(h, () => {
      refused('endGroup', () => imgui.endGroup());
      imgui.begin('group');
      imgui.beginGroup();
      // ImGui closes the group itself, inside End(), and reports it.
      recovered = usage(() => imgui.end()).message;
      refused('endGroup', () => imgui.endGroup());
      // A group of the implicit window, closed from inside another one.
      imgui.beginGroup();
      imgui.begin('group other');
      refused('endGroup', () => imgui.endGroup());
      imgui.end();
      imgui.endGroup();
    });
    console.log(drew ? 'GUARD_GROUP ok' : 'GUARD_GROUP render=' + drew);
    console.log('GUARD_GROUP recovered ' + recovered);
  }

  // --- GUARD_CHILD -----------------------------------------------------
  {
    await harness.frame(h, () => {
      imgui.begin('child top');
      refused('endChild', () => imgui.endChild());
      imgui.end();
    });
    // A parent of 300x200 whose content reaches y 2000, so it scrolls; the
    // child sits there, entirely below the visible area. A new child is
    // not clipped on its first frames (it is still being sized), so frames
    // run until beginChild() answers false, at most a few.
    let clipped = false;
    let accepted = false;
    for (let i = 0; i < 10 && !clipped; ++i) {
      await harness.frame(h, () => {
        imgui.setNextWindowPos([40, 40]);
        imgui.setNextWindowSize([300, 200]);
        imgui.begin('child parent');
        imgui.setCursorScreenPos([60, 2000]);
        const shown = imgui.beginChild('clipped', [100, 100]);
        const before = calls('endChild');
        let threw = 'none';
        try {
          imgui.endChild();
        } catch (e) {
          threw = e.code;
        }
        // Counted only in the frame whose beginChild() returned false: the
        // case is about the clipped child.
        if (shown === false && threw === 'none' &&
            calls('endChild') === before + 1)
          accepted = true;
        if (threw === 'none') {
          imgui.end();
        } else {
          // Wrongly refused, the child is still current: two end() calls
          // close it and the parent, each reporting a usage error, so that
          // the frame stays balanced and the case reports instead of
          // throwing.
          for (let k = 0; k < 2; ++k) {
            try {
              imgui.end();
            } catch (e) {
            }
          }
        }
        clipped = shown === false;
      });
    }
    if (!clipped)
      fail('SETUP child not clipped');
    console.log('SETUP child clipped');
    console.log(accepted ? 'GUARD_CHILD ok' : 'GUARD_CHILD not accepted');
  }

  // --- GUARD_LABELID ---------------------------------------------------
  {
    let message;
    let accepted = false;
    const drew = await harness.frame(h, () => {
      imgui.begin('label id');
      message = refused('button', () => imgui.button('')).message;
      refused('beginChild', () => imgui.beginChild(''));
      refused('treeNode', () => imgui.treeNode(''));
      // The same label one ID-stack level down is not the window's ID.
      const before = calls('button');
      imgui.pushID('p');
      imgui.button('');
      imgui.popID();
      accepted = calls('button') === before + 1;
      imgui.end();
    });
    console.log(drew && accepted ? 'GUARD_LABELID ok' :
                                   'GUARD_LABELID WRONG ' + drew + ' ' +
                                       accepted);
    console.log('GUARD_LABELID message ' + message);
  }

  /** `name` must be accepted: called once more, nothing thrown. */
  function accepted(name, fn) {
    const before = calls(name);
    const result = fn();
    if (calls(name) !== before + 1)
      fail('NOTCALLED ' + name);
    return result;
  }

  /** A window of its own at a fixed place and size. */
  function beginAt(name, flags) {
    imgui.setNextWindowPos([40, 40]);
    imgui.setNextWindowSize([300, 200]);
    imgui.begin(name, null, flags);
  }

  // --- GUARD_TREE ------------------------------------------------------
  {
    const drew = await harness.frame(h, () => {
      beginAt('tree');
      refused('treePop', () => imgui.treePop());
      // DrawLinesFull makes the node keep stack data (widgets:7097-7100,
      // 7120-7121), so TreePop() compares its ID with the ID stack's top.
      if (!imgui.treeNodeEx('lines', imgui.TreeNodeFlags_DefaultOpen |
                                         imgui.TreeNodeFlags_DrawLinesFull))
        fail('SETUP tree lines closed');
      imgui.pushID('left');
      refused('treePop', () => imgui.treePop());
      imgui.popID();
      accepted('treePop', () => imgui.treePop());
      // A node without stack data whose ID was popped: only the window's
      // own ID is left, which TreePop() asserts against (widgets:7426).
      if (!imgui.treeNodeEx('popped', imgui.TreeNodeFlags_DefaultOpen))
        fail('SETUP tree popped closed');
      imgui.popID();
      refused('treePop', () => imgui.treePop());
      imgui.pushID('back');
      accepted('treePop', () => imgui.treePop());
      imgui.end();
    });
    console.log(drew ? 'GUARD_TREE ok' : 'GUARD_TREE render=' + drew);
  }

  // --- GUARD_TABITEM ---------------------------------------------------
  {
    const drew = await harness.frame(h, () => {
      beginAt('tab item');
      if (!imgui.beginTabBar('bar'))
        fail('SETUP no tab bar');
      refused('endTabItem', () => imgui.endTabItem());
      imgui.endTabBar();
      imgui.end();
    });
    console.log(drew ? 'GUARD_TABITEM ok' : 'GUARD_TABITEM render=' + drew);
  }

  // --- GUARD_MENUBAR ---------------------------------------------------
  {
    const drew = await harness.frame(h, () => {
      beginAt('no menu bar');
      refused('endMenuBar', () => imgui.endMenuBar());
      imgui.end();
      beginAt('menu bar', imgui.WindowFlags_MenuBar);
      refused('endMenuBar', () => imgui.endMenuBar());
      if (!imgui.beginMenuBar())
        fail('SETUP no menu bar');
      // The menu bar's own group (widgets:9332) belongs to this window, so
      // endGroup() ends it; nothing is left on the group stack.
      accepted('endGroup', () => imgui.endGroup());
      refused('endMenuBar', () => imgui.endMenuBar());
      imgui.beginGroup();
      accepted('endMenuBar', () => imgui.endMenuBar());
      imgui.end();
      // The same with the implicit window's group below it.
      imgui.beginGroup();
      beginAt('menu bar 2', imgui.WindowFlags_MenuBar);
      if (!imgui.beginMenuBar())
        fail('SETUP no menu bar 2');
      accepted('endGroup', () => imgui.endGroup());
      refused('endMenuBar', () => imgui.endMenuBar());
      imgui.beginGroup();
      accepted('endMenuBar', () => imgui.endMenuBar());
      imgui.end();
      imgui.endGroup();
    });
    console.log(drew ? 'GUARD_MENUBAR ok' : 'GUARD_MENUBAR render=' + drew);
  }

  // --- GUARD_BEGINMENUBAR ----------------------------------------------
  {
    const drew = await harness.frame(h, () => {
      beginAt('menu bar twice', imgui.WindowFlags_MenuBar);
      if (!imgui.beginMenuBar())
        fail('SETUP no menu bar twice');
      refused('beginMenuBar', () => imgui.beginMenuBar());
      imgui.endMenuBar();
      imgui.end();
    });
    console.log(drew ? 'GUARD_BEGINMENUBAR ok' :
                       'GUARD_BEGINMENUBAR render=' + drew);
  }

  // --- GUARD_MAINMENUBAR -----------------------------------------------
  {
    const drew = await harness.frame(h, () => {
      // EndMainMenuBar()'s own check is only that a menu bar is open
      // (widgets:9483); its EndMenuBar() then ends the group on top.
      if (!imgui.beginMainMenuBar())
        fail('SETUP no main menu bar');
      accepted('endGroup', () => imgui.endGroup());
      refused('endMainMenuBar', () => imgui.endMainMenuBar());
      imgui.beginGroup();
      accepted('endMainMenuBar', () => imgui.endMainMenuBar());
      imgui.beginGroup();
      if (!imgui.beginMainMenuBar())
        fail('SETUP no main menu bar 2');
      accepted('endGroup', () => imgui.endGroup());
      refused('endMainMenuBar', () => imgui.endMainMenuBar());
      imgui.beginGroup();
      accepted('endMainMenuBar', () => imgui.endMainMenuBar());
      imgui.endGroup();
    });
    console.log(drew ? 'GUARD_MAINMENUBAR ok' :
                       'GUARD_MAINMENUBAR render=' + drew);
  }

  // --- GUARD_TOOLTIP ---------------------------------------------------
  {
    const drew = await harness.frame(h, () => {
      beginAt('tooltip');
      refused('endTooltip', () => imgui.endTooltip());
      imgui.end();
    });
    console.log(drew ? 'GUARD_TOOLTIP ok' : 'GUARD_TOOLTIP render=' + drew);
  }

  // --- GUARD_LISTBOX ---------------------------------------------------
  {
    // The placement GUARD_CHILD uses: far below a 300x200 parent's visible
    // area. BeginListBox tests visibility before anything else
    // (widgets:8971-8977), so it answers false on its first frame.
    let shown = null;
    const drew = await harness.frame(h, () => {
      beginAt('list parent');
      imgui.setCursorScreenPos([60, 2000]);
      shown = imgui.beginListBox('##clipped', [100, 100]);
      if (shown === false)
        refused('endListBox', () => imgui.endListBox());
      else
        imgui.endListBox();
      imgui.setCursorScreenPos([60, 60]);
      imgui.beginChild('plain child', [100, 100]);
      refused('endListBox', () => imgui.endListBox());
      imgui.endChild();
      imgui.end();
      // A group of the implicit window under a child of another window.
      imgui.beginGroup();
      beginAt('list parent 2');
      imgui.beginChild('plain child 2', [100, 100]);
      refused('endListBox', () => imgui.endListBox());
      imgui.endChild();
      imgui.end();
      imgui.endGroup();
    });
    if (shown !== false)
      fail('SETUP listbox not clipped');
    console.log('SETUP listbox clipped');
    console.log(drew ? 'GUARD_LISTBOX ok' : 'GUARD_LISTBOX render=' + drew);
  }

  // --- GUARD_CONTEXTITEM -----------------------------------------------
  {
    const drew = await harness.frame(h, () => {
      beginAt('context item');
      imgui.text('no id');
      refused('beginPopupContextItem', () => imgui.beginPopupContextItem());
      refused('beginPopupContextItem',
              () => imgui.beginPopupContextItem(null, 0));
      if (accepted('beginPopupContextItem',
                   () => imgui.beginPopupContextItem('named')))
        imgui.endPopup();
      imgui.end();
    });
    console.log(drew ? 'GUARD_CONTEXTITEM ok' :
                       'GUARD_CONTEXTITEM render=' + drew);
  }

  /**
   * The names of the active combo popup windows, `##Combo_<depth>`
   * (widgets:2055); a child window of one is `##Combo_00/<name>_<id>`.
   */
  const comboWindows = () =>
      imgui.__test.state()
          .windows.filter((w) => /^##Combo_-?[0-9]+$/.test(w.name) && w.active)
          .map((w) => w.name);

  // --- GUARD_COMBO -----------------------------------------------------
  let comboName;
  // A combo in a window of its own; `extra` runs inside it while it is
  // open, `before` ahead of it. `endIt` false leaves its endCombo() out.
  let comboOpen = false, comboRect;
  const comboDraw = (before, extra, endIt = true) => () => {
    beginAt('combo');
    if (before)
      before();
    comboOpen = imgui.beginCombo('##guarded', 'preview');
    if (!comboOpen)
      comboRect = harness.rect(imgui);
    if (comboOpen) {
      imgui.selectable('one');
      if (extra)
        extra();
      if (endIt)
        imgui.endCombo();
    }
    if (endIt || !comboOpen)
      imgui.end();
  };
  {
    const plain = comboDraw();
    await harness.frame(h, plain);
    await harness.frame(h, plain);
    await harness.clickAt(h, plain, harness.center(comboRect));
    await harness.until(h, plain, () => comboOpen, 'GUARD_COMBO open');
    const names = comboWindows();
    if (names.length !== 1)
      fail('SETUP combo windows ' + JSON.stringify(names));
    comboName = names[0];
    console.log('SETUP combo ' + comboName);
    const drew = await harness.frame(
        h,
        comboDraw(
            () => refused('endCombo', () => imgui.endCombo()),
            () => {
              imgui.beginChild('in combo', [50, 50]);
              refused('endCombo', () => imgui.endCombo());
              imgui.endChild();
            }));
    const after = comboWindows();
    const ok = drew && comboOpen && after.length === 1 &&
        after[0] === comboName;
    console.log(ok ? 'GUARD_COMBO ok' :
                     'GUARD_COMBO WRONG ' + drew + ' ' + comboOpen + ' ' +
                         JSON.stringify(after));
  }

  // --- COMBO_LEFT_OPEN -------------------------------------------------
  {
    let code = 'none';
    await harness.rawTick(h, () => {
      imgui.newFrame();
      comboDraw(null, null, false)();
      try {
        imgui.render();
      } catch (e) {
        code = e.code;
      }
      return false;
    });
    const leftOpen = comboOpen;
    await harness.resync(h);
    await harness.frame(h, comboDraw());
    const after = comboWindows();
    const ok = code === 'ERR_IMGUI_USAGE' && leftOpen && comboOpen &&
        after.length === 1 && after[0] === comboName;
    console.log(ok ? 'COMBO_LEFT_OPEN ok' :
                     'COMBO_LEFT_OPEN WRONG ' + code + ' ' + leftOpen + ' ' +
                         comboOpen + ' ' + JSON.stringify(after));
    // Closed by a click outside it, so that nothing later sees it.
    const plain = comboDraw();
    await harness.clickAt(h, plain, [600, 440]);
    await harness.until(h, plain, () => !comboOpen, 'COMBO closed');
  }

  // --- ENDMENU, ENDPOPUP, ENDTABBAR ------------------------------------
  {
    const messages = {};
    const drew = await harness.frame(h, () => {
      beginAt('recoverable');
      for (const [label, name] of
               [['ENDMENU', 'endMenu'], ['ENDPOPUP', 'endPopup'],
                ['ENDTABBAR', 'endTabBar']]) {
        messages[label] =
            accepted(name, () => usage(() => imgui[name]())).message;
      }
      imgui.end();
    });
    for (const label of Object.keys(messages)) {
      console.log(drew ? label + ' ok' : label + ' render=' + drew);
      console.log(label + ' message ' + messages[label]);
    }
  }

  // --- TABLE_OUTSIDE_* and ENDTABLE ------------------------------------
  {
    const recovered = {};
    let nextColumn, endTable;
    const drew = await harness.frame(h, () => {
      beginAt('table outside');
      refused('tableNextRow', () => imgui.tableNextRow());
      refused('tableSetColumnIndex', () => imgui.tableSetColumnIndex(0));
      nextColumn = accepted('tableNextColumn', () => imgui.tableNextColumn());
      for (const [name, fn] of [
               ['tableSetupColumn', () => imgui.tableSetupColumn('c')],
               ['tableHeadersRow', () => imgui.tableHeadersRow()],
               ['tableSetupScrollFreeze',
                () => imgui.tableSetupScrollFreeze(0, 1)]])
        recovered[name] = accepted(name, () => usage(fn)).message;
      endTable =
          accepted('endTable', () => usage(() => imgui.endTable())).message;
      imgui.end();
    });
    console.log(drew ? 'TABLE_OUTSIDE_GUARDED ok' :
                       'TABLE_OUTSIDE_GUARDED render=' + drew);
    console.log(nextColumn === false ? 'TABLE_OUTSIDE_FALSE ok' :
                                       'TABLE_OUTSIDE_FALSE ' + nextColumn);
    console.log('TABLE_OUTSIDE_RECOVERED ok');
    for (const name of Object.keys(recovered))
      console.log('TABLE_OUTSIDE_RECOVERED message ' + recovered[name]);
    console.log('ENDTABLE ok');
    console.log('ENDTABLE message ' + endTable);
  }

  // --- TABLE_NO_ROW, TABLE_NEGATIVE, TABLE_INDEX_HIGH, TABLE_FREEZE_LATE -
  {
    let negative, high, after;
    const drew = await harness.frame(h, () => {
      beginAt('table rows');
      imgui.beginTable('no row', 2);
      refused('tableSetColumnIndex', () => imgui.tableSetColumnIndex(0));
      imgui.tableNextRow();
      imgui.endTable();
      imgui.beginTable('negative', 2);
      imgui.tableNextRow();
      const before = calls('tableSetColumnIndex');
      negative = harness.expectThrow(() => imgui.tableSetColumnIndex(-1),
                                     'ERR_OUT_OF_RANGE').message;
      if (calls('tableSetColumnIndex') !== before)
        fail('CALLED tableSetColumnIndex: ' + negative);
      high = accepted('tableSetColumnIndex',
                      () => usage(() => imgui.tableSetColumnIndex(2))).message;
      after = imgui.tableSetColumnIndex(1);
      imgui.endTable();
      imgui.beginTable('freeze', 1, imgui.TableFlags_ScrollY, [0, 100]);
      imgui.tableNextRow();
      refused('tableSetupScrollFreeze',
              () => imgui.tableSetupScrollFreeze(0, 1));
      imgui.endTable();
      imgui.end();
    });
    console.log(drew ? 'TABLE_NO_ROW ok' : 'TABLE_NO_ROW render=' + drew);
    console.log('TABLE_NEGATIVE ok');
    console.log('TABLE_NEGATIVE message ' + negative);
    console.log(after === true ? 'TABLE_INDEX_HIGH ok' :
                                 'TABLE_INDEX_HIGH after ' + after);
    console.log('TABLE_INDEX_HIGH message ' + high);
    console.log('TABLE_FREEZE_LATE ok');
  }

  // --- TABLE_WRONG_WINDOW ----------------------------------------------
  // A scrolling table's outer window names the table as its current one
  // (BeginTableEx, imgui_tables.cpp:551), so appending to that window from
  // inside a cell (a second begin() of its name) makes it current with the
  // table current, while the table's inner window is the child.
  {
    let scope, moved;
    const drew = await harness.frame(h, () => {
      beginAt('wrong window');
      imgui.beginTable('s', 2, imgui.TableFlags_ScrollY, [0, 100]);
      imgui.tableNextRow();
      imgui.tableNextColumn();
      imgui.text('cell');
      imgui.begin('wrong window');
      scope = imgui.__test.tableScope();
      // Inside the row nothing asserts on the window: accepted.
      moved = accepted('tableNextColumn', () => imgui.tableNextColumn());
      // The row is full, so this one would end it (TableEndRow).
      refused('tableNextColumn', () => imgui.tableNextColumn());
      refused('tableNextRow', () => imgui.tableNextRow());
      refused('tableHeadersRow', () => imgui.tableHeadersRow());
      refused('endTable', () => imgui.endTable());
      imgui.end();
      imgui.endTable();
      imgui.end();
    });
    const setup = scope.table && scope.currentWindow === 'wrong window' &&
        scope.innerWindow.startsWith('wrong window/s_');
    if (!setup)
      fail('SETUP wrong window ' + JSON.stringify(scope));
    console.log('SETUP wrong window: table current, current window ' +
                scope.currentWindow + ', inner window ' +
                scope.innerWindow.replace(/_[0-9A-F]{8}$/, '_<id>'));
    console.log(drew && moved === true ? 'TABLE_WRONG_WINDOW ok' :
                                         'TABLE_WRONG_WINDOW WRONG ' + drew +
                                             ' ' + moved);
  }

  // --- TABLE_CURSOR, TABLE_NAMES, TABLE_REGION -------------------------
  {
    const drew = await harness.frame(h, () => {
      beginAt('table misc');
      // EndTable() asserts plainly that the cursor is where the last row
      // ended (imgui_tables.cpp:1493); before any row it is where the
      // table began.
      imgui.beginTable('cursor', 1);
      const p = imgui.getCursorScreenPos();
      imgui.setCursorScreenPos([p[0], p[1] + 10]);
      refused('endTable', () => imgui.endTable());
      imgui.setCursorScreenPos(p);
      accepted('endTable', () => imgui.endTable());
      // Each name is stored at an ImS16 offset into one buffer per table
      // (imgui_tables.cpp:1746-1750): a first name of 32766 bytes puts the
      // second at 32767, which fits, and the third at 32769, which does not.
      imgui.beginTable('names', 4);
      accepted('tableSetupColumn',
               () => imgui.tableSetupColumn('x'.repeat(32766)));
      accepted('tableSetupColumn', () => imgui.tableSetupColumn('b'));
      refused('tableSetupColumn', () => imgui.tableSetupColumn('c'));
      // A column without a name stores none.
      accepted('tableSetupColumn', () => imgui.tableSetupColumn(''));
      imgui.tableHeadersRow();
      imgui.endTable();
      // Ending the scrolling table's own region as a window: ImGui's
      // recovery would end the table from inside End() and end the child
      // twice.
      imgui.beginGroup();
      imgui.beginTable('region', 1, imgui.TableFlags_ScrollY, [0, 100]);
      imgui.tableNextRow();
      imgui.tableNextColumn();
      refused('endChild', () => imgui.endChild());
      refused('end', () => imgui.end());
      refused('endListBox', () => imgui.endListBox());
      imgui.endTable();
      imgui.endGroup();
      imgui.end();
    });
    console.log(drew ? 'TABLE_CURSOR ok' : 'TABLE_CURSOR render=' + drew);
    console.log('TABLE_NAMES ok');
    console.log('TABLE_REGION ok');
  }

  // --- DISCARD ---------------------------------------------------------
  {
    const drew = await harness.frame(h, () => {
      imgui.begin('discarded');
      imgui.newFrame();
    });
    console.log(drew ? 'DISCARD ok' : 'DISCARD render=' + drew);
  }

  harness.checkGuardCoverage(h);
  process.exit(0);
}

(mode === 'closing' ? closing() : main()).catch((e) => {
  console.log('FAILED ' + (e && e.stack));
  process.exit(1);
});

// CHECK: {{^}}UNMATCHED_END ok{{$}}
// CHECK-NEXT: {{^}}UNMATCHED_END message imgui.end(): Calling End() too many times!{{$}}
// CHECK-NEXT: {{^}}LEFT_OPEN ok{{$}}
// CHECK-NEXT: {{^}}PUSHID_LEFT ok{{$}}
// CHECK-NEXT: {{^}}PUSHID_LEFT message imgui.end(): Missing PopID(){{$}}
// CHECK-NEXT: {{^}}MIXED_TICK ok{{$}}
// CHECK-NEXT: {{^}}NO_TARGET ok{{$}}
// CHECK-NEXT: {{^}}GUARD_GROUP ok{{$}}
// CHECK-NEXT: {{^}}GUARD_GROUP recovered imgui.end(): Missing EndGroup(){{$}}
// CHECK-NEXT: {{^}}SETUP child clipped{{$}}
// CHECK-NEXT: {{^}}GUARD_CHILD ok{{$}}
// CHECK-NEXT: {{^}}GUARD_LABELID ok{{$}}
// CHECK-NEXT: {{^}}GUARD_LABELID message imgui.button(): label gives the item the window's own ID (an empty label, or one ending in '###', at the root of a window); use '##name' or pushID(){{$}}
// CHECK-NEXT: {{^}}GUARD_TREE ok{{$}}
// CHECK-NEXT: {{^}}GUARD_TABITEM ok{{$}}
// CHECK-NEXT: {{^}}GUARD_MENUBAR ok{{$}}
// CHECK-NEXT: {{^}}GUARD_BEGINMENUBAR ok{{$}}
// CHECK-NEXT: {{^}}GUARD_MAINMENUBAR ok{{$}}
// CHECK-NEXT: {{^}}GUARD_TOOLTIP ok{{$}}
// CHECK-NEXT: {{^}}SETUP listbox clipped{{$}}
// CHECK-NEXT: {{^}}GUARD_LISTBOX ok{{$}}
// CHECK-NEXT: {{^}}GUARD_CONTEXTITEM ok{{$}}
// CHECK-NEXT: {{^}}SETUP combo ##Combo_00{{$}}
// CHECK-NEXT: {{^}}GUARD_COMBO ok{{$}}
// CHECK-NEXT: {{^}}COMBO_LEFT_OPEN ok{{$}}
// CHECK-NEXT: {{^}}ENDMENU ok{{$}}
// CHECK-NEXT: {{^}}ENDMENU message imgui.endMenu(): Calling EndMenu() in wrong window!{{$}}
// CHECK-NEXT: {{^}}ENDPOPUP ok{{$}}
// CHECK-NEXT: {{^}}ENDPOPUP message imgui.endPopup(): Calling EndPopup() in wrong window!{{$}}
// CHECK-NEXT: {{^}}ENDTABBAR ok{{$}}
// CHECK-NEXT: {{^}}ENDTABBAR message imgui.endTabBar(): Mismatched BeginTabBar()/EndTabBar()!{{$}}
// CHECK-NEXT: {{^}}TABLE_OUTSIDE_GUARDED ok{{$}}
// CHECK-NEXT: {{^}}TABLE_OUTSIDE_FALSE ok{{$}}
// CHECK-NEXT: {{^}}TABLE_OUTSIDE_RECOVERED ok{{$}}
// CHECK-NEXT: {{^}}TABLE_OUTSIDE_RECOVERED message imgui.tableSetupColumn(): Call should only be done while in BeginTable() scope!{{$}}
// CHECK-NEXT: {{^}}TABLE_OUTSIDE_RECOVERED message imgui.tableHeadersRow(): Call should only be done while in BeginTable() scope!{{$}}
// CHECK-NEXT: {{^}}TABLE_OUTSIDE_RECOVERED message imgui.tableSetupScrollFreeze(): Call should only be done while in BeginTable() scope!{{$}}
// CHECK-NEXT: {{^}}ENDTABLE ok{{$}}
// CHECK-NEXT: {{^}}ENDTABLE message imgui.endTable(): EndTable() call should only be done while in BeginTable() scope!{{$}}
// CHECK-NEXT: {{^}}TABLE_NO_ROW ok{{$}}
// CHECK-NEXT: {{^}}TABLE_NEGATIVE ok{{$}}
// CHECK-NEXT: {{^}}TABLE_NEGATIVE message imgui.tableSetColumnIndex(): columnN must be an integer from 0 to 2147483647; got -1{{$}}
// CHECK-NEXT: {{^}}TABLE_INDEX_HIGH ok{{$}}
// CHECK-NEXT: {{^}}TABLE_INDEX_HIGH message imgui.tableSetColumnIndex(): TableSetColumnIndex() invalid column index!{{$}}
// CHECK-NEXT: {{^}}TABLE_FREEZE_LATE ok{{$}}
// CHECK-NEXT: {{^}}SETUP wrong window: table current, current window wrong window, inner window wrong window/s_<id>{{$}}
// CHECK-NEXT: {{^}}TABLE_WRONG_WINDOW ok{{$}}
// CHECK-NEXT: {{^}}TABLE_CURSOR ok{{$}}
// CHECK-NEXT: {{^}}TABLE_NAMES ok{{$}}
// CHECK-NEXT: {{^}}TABLE_REGION ok{{$}}
// CHECK-NEXT: {{^}}DISCARD ok{{$}}
// CHECK-NEXT: {{^}}GUARDS ok{{$}}
// CODE: EXITCODE 0

// Every usage error is traced exactly once, ImGui's own and the guards'
// alike, in the order the cases raise them. TRACE reads the whole stderr
// (for the Sokol exclusions); ONCE reads only the usage-error and discard
// lines, between a BEGIN and an END line, as one -NEXT chain, so a line
// traced twice, anywhere, breaks the chain.
// TRACE: {{^}}[ui] imgui: usage error: Calling End() too many times!{{$}}
// TRACE: {{^}}[ui] imgui: frame discarded{{$}}
// ONCE: {{^}}BEGIN{{$}}
// ONCE-NEXT: {{^}}[ui] imgui: usage error: Calling End() too many times!{{$}}
// ONCE-NEXT: {{^}}[ui] imgui: usage error: Missing End(){{$}}
// ONCE-NEXT: {{^}}[ui] imgui: usage error: Missing PopID(){{$}}
// ONCE-NEXT: {{^}}[ui] imgui: usage error: Missing End(){{$}}
// ONCE-NEXT: {{^}}[ui] imgui: usage error: Missing End(){{$}}
// ONCE-NEXT: {{^}}[ui] imgui: usage error: EndGroup() with no group open{{$}}
// ONCE-NEXT: {{^}}[ui] imgui: usage error: Missing EndGroup(){{$}}
// ONCE-NEXT: {{^}}[ui] imgui: usage error: EndGroup() with no group open{{$}}
// ONCE-NEXT: {{^}}[ui] imgui: usage error: EndGroup() in another window than its BeginGroup(){{$}}
// ONCE-NEXT: {{^}}[ui] imgui: usage error: EndChild() outside a child window{{$}}
// ONCE-NEXT: {{^}}[ui] imgui: usage error: an item ID equal to the window's own ID{{$}}
// ONCE-NEXT: {{^}}[ui] imgui: usage error: an item ID equal to the window's own ID{{$}}
// ONCE-NEXT: {{^}}[ui] imgui: usage error: an item ID equal to the window's own ID{{$}}
// ONCE-NEXT: {{^}}[ui] imgui: usage error: TreePop() with no tree node open{{$}}
// ONCE-NEXT: {{^}}[ui] imgui: usage error: TreePop() with an ID pushed inside the tree node still on the ID stack{{$}}
// ONCE-NEXT: {{^}}[ui] imgui: usage error: TreePop() after the tree node's own ID was popped{{$}}
// ONCE-NEXT: {{^}}[ui] imgui: usage error: EndTabItem() with no tab item begun in the tab bar{{$}}
// ONCE-NEXT: {{^}}[ui] imgui: usage error: EndMenuBar() in a window without WindowFlags_MenuBar{{$}}
// ONCE-NEXT: {{^}}[ui] imgui: usage error: EndMenuBar() with no menu bar open{{$}}
// ONCE-NEXT: {{^}}[ui] imgui: usage error: EndMenuBar() with no group open (the menu bar's own group was ended){{$}}
// ONCE-NEXT: {{^}}[ui] imgui: usage error: EndMenuBar() with another window's group on top (the menu bar's own group was ended){{$}}
// ONCE-NEXT: {{^}}[ui] imgui: usage error: BeginMenuBar() while this window's menu bar is open{{$}}
// ONCE-NEXT: {{^}}[ui] imgui: usage error: EndMainMenuBar() with no group open (the menu bar's own group was ended){{$}}
// ONCE-NEXT: {{^}}[ui] imgui: usage error: EndMainMenuBar() with another window's group on top (the menu bar's own group was ended){{$}}
// ONCE-NEXT: {{^}}[ui] imgui: usage error: EndTooltip() outside a tooltip{{$}}
// ONCE-NEXT: {{^}}[ui] imgui: usage error: EndListBox() outside a child window{{$}}
// ONCE-NEXT: {{^}}[ui] imgui: usage error: EndListBox() with no group open (the list box's group){{$}}
// ONCE-NEXT: {{^}}[ui] imgui: usage error: EndListBox() with another window's group on top (not the list box's){{$}}
// ONCE-NEXT: {{^}}[ui] imgui: usage error: BeginPopupContextItem() with no strId after an item that has no ID{{$}}
// ONCE-NEXT: {{^}}[ui] imgui: usage error: BeginPopupContextItem() with no strId after an item that has no ID{{$}}
// ONCE-NEXT: {{^}}[ui] imgui: usage error: EndCombo() with no combo open{{$}}
// ONCE-NEXT: {{^}}[ui] imgui: usage error: EndCombo() in another window than the combo's popup{{$}}
// COMBO_LEFT_OPEN's render(): the popup and its parent closed by EndFrame().
// ONCE-NEXT: {{^}}[ui] imgui: usage error: Missing End(){{$}}
// ONCE-NEXT: {{^}}[ui] imgui: usage error: Must call EndPopup() and not End()!{{$}}
// ONCE-NEXT: {{^}}[ui] imgui: usage error: Missing End(){{$}}
// ONCE-NEXT: {{^}}[ui] imgui: usage error: Calling EndMenu() in wrong window!{{$}}
// ONCE-NEXT: {{^}}[ui] imgui: usage error: Calling EndPopup() in wrong window!{{$}}
// ONCE-NEXT: {{^}}[ui] imgui: usage error: Mismatched BeginTabBar()/EndTabBar()!{{$}}
// ONCE-NEXT: {{^}}[ui] imgui: usage error: TableNextRow() outside a BeginTable() scope{{$}}
// ONCE-NEXT: {{^}}[ui] imgui: usage error: TableSetColumnIndex() outside a BeginTable() scope{{$}}
// ONCE-NEXT: {{^}}[ui] imgui: usage error: Call should only be done while in BeginTable() scope!{{$}}
// ONCE-NEXT: {{^}}[ui] imgui: usage error: Call should only be done while in BeginTable() scope!{{$}}
// ONCE-NEXT: {{^}}[ui] imgui: usage error: Call should only be done while in BeginTable() scope!{{$}}
// ONCE-NEXT: {{^}}[ui] imgui: usage error: EndTable() call should only be done while in BeginTable() scope!{{$}}
// ONCE-NEXT: {{^}}[ui] imgui: usage error: TableSetColumnIndex() before TableNextRow(){{$}}
// ONCE-NEXT: {{^}}[ui] imgui: usage error: TableSetColumnIndex() invalid column index!{{$}}
// ONCE-NEXT: {{^}}[ui] imgui: usage error: TableSetupScrollFreeze() after the table's first row{{$}}
// ONCE-NEXT: {{^}}[ui] imgui: usage error: TableNextColumn() ending a row in another window than the table's own (a window begun inside a cell is current){{$}}
// ONCE-NEXT: {{^}}[ui] imgui: usage error: TableNextRow() ending a row in another window than the table's own (a window begun inside a cell is current){{$}}
// ONCE-NEXT: {{^}}[ui] imgui: usage error: TableHeadersRow() ending a row in another window than the table's own (a window begun inside a cell is current){{$}}
// ONCE-NEXT: {{^}}[ui] imgui: usage error: EndTable() in another window than the table's own (a window begun inside a cell is current){{$}}
// ONCE-NEXT: {{^}}[ui] imgui: usage error: EndTable() with the cursor moved before the table's first row{{$}}
// ONCE-NEXT: {{^}}[ui] imgui: usage error: TableSetupColumn() with the table's column names already over 32767 bytes{{$}}
// ONCE-NEXT: {{^}}[ui] imgui: usage error: EndChild() in a scrolling table's own region; call EndTable() first{{$}}
// ONCE-NEXT: {{^}}[ui] imgui: usage error: End() in a scrolling table's own region; call EndTable() first{{$}}
// ONCE-NEXT: {{^}}[ui] imgui: usage error: EndListBox() in a scrolling table's own region; call EndTable() first{{$}}
// The discarded frame's open window is reported, then the discard.
// ONCE-NEXT: {{^}}[ui] imgui: usage error: Missing End(){{$}}
// ONCE-NEXT: {{^}}[ui] imgui: frame discarded{{$}}
// ONCE-NEXT: {{^}}END{{$}}

// CLOSING: {{^}}CLOSING ERR_IMGUI_USAGE{{$}}
// CLOSINGCODE: EXITCODE 5
// CLOSINGTRACE: {{^}}[ui] imgui: usage error: Missing End(){{$}}
// CLOSINGONCE: {{^}}BEGIN{{$}}
// CLOSINGONCE-NEXT: {{^}}[ui] imgui: usage error: Missing End(){{$}}
// CLOSINGONCE-NEXT: {{^}}END{{$}}
