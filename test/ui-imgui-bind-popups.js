// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// The Popups and tooltips group: a popup opened by openPopup and closed by
// closeCurrentPopup, a modal blocking a click behind it, a context popup
// opened by a right click, and the three ways of showing a tooltip; the
// group's "Rejected" line; and its coverage. Every popup and tooltip is an
// ImGui window of its own, so `__test.state().windows` is the read-back of
// what is showing: a popup is `##Popup_<id as %08x>` (BeginPopupEx,
// imgui.cpp:13438), its id being getID(strId) where it is begun (or the
// item's for a context popup with no strId, imgui.cpp:13616); a modal is
// its own name (imgui.cpp:13510); a tooltip is `##Tooltip_<nn>`
// (imgui.cpp:13075-13077).
//
// Cases, in the order they run, one output line each (`<CASE> ok`; any
// failure prints its own line and exits 1):
//
//  POPUP     beginPopup('p') returns false, and its window is not active,
//            until openPopup('p') (which returned true) in one frame; then
//            it returns true and its window is active; closeCurrentPopup()
//            inside it closes it.
//  MODAL     with beginPopupModal('modal') open (opened by openPopup), a
//            click on a button behind it does not press the button (the
//            click read back as down, then up); after closeCurrentPopup()
//            inside the modal, the same click presses it.
//            beginPopupModal('never opened', pOpen) with pOpen [true]
//            returns false and writes pOpen[0] = false.
//  CONTEXT   beginPopupContextItem() after button('ctx') returns false
//            until a right click on the button, then true, its window being
//            the popup of the button's own id; beginPopupContextItem('tc')
//            after text('label') opens on a right click on the text.
//  TOOLTIPS  setTooltip, setItemTooltip and beginTooltip/endTooltip each
//            make a `##Tooltip_` window active while the mouse is over the
//            item, and none is active with the mouse away. setTooltip and
//            beginTooltip are called only while isItemHovered() is true;
//            setItemTooltip is called every frame and makes its own hover
//            check.
//  REJECTED  beginPopupModal('') : ERR_INVALID_ARG_VALUE, not called.
//  COVERAGE  every Popups and tooltips function has reached ImGui.
//
// setItemTooltip's hover check uses style.HoverFlagsForTooltipMouse,
// `Stationary | DelayShort` (imgui.cpp:1660, 13115), whose delays,
// style.HoverStationaryDelay and style.HoverDelayShort (0.15 s each), are
// measured in ImGui's time, which advances by the real frame time. The
// test sets both to 0 with __test.setTooltipDelay(0), so the tooltip shows
// a frame or two after the mouse arrives, however long frames take.
//
// THE MUTATIONS THIS MUST BREAK (recorded in progress-imgui-binding.md):
//  ImGui::CloseCurrentPopup() removed, enterImGui() kept: POPUP fails;
//  enterImGui() removed from endPopup: COVERAGE fails;
//  ImGui::SetItemTooltip() removed: TOOLTIPS fails;
//  beginPopupModal's write-back of pOpen removed: MODAL fails;
//  beginPopupModal's empty-name check removed: REJECTED fails (abort).
//
// REQUIRES: ui, ui-gpu, ui-imgui
// REQUIRES: display
//
// RUN: %with-display 300 sh -c 'HERMES_NODE_UI_TEST_HOOKS=1 HERMES_NODE_DEBUG_NATIVE=UI %hermes-node-ui %s > %t.out 2> %t.err'; echo "EXITCODE $?" | %FileCheck --check-prefix=CODE %s
// RUN: %FileCheck %s < %t.out
// RUN: %FileCheck --implicit-check-not='[sapp:error' --implicit-check-not='[sokol-' --implicit-check-not='imgui: usage error' --check-prefix=TRACE %s < %t.err

'use strict';

const harness = require('./fixtures/imgui/harness.js');

const RIGHT = 1;
// An empty spot inside the test window.
const BLANK = [560, 420];

function fail(line) {
  console.log(line);
  process.exit(1);
}

/** The window name ImGui gives the popup of id `id`. */
function popupName(id) {
  return '##Popup_' + (id >>> 0).toString(16).padStart(8, '0');
}

function isActive(imgui, name) {
  return imgui.__test.state().windows.some((w) => w.name === name && w.active);
}

function tooltipActive(imgui) {
  return imgui.__test.state().windows.some(
      (w) => w.name.startsWith('##Tooltip_') && w.active);
}

async function main() {
  const h = await harness.open();
  const {imgui} = h;
  const calls = (name) => harness.calls(h, name);
  const rect = () => harness.rect(imgui);

  /** A draw function: `body` inside a 600x440 window at (20, 20). */
  const inWindow = (body) => () => {
    imgui.setNextWindowPos([20, 20]);
    imgui.setNextWindowSize([600, 440]);
    imgui.begin('popups');
    body();
    imgui.end();
  };

  async function settle(draw) {
    await h.run(draw);
    await h.run(draw);
  }

  // --- POPUP -----------------------------------------------------------
  {
    let openNow = false, opened = null, shown = false, everShown = false;
    let closeNow = false, name = null;
    const draw = inWindow(() => {
      name = popupName(imgui.getID('p'));
      if (openNow) {
        opened = imgui.openPopup('p');
        openNow = false;
      }
      shown = imgui.beginPopup('p');
      if (shown) {
        everShown = true;
        imgui.text('in the popup');
        if (closeNow) {
          imgui.closeCurrentPopup();
          closeNow = false;
        }
        imgui.endPopup();
      }
    });
    await settle(draw);
    const closedBefore = !shown && !isActive(imgui, name);
    openNow = true;
    await harness.until(h, draw, () => shown && isActive(imgui, name),
                        'POPUP open');
    closeNow = true;
    await harness.until(h, draw, () => !shown && !isActive(imgui, name),
                        'POPUP closed');
    const ok = closedBefore && opened === true && everShown;
    console.log(ok ? 'POPUP ok' :
                     'POPUP WRONG ' + closedBefore + ' ' + opened + ' ' +
                         everShown);
  }

  // --- MODAL -----------------------------------------------------------
  {
    let openNow = false, closeNow = false, modalShown = false;
    let presses = 0, behindRect;
    const draw = inWindow(() => {
      if (imgui.button('behind'))
        ++presses;
      behindRect = rect();
      if (openNow) {
        imgui.openPopup('modal');
        openNow = false;
      }
      // Away from the button, so that the click lands on the window behind
      // the modal and not on the modal itself.
      imgui.setNextWindowPos([380, 300]);
      modalShown = imgui.beginPopupModal('modal');
      if (modalShown) {
        imgui.text('modal');
        if (closeNow) {
          imgui.closeCurrentPopup();
          closeNow = false;
        }
        imgui.endPopup();
      }
    });
    await settle(draw);
    openNow = true;
    await harness.until(h, draw, () => modalShown && isActive(imgui, 'modal'),
                        'MODAL open');
    await h.run(draw);
    await harness.clickAt(h, draw, harness.center(behindRect));
    const blocked = presses;
    closeNow = true;
    await harness.until(h, draw, () => !modalShown && !isActive(imgui, 'modal'),
                        'MODAL closed');
    await harness.clickAt(h, draw, harness.center(behindRect));
    // pOpen of a modal that is not open is set to false (imgui.cpp:
    // 13495-13496) and written back, though the call returned false.
    const pOpen = [true];
    let neverShown;
    await h.run(inWindow(() => {
      neverShown = imgui.beginPopupModal('never opened', pOpen);
    }));
    console.log(blocked === 0 && presses === 1 && neverShown === false &&
                        pOpen[0] === false ?
                    'MODAL ok' :
                    'MODAL WRONG ' + blocked + ' ' + presses + ' ' +
                        neverShown + ' ' + pOpen[0]);
  }

  // --- CONTEXT ---------------------------------------------------------
  {
    let itemShown = false, itemEver = false, buttonName = null, buttonRect;
    let textShown = false, textName = null, textRect;
    const draw = inWindow(() => {
      imgui.button('ctx');
      buttonRect = rect();
      buttonName = popupName(imgui.getID('ctx'));
      itemShown = imgui.beginPopupContextItem();
      if (itemShown) {
        itemEver = true;
        imgui.text('item menu');
        imgui.endPopup();
      }
      imgui.text('label');
      textRect = rect();
      textName = popupName(imgui.getID('tc'));
      textShown = imgui.beginPopupContextItem('tc', 0);
      if (textShown) {
        imgui.text('text menu');
        imgui.endPopup();
      }
    });
    await settle(draw);
    const closedBefore = !itemShown && !textShown;
    await harness.clickAt(h, draw, harness.center(buttonRect), RIGHT);
    await harness.until(h, draw, () => itemShown && isActive(imgui, buttonName),
                        'CONTEXT item');
    // A click elsewhere closes it (ClosePopupsOverWindow on a click
    // outside), then a right click on the text opens the other one.
    await harness.clickAt(h, draw, BLANK);
    await harness.until(h, draw, () => !itemShown, 'CONTEXT item closed');
    await harness.clickAt(h, draw, harness.center(textRect), RIGHT);
    await harness.until(h, draw, () => textShown && isActive(imgui, textName),
                        'CONTEXT text');
    await harness.clickAt(h, draw, BLANK);
    await harness.until(h, draw, () => !textShown, 'CONTEXT text closed');
    console.log(closedBefore && itemEver ? 'CONTEXT ok' :
                                           'CONTEXT WRONG ' + closedBefore);
  }

  // --- TOOLTIPS --------------------------------------------------------
  {
    imgui.__test.setTooltipDelay(0);
    const rects = {};
    let mode = 'none';
    const draw = inWindow(() => {
      imgui.button('t set');
      rects.set = rect();
      if (mode === 'set' && imgui.isItemHovered())
        imgui.setTooltip('from setTooltip');
      imgui.button('t item');
      rects.item = rect();
      if (mode === 'item')
        imgui.setItemTooltip('from setItemTooltip');
      imgui.button('t begin');
      rects.begin = rect();
      if (mode === 'begin' && imgui.isItemHovered()) {
        if (!imgui.beginTooltip())
          fail('TOOLTIPS beginTooltip false');
        imgui.text('from beginTooltip');
        imgui.endTooltip();
      }
    });
    await settle(draw);
    const wrong = [];
    for (const m of ['set', 'item', 'begin']) {
      mode = m;
      await harness.moveTo(h, draw, BLANK);
      await h.run(draw);
      if (tooltipActive(imgui))
        wrong.push(m + ':away');
      await harness.moveTo(h, draw, harness.center(rects[m]));
      await harness.until(h, draw, () => tooltipActive(imgui),
                          'TOOLTIPS ' + m);
    }
    mode = 'none';
    await harness.moveTo(h, draw, BLANK);
    imgui.__test.setTooltipDelay(0.15);
    console.log(wrong.length === 0 ? 'TOOLTIPS ok' : 'TOOLTIPS WRONG ' + wrong);
  }

  // --- REJECTED --------------------------------------------------------
  {
    await h.run(inWindow(() => {
      const before = calls('beginPopupModal');
      harness.expectThrow(() => imgui.beginPopupModal(''),
                          'ERR_INVALID_ARG_VALUE');
      if (calls('beginPopupModal') !== before)
        fail('CALLED beginPopupModal');
    }));
    console.log('REJECTED ok');
  }

  harness.checkGroupCoverage(h, 'Popups');
  process.exit(0);
}

main().catch((e) => {
  console.log('FAILED ' + (e && e.stack));
  process.exit(1);
});

// CHECK: {{^}}POPUP ok{{$}}
// CHECK-NEXT: {{^}}MODAL ok{{$}}
// CHECK-NEXT: {{^}}CONTEXT ok{{$}}
// CHECK-NEXT: {{^}}TOOLTIPS ok{{$}}
// CHECK-NEXT: {{^}}REJECTED ok{{$}}
// CHECK-NEXT: {{^}}COVERAGE Popups ok{{$}}
// CODE: EXITCODE 0

// No case here is a usage error, so none is traced (the
// --implicit-check-not on the TRACE run).
// TRACE: {{^}}[ui] imgui: context created
