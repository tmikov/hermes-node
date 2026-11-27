// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// The Windows group: each function's behaviour, read back through the
// binding itself, captured pixels and injected input; its "Rejected"
// lines; and the group's coverage.
//
// Cases, in the order they run, one output line each (`<CASE> ok`; any
// failure prints its own line and exits 1):
//
//  POS       setNextWindowPos([40, 50]) and setNextWindowSize([300, 200]):
//            getWindowPos() and getWindowSize() read them back exactly,
//            into a new array and into a passed one (returned `=== out`).
//  CLOSE     begin('closable', open): a click on the title bar's close
//            button (located from the window's right end, the frame
//            height and a pushed FramePadding) writes open[0] = false, and
//            the window is no longer active once it is not submitted.
//  HOVER     isWindowHovered() is true with the mouse inside the window,
//            false outside it.
//  FOCUS     two windows: a click in one makes isWindowFocused() true in
//            it and false in the other, and the other way round.
//  SCROLL    a window holding a child taller than itself has
//            getScrollMaxY() > 0 and getScrollY() 0; setScrollHereY(1) on
//            the last item makes getScrollY() === getScrollMaxY() on the
//            next frame.
//  AVAIL     getContentRegionAvail() at the start of a 300x200 window with
//            no title bar and a WindowPadding of 10 is [280, 180].
//  BGALPHA   a WindowBg of alpha 0.25 overridden by setNextWindowBgAlpha(1):
//            the window's interior is exactly that colour, opaque; outside
//            it, CLEAR.
//  REJECTED  begin(''): ERR_INVALID_ARG_VALUE. beginChild's three flag
//            combinations (WindowFlags_AlwaysAutoResize; ChildFlags_
//            AlwaysAutoResize with ResizeX or ResizeY; it without
//            AutoResizeX/Y): ERR_OUT_OF_RANGE, while a valid
//            AlwaysAutoResize | AutoResizeY is accepted.
//            setScrollHereY outside 0..1 (NaN included): ERR_OUT_OF_RANGE.
//            None reaches ImGui. isWindowHovered with an item-only flag is
//            ImGui's own recoverable error: ERR_IMGUI_USAGE, after the call.
//  SIZES     a size component of 2^31 or more, to setNextWindowSize or
//            beginChild: ERR_OUT_OF_RANGE, not called. The largest float
//            below 2^31 is accepted by both: getWindowSize() reads it back
//            in the window and in the child.
//  POSITION  a setNextWindowPos component of 2^31 or more, or below
//            -2^31: ERR_OUT_OF_RANGE, not called, each component and
//            sign. -2^31 and the largest float below 2^31 accepted: two
//            32 x 32 windows begun there read their positions back.
//  COVERAGE  every Windows function has reached ImGui.
//
// THE MUTATIONS THIS MUST BREAK (recorded in progress-imgui-binding.md):
//  ImGui::SetNextWindowBgAlpha() removed, enterImGui() kept: BGALPHA fails;
//  enterImGui() removed from getScrollMaxY: COVERAGE fails;
//  begin()'s write-back of pOpen removed: CLOSE fails;
//  setScrollHereY's ratio check removed: REJECTED fails (ImGui's plain
//  assertion aborts);
//  begin('')'s check removed: REJECTED fails (ImGui aborts);
//  each of beginChild's three flag checks removed: REJECTED fails;
//  getWindowPos(out) returning a new array instead of out: POS fails;
//  setNextWindowSize's or beginChild's size bound removed: SIZES fails;
//  setNextWindowPos's position bound removed, or one-sided: POSITION fails.
//
// REQUIRES: ui, ui-gpu, ui-imgui
// REQUIRES: display
//
// RUN: %with-display 300 sh -c 'HERMES_NODE_UI_TEST_HOOKS=1 HERMES_NODE_DEBUG_NATIVE=UI %hermes-node-ui %s %t.ppm > %t.out 2> %t.err'; echo "EXITCODE $?" | %FileCheck --check-prefix=CODE %s
// RUN: %FileCheck %s < %t.out
// RUN: %FileCheck --implicit-check-not='[sapp:error' --implicit-check-not='[sokol-' --check-prefix=TRACE %s < %t.err

'use strict';

const harness = require('./fixtures/imgui/harness.js');

const capturePath = process.argv[2];
const OUTSIDE = [600, 440];

// 2^31, where ImGui's (int) conversion of a size becomes undefined, and
// the largest float below it.
const INT_LIMIT = 2147483648;
const LARGEST_BELOW_LIMIT = 2147483520;

function fail(line) {
  console.log(line);
  process.exit(1);
}

function same(a, b) {
  return a.length === b.length && a.every((v, i) => v === b[i]);
}

async function main() {
  const h = await harness.open();
  const {imgui} = h;
  const calls = (name) => harness.calls(h, name);

  /** `fn` must throw `code` and leave `name`'s call count unchanged. */
  function rejects(name, fn, code) {
    const before = calls(name);
    const e = harness.expectThrow(fn, code);
    if (calls(name) !== before)
      fail('CALLED ' + name + ' ' + code + ': ' + e.message);
    return e;
  }

  /** A window at `pos` of `size`, with `body` run inside it. */
  function windowAt(name, pos, size, body, flags = 0) {
    imgui.setNextWindowPos(pos);
    imgui.setNextWindowSize(size);
    imgui.begin(name, null, flags);
    body();
    imgui.end();
  }

  // --- POS -------------------------------------------------------------
  {
    let pos, size, posOut, sizeOut, posRet, sizeRet;
    const outPos = [];
    const outSize = [-1, -1];
    await h.run(() => windowAt('pos', [40, 50], [300, 200], () => {
      pos = imgui.getWindowPos();
      size = imgui.getWindowSize();
      posRet = imgui.getWindowPos(outPos);
      sizeRet = imgui.getWindowSize(outSize);
      posOut = outPos.slice();
      sizeOut = outSize.slice();
    }));
    const ok = Array.isArray(pos) && same(pos, [40, 50]) &&
        same(size, [300, 200]) && posRet === outPos && sizeRet === outSize &&
        same(posOut, [40, 50]) && same(sizeOut, [300, 200]);
    console.log(
        ok ? 'POS ok' :
             'POS WRONG pos=' + pos + ' size=' + size + ' out=' + posOut +
                 '/' + sizeOut + ' same=' + (posRet === outPos) + '/' +
                 (sizeRet === outSize));
  }

  // --- CLOSE -----------------------------------------------------------
  {
    const open = [true];
    let closeAt = null;
    const draw = () => {
      if (!open[0])
        return;
      // A known FramePadding, so that the close button's place
      // (RenderWindowTitleBarContents, imgui.cpp:7865: right end minus
      // FramePadding.x minus the font size, FramePadding.y below the top)
      // is computed rather than assumed.
      imgui.pushStyleVar(imgui.StyleVar_FramePadding, [4, 4]);
      imgui.setNextWindowPos([40, 50]);
      imgui.setNextWindowSize([300, 200]);
      imgui.begin('closable', open);
      const p = imgui.getWindowPos();
      const s = imgui.getWindowSize();
      const font = imgui.getTextLineHeight();
      closeAt = [
        Math.floor(p[0] + s[0] - 4 - font / 2),
        Math.floor(p[1] + 4 + font / 2),
      ];
      imgui.end();
      imgui.popStyleVar();
    };
    await h.run(draw);
    await h.run(draw);
    await harness.clickAt(h, draw, closeAt);
    await h.run(draw);
    const active = imgui.__test.state().windows.some(
        (w) => w.name === 'closable' && w.active);
    console.log(
        open[0] === false && !active ?
            'CLOSE ok' :
            'CLOSE WRONG open=' + open[0] + ' active=' + active + ' at=' +
                closeAt);
  }

  // --- HOVER -----------------------------------------------------------
  {
    let hovered = null;
    const draw = () => windowAt('hover', [40, 50], [300, 200], () => {
      hovered = imgui.isWindowHovered();
    });
    await harness.moveTo(h, draw, [190, 150]);
    await h.run(draw);
    const inside = hovered;
    await harness.moveTo(h, draw, OUTSIDE);
    await h.run(draw);
    const outside = hovered;
    console.log(
        inside === true && outside === false ?
            'HOVER ok' :
            'HOVER WRONG inside=' + inside + ' outside=' + outside);
  }

  // --- FOCUS -----------------------------------------------------------
  {
    const focused = {};
    const draw = () => {
      windowAt('focus a', [40, 50], [200, 150], () => {
        focused.a = imgui.isWindowFocused();
      });
      windowAt('focus b', [340, 50], [200, 150], () => {
        focused.b = imgui.isWindowFocused();
      });
    };
    await harness.clickAt(h, draw, [440, 150]);
    await h.run(draw);
    const afterB = Object.assign({}, focused);
    await harness.clickAt(h, draw, [140, 150]);
    await h.run(draw);
    const afterA = Object.assign({}, focused);
    const ok = afterB.a === false && afterB.b === true &&
        afterA.a === true && afterA.b === false;
    console.log(
        ok ? 'FOCUS ok' :
             'FOCUS WRONG afterB=' + JSON.stringify(afterB) + ' afterA=' +
                 JSON.stringify(afterA));
  }

  // --- SCROLL ----------------------------------------------------------
  {
    let scrollY = -1;
    let scrollMaxY = -1;
    let scrollHere = false;
    const draw = () => windowAt('scroll', [40, 50], [300, 200], () => {
      // Scrolling is applied in Begin(), so these read what the previous
      // frame asked for.
      scrollY = imgui.getScrollY();
      scrollMaxY = imgui.getScrollMaxY();
      imgui.beginChild('tall', [0, 600]);
      imgui.endChild();
      imgui.text('last');
      if (scrollHere)
        imgui.setScrollHereY(1);
    });
    // A new window is sized over its first frames; the content size is
    // the previous frame's, so getScrollMaxY() becomes positive within a
    // few.
    for (let i = 0; i < 10 && !(scrollMaxY > 0); ++i)
      await h.run(draw);
    const before = [scrollY, scrollMaxY];
    scrollHere = true;
    await h.run(draw);
    scrollHere = false;
    await h.run(draw);
    const ok = before[1] > 0 && before[0] === 0 && scrollMaxY > 0 &&
        scrollY === scrollMaxY;
    console.log(
        ok ? 'SCROLL ok' :
             'SCROLL WRONG before=' + before + ' after=' + scrollY + '/' +
                 scrollMaxY);
  }

  // --- AVAIL -----------------------------------------------------------
  {
    let avail, availRet;
    const out = [];
    await h.run(() => {
      imgui.pushStyleVar(imgui.StyleVar_WindowPadding, [10, 10]);
      windowAt('avail', [40, 50], [300, 200], () => {
        avail = imgui.getContentRegionAvail();
        availRet = imgui.getContentRegionAvail(out);
      }, imgui.WindowFlags_NoTitleBar);
      imgui.popStyleVar();
    });
    const ok = same(avail, [280, 180]) && availRet === out &&
        same(out, [280, 180]);
    console.log(ok ? 'AVAIL ok' : 'AVAIL WRONG ' + avail + ' out=' + out);
  }

  // --- BGALPHA ---------------------------------------------------------
  {
    const draw = () => {
      imgui.pushStyleColor(imgui.Col_WindowBg, [0, 0, 1, 0.25]);
      imgui.setNextWindowBgAlpha(1);
      windowAt('bg', [100, 100], [200, 150], () => {},
               imgui.WindowFlags_NoTitleBar);
      imgui.popStyleColor();
    };
    await h.run(draw);
    const image = await harness.capture(h, draw, capturePath);
    const inside = [[200, 175], [120, 120], [280, 230]];
    const outside = [[20, 20], OUTSIDE, [90, 175], [310, 175]];
    const wrong = [];
    for (const [x, y] of inside) {
      if (!same(harness.pixelAt(image, x, y), [0, 0, 255]))
        wrong.push(x + ',' + y + '=' + harness.pixelAt(image, x, y));
    }
    for (const [x, y] of outside) {
      if (!same(harness.pixelAt(image, x, y), harness.CLEAR))
        wrong.push(x + ',' + y + '=' + harness.pixelAt(image, x, y));
    }
    console.log(
        wrong.length === 0 ? 'BGALPHA ok' : 'BGALPHA WRONG ' + wrong.join(' '));
  }

  // --- REJECTED --------------------------------------------------------
  {
    let emptyName, accepted = false, usage;
    await h.run(() => windowAt('rejected', [40, 50], [300, 200], () => {
      emptyName = rejects('begin', () => imgui.begin(''),
                          'ERR_INVALID_ARG_VALUE').message;
      rejects('beginChild',
              () => imgui.beginChild('c', [0, 0], 0,
                                     imgui.WindowFlags_AlwaysAutoResize),
              'ERR_OUT_OF_RANGE');
      for (const resize of [imgui.ChildFlags_ResizeX, imgui.ChildFlags_ResizeY])
        rejects('beginChild',
                () => imgui.beginChild('c', [0, 0],
                                       imgui.ChildFlags_AlwaysAutoResize |
                                           imgui.ChildFlags_AutoResizeY |
                                           resize),
                'ERR_OUT_OF_RANGE');
      rejects('beginChild',
              () => imgui.beginChild('c', [0, 0],
                                     imgui.ChildFlags_AlwaysAutoResize),
              'ERR_OUT_OF_RANGE');
      const before = calls('beginChild');
      imgui.beginChild('c', [0, 0],
                       imgui.ChildFlags_AlwaysAutoResize |
                           imgui.ChildFlags_AutoResizeY);
      imgui.endChild();
      accepted = calls('beginChild') === before + 1;
      for (const ratio of [1.5, -0.25, NaN])
        rejects('setScrollHereY', () => imgui.setScrollHereY(ratio),
                'ERR_OUT_OF_RANGE');
      imgui.setScrollHereY(0);
      // ImGui's own check (imgui.cpp:9225): the call is made, then thrown.
      const hoveredBefore = calls('isWindowHovered');
      usage = harness.expectThrow(
          () => imgui.isWindowHovered(imgui.HoveredFlags_AllowWhenOverlapped),
          'ERR_IMGUI_USAGE').message;
      if (calls('isWindowHovered') !== hoveredBefore + 1)
        fail('REJECTED isWindowHovered not called');
    }));
    if (!accepted)
      fail('REJECTED valid AlwaysAutoResize not accepted');
    console.log('REJECTED ok');
    console.log('REJECTED message ' + emptyName);
    console.log('REJECTED usage ' + usage);
  }

  // --- SIZES -----------------------------------------------------------
  {
    // SetWindowSize converts each positive component with IM_TRUNC
    // (imgui.cpp:9361, 9365), undefined from 2^31 up; beginChild's size
    // reaches it through CalcItemSize (imgui.cpp:12615-12622) and
    // SetNextWindowSize (imgui.cpp:6961).
    const range = 'ERR_OUT_OF_RANGE';
    let message, outer, child;
    await h.run(() => windowAt('sizes', [40, 50], [300, 200], () => {
      for (const s of [[INT_LIMIT, 100], [100, INT_LIMIT], [3e38, 3e38]]) {
        message = rejects('setNextWindowSize', () => imgui.setNextWindowSize(s),
                          range).message;
        rejects('beginChild', () => imgui.beginChild('c', s), range);
      }
    }));
    const L = LARGEST_BELOW_LIMIT;
    await h.run(() => {
      imgui.setNextWindowPos([40, 50]);
      imgui.setNextWindowSize([L, L]);
      imgui.begin('huge');
      outer = imgui.getWindowSize();
      imgui.beginChild('c', [L, 100]);
      child = imgui.getWindowSize();
      imgui.endChild();
      imgui.end();
    });
    const ok = same(outer, [L, L]) && same(child, [L, 100]);
    console.log(ok ? 'SIZES ok' : 'SIZES WRONG ' + outer + ' / ' + child);
    console.log('SIZES message ' + message);
  }

  // --- POSITION --------------------------------------------------------
  {
    // With the default pivot, Begin() passes the position to SetWindowPos
    // as it is (imgui.cpp:8155-8169), which truncates it with ImTrunc
    // (imgui.cpp:9311): undefined outside -2^31 .. 2^31, both signs.
    const range = 'ERR_OUT_OF_RANGE';
    const BELOW_MIN = -2147483904; // the float next below -2^31
    let message, low, high;
    await h.run(() => windowAt('position', [40, 50], [300, 200], () => {
      for (const p of [[INT_LIMIT, 0], [0, INT_LIMIT], [BELOW_MIN, 0],
                       [0, BELOW_MIN], [3e38, 0], [0, -3e38]])
        message = rejects('setNextWindowPos', () => imgui.setNextWindowPos(p),
                          range).message;
    }));
    // The extremes accepted. A 32 x 32 window keeps position + size from
    // rounding up to 2^31 (the float spacing there is 128).
    const L = LARGEST_BELOW_LIMIT, M = -INT_LIMIT;
    await h.run(() => {
      for (const [name, p] of [['low', [M, M]], ['high', [L, L]]]) {
        imgui.setNextWindowPos(p);
        imgui.setNextWindowSize([32, 32]);
        imgui.begin(name, null, imgui.WindowFlags_NoTitleBar);
        if (name === 'low')
          low = imgui.getWindowPos();
        else
          high = imgui.getWindowPos();
        imgui.end();
      }
    });
    const ok = same(low, [M, M]) && same(high, [L, L]);
    console.log(ok ? 'POSITION ok' : 'POSITION WRONG ' + low + ' / ' + high);
    console.log('POSITION message ' + message);
  }

  harness.checkGroupCoverage(h, 'Windows');
  process.exit(0);
}

main().catch((e) => {
  console.log('FAILED ' + (e && e.stack));
  process.exit(1);
});

// CHECK: {{^}}POS ok{{$}}
// CHECK-NEXT: {{^}}CLOSE ok{{$}}
// CHECK-NEXT: {{^}}HOVER ok{{$}}
// CHECK-NEXT: {{^}}FOCUS ok{{$}}
// CHECK-NEXT: {{^}}SCROLL ok{{$}}
// CHECK-NEXT: {{^}}AVAIL ok{{$}}
// CHECK-NEXT: {{^}}BGALPHA ok{{$}}
// CHECK-NEXT: {{^}}REJECTED ok{{$}}
// CHECK-NEXT: {{^}}REJECTED message imgui.begin(): name must not be empty{{$}}
// CHECK-NEXT: {{^}}REJECTED usage imgui.isWindowHovered(): Invalid flags for IsWindowHovered()!{{$}}
// CHECK-NEXT: {{^}}SIZES ok{{$}}
// CHECK-NEXT: {{^}}SIZES message imgui.setNextWindowSize(): size[0] must be less than 2147483648; got 3e+38{{$}}
// CHECK-NEXT: {{^}}POSITION ok{{$}}
// CHECK-NEXT: {{^}}POSITION message imgui.setNextWindowPos(): pos[1] must be at least -2147483648 and less than 2147483648; got -3e+38{{$}}
// CHECK-NEXT: {{^}}COVERAGE Windows ok{{$}}
// CODE: EXITCODE 0

// TRACE: {{^}}[ui] imgui: context created
// TRACE: {{^}}[ui] imgui: usage error: Invalid flags for IsWindowHovered()!{{$}}
