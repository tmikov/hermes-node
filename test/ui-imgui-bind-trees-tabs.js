// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// The Trees and tabs group: tree nodes and collapsing headers opened by a
// click, a tab bar whose selection follows a click, the close buttons of a
// header and of a tab writing their reference arrays back; the group's
// "Rejected" line; and its coverage. The treePop guard's refusals are
// test/ui-imgui-bind-errors.js's (GUARD_TREE); the one here is the
// NoTreePushOnOpen case, which the surface entry of treeNodeEx names.
//
// Cases, in the order they run, one output line each (`<CASE> ok`; any
// failure prints its own line and exits 1):
//
//  TREENODE  treeNode('node') is closed, and its child item is not drawn,
//            until a click on it; then it returns true and the child item
//            is drawn below it.
//  SETOPEN   setNextItemOpen(true) before treeNode('preset') opens it in
//            the first frame it is drawn, with no click; a node drawn
//            beside it without one stays closed.
//  TREEEX    treeNodeEx(label, TreeNodeFlags_DefaultOpen) returns true and
//            treePop() is accepted; with TreeNodeFlags_NoTreePushOnOpen
//            added it also returns true, and a following treePop() throws
//            ERR_IMGUI_USAGE without reaching ImGui (nothing was pushed).
//  HEADER    collapsingHeader('h') opens on a click; collapsingHeader(
//            'hv', pVisible) with pVisible [true] draws, and a click on its
//            close button sets pVisible[0] to false, after which it draws
//            nothing and returns false; collapsingHeader('hn', null) draws
//            as the first form does.
//  TABS      a tab bar with tabs 'A' and 'B': 'A' is the one whose
//            beginTabItem returns true until a click on 'B', then 'B' is.
//  TABCLOSE  a tab 'C' with pOpen [true]: a click on its close button sets
//            pOpen[0] to false.
//  REJECTED  beginTabItem with TabItemFlags_Leading and _Trailing together:
//            ERR_OUT_OF_RANGE, not called.
//  COVERAGE  every Trees and tabs function has reached ImGui.
//
// Close buttons are located from the item's rectangle, a pushed
// StyleVar_FramePadding of [4, 4] and getTextLineHeight() (the font size),
// following CollapsingHeader (imgui_widgets.cpp:7494-7495) and
// TabItemLabelAndCloseButton (imgui_widgets.cpp:11227-11228), rather than
// from default style values. A selected tab's close button is always
// visible (style.TabCloseButtonMinWidthSelected is -1, imgui.cpp:1620).
//
// THE MUTATIONS THIS MUST BREAK (recorded in progress-imgui-binding.md):
//  ImGui::TreeNode() replaced by false, enterImGui() kept: TREENODE fails;
//  enterImGui() removed from setNextItemOpen: COVERAGE fails;
//  collapsingHeader(label, pVisible)'s write-back removed: HEADER fails;
//  the Leading/Trailing check removed: REJECTED fails (abort).
//
// REQUIRES: ui, ui-gpu, ui-imgui
// REQUIRES: display
//
// RUN: %with-display 300 sh -c 'HERMES_NODE_UI_TEST_HOOKS=1 HERMES_NODE_DEBUG_NATIVE=UI %hermes-node-ui %s > %t.out 2> %t.err'; echo "EXITCODE $?" | %FileCheck --check-prefix=CODE %s
// RUN: %FileCheck %s < %t.out
// RUN: %FileCheck --implicit-check-not='[sapp:error' --implicit-check-not='[sokol-' --check-prefix=TRACE %s < %t.err
// RUN: sh -c 'echo BEGIN; grep "imgui: usage error" %t.err; echo END' | %FileCheck --implicit-check-not='[sapp:error' --implicit-check-not='[sokol-' --check-prefix=USAGE %s

'use strict';

const harness = require('./fixtures/imgui/harness.js');

/** The frame padding the close-button cases push, in both axes. */
const PAD = 4;

function fail(line) {
  console.log(line);
  process.exit(1);
}

/**
 * The centre of the close button of an item with rectangle `r`; `size` is
 * the font size, getTextLineHeight() read inside the frame.
 */
function closeButtonCenter(r, size) {
  return [
    Math.floor(r.max[0] - PAD - size / 2),
    Math.floor(r.min[1] + PAD + size / 2),
  ];
}

async function main() {
  const h = await harness.open();
  const {imgui} = h;
  const calls = (name) => harness.calls(h, name);
  const rect = () => harness.rect(imgui);
  // The font size, read in each close-button case's frames.
  let fontSize = 0;

  function rejects(name, fn, code) {
    const before = calls(name);
    const e = harness.expectThrow(fn, code);
    if (calls(name) !== before)
      fail('CALLED ' + name + ' ' + code + ': ' + e.message);
    return e;
  }

  /** A draw function: `body` inside a 600x440 window at (20, 20). */
  const inWindow = (body) => () => {
    imgui.setNextWindowPos([20, 20]);
    imgui.setNextWindowSize([600, 440]);
    imgui.begin('trees');
    body();
    imgui.end();
  };

  /** Two frames, so that a new layout has settled before it is read. */
  async function settle(draw) {
    await h.run(draw);
    await h.run(draw);
  }

  // --- TREENODE --------------------------------------------------------
  {
    let open = false, childDrawn = false, closedFrames = 0;
    let nodeRect, childRect;
    const draw = inWindow(() => {
      open = imgui.treeNode('node');
      nodeRect = rect();
      childDrawn = false;
      if (open) {
        imgui.text('child');
        childRect = rect();
        childDrawn = true;
        imgui.treePop();
      } else {
        ++closedFrames;
      }
    });
    await settle(draw);
    const closedBefore = !open && !childDrawn;
    await harness.clickAt(h, draw, harness.center(nodeRect));
    await harness.until(h, draw, () => open, 'TREENODE open');
    const ok = closedBefore && closedFrames >= 2 && childDrawn &&
        childRect.min[1] >= nodeRect.max[1];
    console.log(ok ? 'TREENODE ok' :
                     'TREENODE WRONG ' + closedBefore + ' ' + childDrawn +
                         ' ' + JSON.stringify([nodeRect, childRect]));
  }

  // --- SETOPEN ---------------------------------------------------------
  {
    let preset, plain;
    await h.run(inWindow(() => {
      imgui.setNextItemOpen(true);
      preset = imgui.treeNode('preset');
      if (preset)
        imgui.treePop();
      plain = imgui.treeNode('plain');
      if (plain)
        imgui.treePop();
    }));
    console.log(preset === true && plain === false ?
                    'SETOPEN ok' :
                    'SETOPEN WRONG ' + preset + ' ' + plain);
  }

  // --- TREEEX ----------------------------------------------------------
  {
    let pushed, unpushed, refusedMessage = 'none';
    let popsAccepted = 0;
    await h.run(inWindow(() => {
      pushed = imgui.treeNodeEx('ex pushed', imgui.TreeNodeFlags_DefaultOpen);
      if (pushed) {
        const before = calls('treePop');
        imgui.treePop();
        popsAccepted = calls('treePop') - before;
      }
      unpushed = imgui.treeNodeEx(
          'ex unpushed',
          imgui.TreeNodeFlags_DefaultOpen |
              imgui.TreeNodeFlags_NoTreePushOnOpen);
      if (unpushed)
        refusedMessage =
            rejects('treePop', () => imgui.treePop(), 'ERR_IMGUI_USAGE')
                .message;
    }));
    const ok = pushed === true && popsAccepted === 1 && unpushed === true &&
        refusedMessage !== 'none';
    console.log(ok ? 'TREEEX ok' :
                     'TREEEX WRONG ' + pushed + ' ' + popsAccepted + ' ' +
                         unpushed + ' ' + refusedMessage);
  }

  // --- HEADER ----------------------------------------------------------
  {
    const visible = [true];
    let open = false, visibleResult = null, nullForm = null;
    let headerRect, visibleRect = null;
    const draw = inWindow(() => {
      fontSize = imgui.getTextLineHeight();
      imgui.pushStyleVar(imgui.StyleVar_FramePadding, [PAD, PAD]);
      open = imgui.collapsingHeader('h');
      headerRect = rect();
      const before = calls('collapsingHeader');
      visibleResult = imgui.collapsingHeader('hv', visible);
      // Drawn only while pVisible[0] is true (widgets:7479-7480).
      if (visible[0])
        visibleRect = rect();
      nullForm = imgui.collapsingHeader('hn', null);
      imgui.popStyleVar();
      if (calls('collapsingHeader') !== before + 2)
        fail('HEADER not called');
    });
    await settle(draw);
    const closedBefore = open === false;
    await harness.clickAt(h, draw, harness.center(headerRect));
    await harness.until(h, draw, () => open, 'HEADER open');
    await settle(draw);
    const shownBefore = visible[0];
    await harness.clickAt(h, draw, closeButtonCenter(visibleRect, fontSize));
    await harness.until(h, draw, () => !visible[0], 'HEADER hidden');
    await h.run(draw);
    const ok = closedBefore && open === true && shownBefore === true &&
        visible[0] === false && visibleResult === false && nullForm === false;
    console.log(ok ? 'HEADER ok' :
                     'HEADER WRONG ' + closedBefore + ' ' + open + ' ' +
                         shownBefore + ' ' + visible[0] + ' ' +
                         visibleResult + ' ' + nullForm);
  }

  // --- TABS ------------------------------------------------------------
  {
    let selected = [], barShown = false;
    let rectB;
    const draw = inWindow(() => {
      selected = [];
      barShown = imgui.beginTabBar('tabs');
      if (!barShown)
        return;
      if (imgui.beginTabItem('A')) {
        selected.push('A');
        imgui.text('page A');
        imgui.endTabItem();
      }
      // The tab's own rectangle is the last item right after its
      // beginTabItem (TabItemEx's ItemAdd, widgets:10957; the close button
      // restores it, widgets:11257-11259).
      const shownB = imgui.beginTabItem('B');
      rectB = rect();
      if (shownB) {
        selected.push('B');
        imgui.text('page B');
        imgui.endTabItem();
      }
      imgui.endTabBar();
    });
    await settle(draw);
    const before = selected.slice();
    await harness.clickAt(h, draw, harness.center(rectB));
    await harness.until(h, draw, () => selected[0] === 'B', 'TABS B');
    const ok = barShown && before.join() === 'A' && selected.join() === 'B';
    console.log(ok ? 'TABS ok' :
                     'TABS WRONG ' + barShown + ' ' + before + ' ' + selected);
  }

  // --- TABCLOSE --------------------------------------------------------
  {
    const pOpen = [true];
    let tabRect = null, shown = false;
    const draw = inWindow(() => {
      fontSize = imgui.getTextLineHeight();
      imgui.pushStyleVar(imgui.StyleVar_FramePadding, [PAD, PAD]);
      if (imgui.beginTabBar('closing')) {
        shown = imgui.beginTabItem('C', pOpen);
        if (shown) {
          tabRect = rect();
          imgui.endTabItem();
        }
        if (imgui.beginTabItem('D'))
          imgui.endTabItem();
        imgui.endTabBar();
      }
      imgui.popStyleVar();
    });
    await settle(draw);
    if (!shown || tabRect === null)
      fail('TABCLOSE C not selected');
    await harness.clickAt(h, draw, closeButtonCenter(tabRect, fontSize));
    await harness.until(h, draw, () => !pOpen[0], 'TABCLOSE closed');
    console.log('TABCLOSE ok');
  }

  // --- REJECTED --------------------------------------------------------
  {
    let accepted = 0;
    await h.run(inWindow(() => {
      if (!imgui.beginTabBar('rejected'))
        fail('REJECTED no tab bar');
      rejects('beginTabItem',
              () => imgui.beginTabItem(
                  'x', null,
                  imgui.TabItemFlags_Leading | imgui.TabItemFlags_Trailing),
              'ERR_OUT_OF_RANGE');
      const before = calls('beginTabItem');
      if (imgui.beginTabItem('lead', null, imgui.TabItemFlags_Leading))
        imgui.endTabItem();
      if (imgui.beginTabItem('trail', undefined, imgui.TabItemFlags_Trailing))
        imgui.endTabItem();
      accepted = calls('beginTabItem') - before;
      imgui.endTabBar();
    }));
    console.log(accepted === 2 ? 'REJECTED ok' :
                                 'REJECTED accepted ' + accepted);
  }

  harness.checkGroupCoverage(h, 'TreesTabs');
  process.exit(0);
}

main().catch((e) => {
  console.log('FAILED ' + (e && e.stack));
  process.exit(1);
});

// CHECK: {{^}}TREENODE ok{{$}}
// CHECK-NEXT: {{^}}SETOPEN ok{{$}}
// CHECK-NEXT: {{^}}TREEEX ok{{$}}
// CHECK-NEXT: {{^}}HEADER ok{{$}}
// CHECK-NEXT: {{^}}TABS ok{{$}}
// CHECK-NEXT: {{^}}TABCLOSE ok{{$}}
// CHECK-NEXT: {{^}}REJECTED ok{{$}}
// CHECK-NEXT: {{^}}COVERAGE TreesTabs ok{{$}}
// CODE: EXITCODE 0

// TRACE: {{^}}[ui] imgui: context created

// The one usage error of the run is TREEEX's refused treePop(), traced by
// its guard; USAGE reads only the usage-error lines, between a BEGIN and
// an END line, as one -NEXT chain, so any other one breaks it.
// USAGE: {{^}}BEGIN{{$}}
// USAGE-NEXT: {{^}}[ui] imgui: usage error: TreePop() with no tree node open{{$}}
// USAGE-NEXT: {{^}}END{{$}}
