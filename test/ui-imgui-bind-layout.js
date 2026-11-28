// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// The Layout, IDs, Text and Item and input queries groups: geometry read
// back from item rectangles, IDs compared, a captured glyph colour, and
// queries that follow injected input; each group's "Rejected" lines; and
// the four groups' coverage.
//
// Cases, in the order they run, one output line each (`<CASE> ok`; any
// failure prints its own line and exits 1):
//
//  SAMELINE  sameLine(): the second button's min[1] equals the first's,
//            and it starts right of the first one's max[0].
//  INDENT    indent(20) moves the next item's min[0] by 20; unindent(20)
//            moves it back.
//  GROUP     beginGroup()/endGroup() around two buttons make one item
//            whose rectangle is the bounding box of both.
//  DUMMY     dummy([30, 40]) is an item of exactly that size.
//  WIDTH     setNextItemWidth(123) gives a slider 123 wide; pushItemWidth(90)
//            gives the next two sliders 90; after popItemWidth() a slider
//            has the default width again (not 90).
//  ITEMWIDTH an item width of 2^31 or more, to setNextItemWidth or
//            pushItemWidth: ERR_OUT_OF_RANGE, not called. The largest
//            float below 2^31 is accepted by both, and a slider is drawn
//            that wide; -3e38 makes a slider 1 wide.
//  CURSOR    setCursorScreenPos([200, 150]): getCursorScreenPos() reads it
//            (also into a passed array), and the next item's min is there.
//  SPACING   spacing() and newLine() each widen the gap between two text
//            lines; separator() and separatorText() are items wider than
//            100 between their neighbours.
//  TEXTSIZE  calcTextSize('abc') equals text('abc')'s rectangle; with
//            hideTextAfterDoubleHash 'abc##x' measures as 'abc', without it
//            wider; a wrap width of 1 makes 'aa bb cc' taller than a line.
//  HEIGHTS   getTextLineHeight() is text('abc')'s height, and is less than
//            getTextLineHeightWithSpacing() and getFrameHeight();
//            alignTextToFramePadding() lowers the next text by half the
//            difference between the frame height and the line height.
//  TEXTS     text, textColored, textDisabled, textWrapped, bulletText and
//            labelText each produce a non-empty item starting at or below
//            the previous one's bottom; a long textWrapped is several lines
//            and narrower than the window; bulletText is wider than its
//            text, labelText wider than its value; a '%' in the text is
//            drawn as it is (same width as an 'x').
//  GREEN     textColored([0, 1, 0, 1], ...) puts exactly (0, 255, 0) pixels
//            inside its rectangle, found by scanning the captured rows;
//            text() beside it puts none.
//  IDS       one label under pushID('a') and pushID('b') gives two getID()
//            results; pushID(7) and pushID('7') differ; getID(7) and
//            getID('7') differ; popID() restores the outer ID; IDs are
//            integers in 0 .. 2^32-1.
//  HOVERED   isItemHovered() follows the mouse onto and off a button.
//  CLICK     a click on the button: isItemClicked(), isMouseClicked(0) and
//            isItemActive() were each true in some frame; isMouseDown(0)
//            was true while held and is false after.
//  DOUBLE    with the double-click time made unbounded
//            (__test.setDoubleClickTime), two clicks: isMouseDoubleClicked(0)
//            false through the first, true in exactly one frame of the
//            second.
//  MOUSEPOS  getMousePos() is the injected position (also into `out`).
//  DRAG      a drag from the button by (40, 30): getMouseDragDelta() while
//            held reads [40, 30] (also into `out`).
//  KEY       Key_A down: isKeyDown(Key_A) true; up: false; one press
//            counted by isKeyPressed(Key_A, false), at least one by
//            isKeyPressed(Key_A).
//  FOCUSHERE setKeyboardFocusHere() before a button in one window, while
//            another has focus: the first window becomes the focused one.
//            (The text-field half, where focus also activates the field,
//            is test/ui-imgui-bind-input-text.js FOCUS.)
//  DEFAULTFOCUS a new 300x200 window holding dummy([10, 600]), a button and
//            setItemDefaultFocus(): the window scrolls to the button
//            (getScrollY() > 0, waited for with harness.until).
//  REJECTED  mouse buttons outside 0..4, the keys that are not keys
//            (Key_None, Key_NamedKey_END, Key_NamedKey_COUNT) and a
//            combined modifier, and setKeyboardFocusHere(-2) and
//            (2147483647): ERR_OUT_OF_RANGE, not called; button 4,
//            Mod_Ctrl and offsets -1 and 2147483646 accepted. isItemHovered with a window-only flag is ImGui's own
//            recoverable error: ERR_IMGUI_USAGE, the call counted.
//  COVERAGE  every Layout, IDs, Text and Queries function has reached ImGui.
//            isItemDeactivatedAfterEdit is only called here; its
//            behaviour is test/ui-imgui-bind-input-text.js DEACTIVATE.
//
// THE MUTATIONS THIS MUST BREAK: the full list, with what each did, is the
// Mutations table of docs/superpowers/plans/progress-imgui-binding.md
// (the rows for this file: Task 3, its review, and Task 8's widths).
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

// Sokol's key code for A, and ImGui's left mouse button (also Sokol's).
const SAPP_KEYCODE_A = 65;
const LEFT = 0;

// 2^31, where ImGui's (int) conversion of a width becomes undefined, and
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

function width(r) {
  return r.max[0] - r.min[0];
}

function height(r) {
  return r.max[1] - r.min[1];
}

async function main() {
  const h = await harness.open();
  const {imgui} = h;
  const calls = (name) => harness.calls(h, name);
  const rect = () => harness.rect(imgui);

  function rejects(name, fn, code) {
    const before = calls(name);
    const e = harness.expectThrow(fn, code);
    if (calls(name) !== before)
      fail('CALLED ' + name + ' ' + code + ': ' + e.message);
    return e;
  }

  /** One frame with `body` inside a 600x440 window at (20, 20). */
  const inWindow = (body) => h.run(() => {
    imgui.setNextWindowPos([20, 20]);
    imgui.setNextWindowSize([600, 440]);
    imgui.begin('layout');
    body();
    imgui.end();
  });

  // --- SAMELINE --------------------------------------------------------
  {
    let a, b;
    await inWindow(() => {
      imgui.button('a');
      a = rect();
      imgui.sameLine();
      imgui.button('b');
      b = rect();
    });
    const ok = b.min[1] === a.min[1] && b.min[0] > a.max[0];
    console.log(ok ? 'SAMELINE ok' : 'SAMELINE WRONG ' + JSON.stringify([a, b]));
  }

  // --- INDENT ----------------------------------------------------------
  {
    let r0, r1, r2;
    await inWindow(() => {
      imgui.text('x');
      r0 = rect();
      imgui.indent(20);
      imgui.text('y');
      r1 = rect();
      imgui.unindent(20);
      imgui.text('z');
      r2 = rect();
    });
    const ok = r1.min[0] === r0.min[0] + 20 && r2.min[0] === r0.min[0];
    console.log(
        ok ? 'INDENT ok' :
             'INDENT WRONG ' + r0.min[0] + ' ' + r1.min[0] + ' ' + r2.min[0]);
  }

  // --- GROUP -----------------------------------------------------------
  {
    let g1, g2, group;
    await inWindow(() => {
      imgui.beginGroup();
      imgui.button('g1');
      g1 = rect();
      imgui.button('wider g2');
      g2 = rect();
      imgui.endGroup();
      group = rect();
    });
    const ok = same(group.min, g1.min) && group.max[0] === g2.max[0] &&
        group.max[1] === g2.max[1] && g2.max[0] > g1.max[0];
    console.log(ok ? 'GROUP ok' : 'GROUP WRONG ' + JSON.stringify([g1, g2, group]));
  }

  // --- DUMMY -----------------------------------------------------------
  {
    let r;
    await inWindow(() => {
      imgui.dummy([30, 40]);
      r = rect();
    });
    console.log(
        width(r) === 30 && height(r) === 40 ? 'DUMMY ok' :
                                              'DUMMY WRONG ' + JSON.stringify(r));
  }

  // --- WIDTH -----------------------------------------------------------
  {
    // A slider with a hidden label: its item is the group of its three
    // fields, exactly the item width (PushMultiItemsWidths).
    const v = [0, 0, 0];
    const widths = [];
    await inWindow(() => {
      imgui.sliderFloat3('##default', v, 0, 1);
      widths.push(width(rect()));
      imgui.setNextItemWidth(123);
      imgui.sliderFloat3('##next', v, 0, 1);
      widths.push(width(rect()));
      imgui.pushItemWidth(90);
      imgui.sliderFloat3('##pushed1', v, 0, 1);
      widths.push(width(rect()));
      imgui.sliderFloat3('##pushed2', v, 0, 1);
      widths.push(width(rect()));
      imgui.popItemWidth();
      imgui.sliderFloat3('##popped', v, 0, 1);
      widths.push(width(rect()));
    });
    const ok = widths[1] === 123 && widths[2] === 90 && widths[3] === 90 &&
        widths[4] === widths[0] && widths[0] !== 90 && widths[0] !== 123;
    console.log(ok ? 'WIDTH ok' : 'WIDTH WRONG ' + widths);
  }

  // --- ITEMWIDTH -------------------------------------------------------
  {
    // CalcItemWidth converts a positive item width with IM_TRUNC,
    // (float)(int)w (imgui.cpp:12601), undefined from 2^31 up; a negative
    // one is first replaced by max(1, avail + w) (imgui.cpp:12597-12600).
    const v = [0];
    let message, next, pushed, negative;
    await h.run(() => {
      imgui.setNextWindowPos([20, 20]);
      imgui.setNextWindowSize([600, 440]);
      imgui.begin('itemwidth');
      for (const w of [INT_LIMIT, 3e38]) {
        message = rejects('setNextItemWidth', () => imgui.setNextItemWidth(w),
                          'ERR_OUT_OF_RANGE').message;
        rejects('pushItemWidth', () => imgui.pushItemWidth(w),
                'ERR_OUT_OF_RANGE');
      }
      imgui.setNextItemWidth(LARGEST_BELOW_LIMIT);
      imgui.sliderFloat('##next', v, 0, 1);
      next = rect();
      imgui.pushItemWidth(LARGEST_BELOW_LIMIT);
      imgui.sliderFloat('##pushed', v, 0, 1);
      pushed = rect();
      imgui.popItemWidth();
      imgui.setNextItemWidth(-3e38);
      imgui.sliderFloat('##negative', v, 0, 1);
      negative = rect();
      imgui.end();
    });
    // The item's max is min + width rounded to a float.
    const want = (r) => Math.fround(r.min[0] + LARGEST_BELOW_LIMIT) - r.min[0];
    const ok = width(next) === want(next) && width(pushed) === want(pushed) &&
        width(next) > 2e9 && width(negative) === 1;
    console.log(ok ? 'ITEMWIDTH ok' :
                     'ITEMWIDTH WRONG ' + JSON.stringify([next, pushed, negative]));
    console.log('ITEMWIDTH message ' + message);
  }

  // --- CURSOR ----------------------------------------------------------
  {
    let pos, ret, r;
    const out = [];
    await inWindow(() => {
      imgui.setCursorScreenPos([200, 150]);
      pos = imgui.getCursorScreenPos();
      ret = imgui.getCursorScreenPos(out);
      imgui.button('c');
      r = rect();
    });
    const ok = same(pos, [200, 150]) && ret === out && same(out, [200, 150]) &&
        same(r.min, [200, 150]);
    console.log(
        ok ? 'CURSOR ok' : 'CURSOR WRONG ' + pos + ' ' + out + ' ' + r.min);
  }

  // --- SPACING ---------------------------------------------------------
  {
    const gaps = {};
    let sep, sepText, above, below;
    await inWindow(() => {
      const gap = (between) => {
        imgui.text('a');
        const r0 = rect();
        between();
        imgui.text('b');
        return rect().min[1] - r0.max[1];
      };
      gaps.plain = gap(() => {});
      gaps.spacing = gap(() => imgui.spacing());
      gaps.newLine = gap(() => imgui.newLine());
      imgui.text('above');
      above = rect();
      imgui.separator();
      sep = rect();
      imgui.separatorText('S');
      sepText = rect();
      imgui.text('below');
      below = rect();
    });
    const ok = gaps.spacing > gaps.plain && gaps.newLine > gaps.plain &&
        width(sep) > 100 && sep.min[1] >= above.max[1] &&
        width(sepText) > 100 && sepText.min[1] >= sep.max[1] &&
        below.min[1] >= sepText.max[1];
    console.log(
        ok ? 'SPACING ok' :
             'SPACING WRONG ' + JSON.stringify({gaps, above, sep, sepText,
                                                below}));
  }

  // --- TEXTSIZE --------------------------------------------------------
  {
    let size, ret, r, hidden, shown, wrapped, line;
    const out = [];
    await inWindow(() => {
      size = imgui.calcTextSize('abc');
      imgui.text('abc');
      r = rect();
      ret = imgui.calcTextSize('abc', false, -1, out);
      hidden = imgui.calcTextSize('abc##x', true);
      shown = imgui.calcTextSize('abc##x');
      wrapped = imgui.calcTextSize('aa bb cc', false, 1);
      line = imgui.getTextLineHeight();
    });
    const ok = same(size, [width(r), height(r)]) && ret === out &&
        same(out, size) && same(hidden, size) && shown[0] > size[0] &&
        wrapped[1] > line;
    console.log(
        ok ? 'TEXTSIZE ok' :
             'TEXTSIZE WRONG ' + JSON.stringify({size, r, out, hidden, shown,
                                                 wrapped, line}));
  }

  // --- HEIGHTS ---------------------------------------------------------
  {
    let line, withSpacing, frameH, textH, cursorY, alignedY;
    await inWindow(() => {
      line = imgui.getTextLineHeight();
      withSpacing = imgui.getTextLineHeightWithSpacing();
      frameH = imgui.getFrameHeight();
      imgui.text('abc');
      textH = height(rect());
      cursorY = imgui.getCursorScreenPos()[1];
      imgui.alignTextToFramePadding();
      imgui.text('aligned');
      alignedY = rect().min[1];
    });
    const ok = line > 0 && line === textH && line < withSpacing &&
        line < frameH && alignedY === cursorY + (frameH - line) / 2;
    console.log(
        ok ? 'HEIGHTS ok' :
             'HEIGHTS WRONG ' + JSON.stringify({line, withSpacing, frameH,
                                                textH, cursorY, alignedY}));
  }

  // --- TEXTS -----------------------------------------------------------
  {
    // rect() reads the LAST item, so a variant that drew nothing would be
    // read as the previous one's rectangle: each must start at or below
    // the previous one's bottom, and each has a property of its own.
    const order = ['text', 'textColored', 'textDisabled', 'textWrapped',
                   'bulletText', 'labelText'];
    const rects = {};
    const long = 'word '.repeat(200);
    let percent, plain, line, bulletTextW, valueW;
    await inWindow(() => {
      line = imgui.getTextLineHeight();
      bulletTextW = imgui.calcTextSize('bullet')[0];
      valueW = imgui.calcTextSize('value')[0];
      imgui.text('text');
      rects.text = rect();
      imgui.textColored([1, 0, 0, 1], 'colored');
      rects.textColored = rect();
      imgui.textDisabled('disabled');
      rects.textDisabled = rect();
      // About 7000 units of text in a 600-unit window: several lines.
      imgui.textWrapped(long);
      rects.textWrapped = rect();
      imgui.bulletText('bullet');
      rects.bulletText = rect();
      imgui.labelText('label', 'value');
      rects.labelText = rect();
      // Never formatted: '%d' is two characters wide, as 'xx' is.
      imgui.textDisabled('%d');
      percent = width(rect());
      imgui.textDisabled('xx');
      plain = width(rect());
    });
    const wrong = [];
    order.forEach((k, i) => {
      const r = rects[k];
      if (!(width(r) > 0 && height(r) > 0))
        wrong.push(k + ':empty');
      if (i > 0 && !(r.min[1] >= rects[order[i - 1]].max[1]))
        wrong.push(k + ':not-below');
    });
    if (!(height(rects.textWrapped) > line))
      wrong.push('textWrapped:one-line');
    if (!(width(rects.textWrapped) < 600))
      wrong.push('textWrapped:unwrapped');
    if (!(width(rects.bulletText) > bulletTextW))
      wrong.push('bulletText:no-bullet');
    if (!(width(rects.labelText) > valueW))
      wrong.push('labelText:no-label');
    if (percent !== plain)
      wrong.push('percent:' + percent + '/' + plain);
    console.log(
        wrong.length === 0 ?
            'TEXTS ok' :
            'TEXTS WRONG ' + wrong.join(' ') + ' ' + JSON.stringify(rects));
  }

  // --- GREEN -----------------------------------------------------------
  {
    let green, white;
    const draw = () => {
      imgui.setNextWindowPos([20, 20]);
      imgui.setNextWindowSize([600, 440]);
      imgui.begin('green');
      imgui.textColored([0, 1, 0, 1], 'MMMMMMMM');
      green = rect();
      imgui.text('MMMMMMMM');
      white = rect();
      imgui.end();
    };
    await h.run(draw);
    const image = await harness.capture(h, draw, capturePath);
    const count = (r) => {
      let n = 0;
      for (let y = Math.ceil(r.min[1]); y < r.max[1]; ++y) {
        for (let x = Math.ceil(r.min[0]); x < r.max[0]; ++x) {
          if (same(harness.pixelAt(image, x, y), [0, 255, 0]))
            ++n;
        }
      }
      return n;
    };
    const g = count(green);
    const w = count(white);
    console.log(g > 0 && w === 0 ? 'GREEN ok' : 'GREEN WRONG ' + g + ' ' + w);
  }

  // --- IDS -------------------------------------------------------------
  {
    const ids = {};
    await inWindow(() => {
      ids.outer = imgui.getID('x');
      ids.outerAgain = imgui.getID('x');
      imgui.pushID('a');
      ids.a = imgui.getID('x');
      imgui.popID();
      ids.popped = imgui.getID('x');
      imgui.pushID('b');
      ids.b = imgui.getID('x');
      imgui.popID();
      imgui.pushID(7);
      ids.int7 = imgui.getID('x');
      imgui.popID();
      imgui.pushID('7');
      ids.str7 = imgui.getID('x');
      imgui.popID();
      ids.getInt = imgui.getID(7);
      ids.getStr = imgui.getID('7');
    });
    const values = Object.values(ids);
    const ok = values.every((v) => Number.isInteger(v) && v >= 0 &&
                                   v <= 0xFFFFFFFF) &&
        ids.outer === ids.outerAgain && ids.popped === ids.outer &&
        ids.a !== ids.b && ids.a !== ids.outer && ids.int7 !== ids.str7 &&
        ids.getInt !== ids.getStr;
    console.log(ok ? 'IDS ok' : 'IDS WRONG ' + JSON.stringify(ids));
  }

  // --- Queries: one window, one button, every query recorded ------------
  const q = {
    hovered: null,
    active: false,
    clicked: false,
    mouseClicked: false,
    mouseDown: null,
    mouseDownSeen: false,
    double: false,
    pos: null,
    posOut: [],
    posRet: null,
    delta: null,
    deltaOut: [],
    deltaRet: null,
    keyDown: null,
    keyDownSeen: false,
    pressed: 0,
    pressedRepeat: 0,
    button: null,
  };
  const draw = () => {
    imgui.setNextWindowPos([40, 50]);
    imgui.setNextWindowSize([300, 200]);
    imgui.begin('queries');
    imgui.button('qb', [100, 30]);
    q.button = rect();
    q.hovered = imgui.isItemHovered();
    q.active = imgui.isItemActive() || q.active;
    q.clicked = imgui.isItemClicked() || q.clicked;
    imgui.isItemDeactivatedAfterEdit(); // see ui-imgui-bind-input-text.js
    q.mouseDown = imgui.isMouseDown(LEFT);
    q.mouseDownSeen = q.mouseDown || q.mouseDownSeen;
    q.mouseClicked = imgui.isMouseClicked(LEFT) || q.mouseClicked;
    q.double = imgui.isMouseDoubleClicked(LEFT) || q.double;
    q.pos = imgui.getMousePos();
    q.posRet = imgui.getMousePos(q.posOut);
    if (q.mouseDown) {
      q.delta = imgui.getMouseDragDelta();
      q.deltaRet = imgui.getMouseDragDelta(LEFT, -1, q.deltaOut);
    }
    q.keyDown = imgui.isKeyDown(imgui.Key_A);
    q.keyDownSeen = q.keyDown || q.keyDownSeen;
    if (imgui.isKeyPressed(imgui.Key_A, false))
      ++q.pressed;
    if (imgui.isKeyPressed(imgui.Key_A))
      ++q.pressedRepeat;
    imgui.end();
  };
  await h.run(draw);
  await h.run(draw);
  const buttonAt = harness.center(q.button);

  // --- HOVERED ---------------------------------------------------------
  {
    await harness.moveTo(h, draw, buttonAt);
    await h.run(draw);
    const on = q.hovered;
    await harness.moveTo(h, draw, OUTSIDE);
    await h.run(draw);
    const off = q.hovered;
    console.log(
        on === true && off === false ? 'HOVERED ok' :
                                       'HOVERED WRONG ' + on + ' ' + off);
  }

  // --- CLICK -----------------------------------------------------------
  {
    q.active = q.clicked = q.mouseClicked = q.mouseDownSeen = false;
    await harness.clickAt(h, draw, buttonAt);
    const ok = q.active && q.clicked && q.mouseClicked && q.mouseDownSeen &&
        q.mouseDown === false;
    console.log(
        ok ? 'CLICK ok' :
             'CLICK WRONG ' + JSON.stringify([q.active, q.clicked,
                                              q.mouseClicked, q.mouseDownSeen,
                                              q.mouseDown]));
  }

  // --- DOUBLE ----------------------------------------------------------
  {
    // ImGui counts two clicks as a double click only within
    // io.MouseDoubleClickTime of its own time, which advances with the real
    // clock; the hook makes that window effectively unbounded, so the case
    // does not depend on how long the frames take.
    const previous = imgui.__test.setDoubleClickTime(1000);
    q.double = false;
    let doubles = 0;
    const counting = () => {
      draw();
      if (q.double)
        ++doubles;
      q.double = false;
    };
    // A click far from the button first: ImGui counts a click as a
    // repeat only within MouseDoubleClickMaxDist of the last one
    // (imgui.cpp:11154-11163), so CLICK's earlier click on the button,
    // well inside the widened time, does not make the next one a double.
    await harness.clickAt(h, counting, OUTSIDE);
    await harness.clickAt(h, counting, buttonAt);
    const afterFirst = doubles;
    await harness.clickAt(h, counting, buttonAt);
    imgui.__test.setDoubleClickTime(previous);
    console.log(
        afterFirst === 0 && doubles === 1 ?
            'DOUBLE ok' :
            'DOUBLE WRONG afterFirst=' + afterFirst + ' doubles=' + doubles);
  }

  // --- MOUSEPOS --------------------------------------------------------
  {
    await harness.moveTo(h, draw, [123, 145]);
    await h.run(draw);
    const ok = same(q.pos, [123, 145]) && q.posRet === q.posOut &&
        same(q.posOut, [123, 145]);
    console.log(ok ? 'MOUSEPOS ok' : 'MOUSEPOS WRONG ' + q.pos + ' ' + q.posOut);
  }

  // --- DRAG ------------------------------------------------------------
  {
    q.delta = null;
    const to = [buttonAt[0] + 40, buttonAt[1] + 30];
    await harness.dragFromTo(h, draw, buttonAt, to);
    const ok = q.delta !== null && same(q.delta, [40, 30]) &&
        q.deltaRet === q.deltaOut && same(q.deltaOut, [40, 30]);
    console.log(ok ? 'DRAG ok' : 'DRAG WRONG ' + q.delta + ' ' + q.deltaOut);
  }

  // --- KEY -------------------------------------------------------------
  {
    q.pressed = q.pressedRepeat = 0;
    q.keyDownSeen = false;
    harness.inject(h, {type: 'KEY_DOWN', keyCode: SAPP_KEYCODE_A});
    await harness.until(h, draw, () => q.keyDownSeen, 'KEY down');
    harness.inject(h, {type: 'KEY_UP', keyCode: SAPP_KEYCODE_A});
    await harness.until(h, draw, () => q.keyDown === false, 'KEY up');
    const ok = q.pressed === 1 && q.pressedRepeat >= 1;
    console.log(
        ok ? 'KEY ok' : 'KEY WRONG pressed=' + q.pressed + '/' +
                         q.pressedRepeat);
  }

  // --- FOCUSHERE -------------------------------------------------------
  {
    const focused = {};
    let focusHere = false;
    const drawFocus = () => {
      imgui.setNextWindowPos([40, 50]);
      imgui.setNextWindowSize([200, 150]);
      imgui.begin('focus here');
      focused.here = imgui.isWindowFocused();
      if (focusHere)
        imgui.setKeyboardFocusHere();
      imgui.button('target');
      imgui.end();
      imgui.setNextWindowPos([340, 50]);
      imgui.setNextWindowSize([200, 150]);
      imgui.begin('focus other');
      focused.other = imgui.isWindowFocused();
      imgui.end();
    };
    await harness.clickAt(h, drawFocus, [440, 150]);
    await h.run(drawFocus);
    const before = Object.assign({}, focused);
    focusHere = true;
    await h.run(drawFocus);
    focusHere = false;
    await h.run(drawFocus);
    const after = Object.assign({}, focused);
    const ok = before.here === false && before.other === true &&
        after.here === true && after.other === false;
    console.log(
        ok ? 'FOCUSHERE ok' :
             'FOCUSHERE WRONG before=' + JSON.stringify(before) + ' after=' +
                 JSON.stringify(after));
  }

  // --- DEFAULTFOCUS ----------------------------------------------------
  {
    // A window submitted for the first time appears and takes focus, and
    // Begin() then asks navigation to initialise it (FocusWindow, then
    // NavInitWindow, imgui.cpp:8799-8802, which sets g.NavInitRequest,
    // imgui.cpp:14688). setItemDefaultFocus() on an item below the visible
    // area answers that request and scrolls the item into view
    // (imgui.cpp:9716-9732).
    let scrollY = -1;
    const drawDefault = () => {
      imgui.setNextWindowPos([40, 50]);
      imgui.setNextWindowSize([300, 200]);
      imgui.begin('default focus');
      scrollY = imgui.getScrollY();
      imgui.dummy([10, 600]);
      imgui.button('t');
      imgui.setItemDefaultFocus();
      imgui.end();
    };
    await harness.until(h, drawDefault, () => scrollY > 0, 'DEFAULTFOCUS');
    console.log('DEFAULTFOCUS ok');
  }

  // --- REJECTED --------------------------------------------------------
  {
    let usage;
    await inWindow(() => {
      imgui.button('r');
      const range = 'ERR_OUT_OF_RANGE';
      for (const b of [5, -1, imgui.MouseButton_COUNT]) {
        rejects('isItemClicked', () => imgui.isItemClicked(b), range);
        rejects('isMouseDown', () => imgui.isMouseDown(b), range);
        rejects('isMouseClicked', () => imgui.isMouseClicked(b), range);
        rejects('isMouseDoubleClicked', () => imgui.isMouseDoubleClicked(b),
                range);
        rejects('getMouseDragDelta', () => imgui.getMouseDragDelta(b), range);
      }
      for (const k of [imgui.Key_None, imgui.Mod_None, imgui.Key_NamedKey_END,
                       imgui.Key_NamedKey_COUNT,
                       imgui.Mod_Ctrl | imgui.Mod_Shift]) {
        rejects('isKeyPressed', () => imgui.isKeyPressed(k), range);
        rejects('isKeyDown', () => imgui.isKeyDown(k), range);
      }
      // -2 asserts plainly (imgui.cpp:9687); 2^31 - 1 overflows
      // `offset + 1` (imgui.cpp:9712).
      for (const offset of [-2, 2147483647])
        rejects('setKeyboardFocusHere',
                () => imgui.setKeyboardFocusHere(offset), range);
      // Accepted: the edges of each range.
      const before = calls('isMouseDown') + calls('isKeyDown') +
          calls('isKeyPressed') + calls('setKeyboardFocusHere');
      imgui.isMouseDown(4);
      imgui.isKeyDown(imgui.Mod_Ctrl);
      imgui.isKeyPressed(imgui.Key_NamedKey_BEGIN);
      imgui.setKeyboardFocusHere(2147483646);
      imgui.setKeyboardFocusHere(-1);
      const after = calls('isMouseDown') + calls('isKeyDown') +
          calls('isKeyPressed') + calls('setKeyboardFocusHere');
      if (after !== before + 5)
        fail('REJECTED edges not accepted: ' + (after - before));
      // ImGui's own check (imgui.cpp:5114): the call is made, then thrown.
      const hoveredBefore = calls('isItemHovered');
      usage = harness.expectThrow(
          () => imgui.isItemHovered(imgui.HoveredFlags_ChildWindows),
          'ERR_IMGUI_USAGE').message;
      if (calls('isItemHovered') !== hoveredBefore + 1)
        fail('REJECTED isItemHovered not called');
    });
    console.log('REJECTED ok');
    console.log('REJECTED usage ' + usage);
  }

  for (const group of ['Layout', 'IDs', 'Text', 'Queries'])
    harness.checkGroupCoverage(h, group);
  process.exit(0);
}

main().catch((e) => {
  console.log('FAILED ' + (e && e.stack));
  process.exit(1);
});

// CHECK: {{^}}SAMELINE ok{{$}}
// CHECK-NEXT: {{^}}INDENT ok{{$}}
// CHECK-NEXT: {{^}}GROUP ok{{$}}
// CHECK-NEXT: {{^}}DUMMY ok{{$}}
// CHECK-NEXT: {{^}}WIDTH ok{{$}}
// CHECK-NEXT: {{^}}ITEMWIDTH ok{{$}}
// CHECK-NEXT: {{^}}ITEMWIDTH message imgui.setNextItemWidth(): itemWidth must be less than 2147483648; got 3e+38{{$}}
// CHECK-NEXT: {{^}}CURSOR ok{{$}}
// CHECK-NEXT: {{^}}SPACING ok{{$}}
// CHECK-NEXT: {{^}}TEXTSIZE ok{{$}}
// CHECK-NEXT: {{^}}HEIGHTS ok{{$}}
// CHECK-NEXT: {{^}}TEXTS ok{{$}}
// CHECK-NEXT: {{^}}GREEN ok{{$}}
// CHECK-NEXT: {{^}}IDS ok{{$}}
// CHECK-NEXT: {{^}}HOVERED ok{{$}}
// CHECK-NEXT: {{^}}CLICK ok{{$}}
// CHECK-NEXT: {{^}}DOUBLE ok{{$}}
// CHECK-NEXT: {{^}}MOUSEPOS ok{{$}}
// CHECK-NEXT: {{^}}DRAG ok{{$}}
// CHECK-NEXT: {{^}}KEY ok{{$}}
// CHECK-NEXT: {{^}}FOCUSHERE ok{{$}}
// CHECK-NEXT: {{^}}DEFAULTFOCUS ok{{$}}
// CHECK-NEXT: {{^}}REJECTED ok{{$}}
// CHECK-NEXT: {{^}}REJECTED usage imgui.isItemHovered(): Invalid flags for IsItemHovered()!{{$}}
// CHECK-NEXT: {{^}}COVERAGE Layout ok{{$}}
// CHECK-NEXT: {{^}}COVERAGE IDs ok{{$}}
// CHECK-NEXT: {{^}}COVERAGE Text ok{{$}}
// CHECK-NEXT: {{^}}COVERAGE Queries ok{{$}}
// CODE: EXITCODE 0

// TRACE: {{^}}[ui] imgui: context created
// TRACE: {{^}}[ui] imgui: usage error: Invalid flags for IsItemHovered()!{{$}}
