// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// The Widgets group: each widget built in one window, its rectangle read
// back, an injected interaction, then its reference array and return
// value; the group's "Rejected" lines; and its coverage. The inputText
// family's behaviour is test/ui-imgui-bind-input-text.js's; here each of
// the three only draws an item, for coverage.
//
// Cases, in the order they run, one output line each (`<CASE> ok`; any
// failure prints its own line and exits 1):
//
//  BUTTONS   button, smallButton, arrowButton and invisibleButton: a click
//            on each makes it return true in exactly one frame, the frame
//            of the release (isMouseDown(0) false in it), and no other of
//            the four returns true.
//  CHECKBOX  a click flips v[0] to true and returns true once; a second
//            click flips it back.
//  RADIO     radioButton(label, active) returns true on a click;
//            radioButton(label, v, 1) writes v[0] = 1 and returns true.
//  SLIDERS   sliderFloat, sliderInt and sliderFloat2..4, dragged from their
//            last field to beyond their right end: that element holds the
//            maximum exactly, the others are unchanged, each returned true.
//  DRAGS     dragFloat and dragInt dragged 40 units right: v[0] > 0, true
//            returned.
//  INPUTNUM  inputFloat and inputInt: a click, typed digits and Enter set
//            v[0] to 2.5 and 42; each returned true.
//  COMBO     beginCombo opens on a click; selectable inside it returns true
//            on a click on its item, after which beginCombo returns false.
//  SELREF    selectable(label, p) flips p[0] on a click, and back.
//  COLOREDIT colorEdit3 and colorEdit4: dragging the R field to the right
//            raises col[0]; the other components stay exactly 0 and 1.
//  COLORBTN  colorButton returns true on a click, and its centre pixel is
//            exactly its colour, (255, 0, 0).
//  PROGRESS  progressBar(0.5, [200, 20]) is an item of exactly 200 x 20;
//            the default size, an overlay and a null overlay draw.
//  BULLET    bullet() is a non-empty item starting at the cursor.
//  LISTBOX   beginListBox('##lb', [200, 100]) returns true and is an item of
//            that size holding its selectables; a click on one returns
//            true.
//  TEXTFIELDS inputText, inputTextWithHint and inputTextMultiline each draw
//            a non-empty item.
//  FORMAT    a slider with format '%.1f units' draws; '%s', '%d', '%f %f'
//            and '%' on a float (sliders, dragFloat, inputFloat,
//            sliderFloat2..4), '%f' and '%s' on an int, and the adversarial
//            spellings ('%ld', '%hhd', '%ls', '%n', '%*d', '%5$d', '%.*f',
//            '%lf', '%Lf') on both: ERR_INVALID_ARG_VALUE, not called.
//            '%%%.1f', '%i' and '%04x' are accepted.
//  FORMATLEN a 30-byte conversion is accepted by sliderFloat and dragInt
//            and then driven through ImGui's 32-byte copy of it (a drag to
//            the maximum; a Ctrl+click, 7 and Enter); one byte longer is
//            ERR_INVALID_ARG_VALUE, not called.
//  REJECTED  arrowButton(Dir_None / Dir_COUNT); SliderFlags_WrapAround on
//            every slider (accepted by dragFloat); slider bounds beyond
//            FLT_MAX/2 and INT_MIN/2 .. INT_MAX/2 (the edges accepted);
//            beginCombo's three flag combinations; colorEdit3/4's three;
//            inputFloat / inputInt with a Callback* bit or WordWrap: each
//            ERR_OUT_OF_RANGE, not called.
//  COVERAGE  every Widgets function has reached ImGui.
//
// THE MUTATIONS THIS MUST BREAK: the full list, with what each did, is the
// Mutations table of docs/superpowers/plans/progress-imgui-binding.md
// (the rows for this file, Task 4 and its review).
//
// REQUIRES: ui, ui-gpu, ui-imgui
// REQUIRES: display
//
// RUN: %with-display 300 sh -c 'HERMES_NODE_UI_TEST_HOOKS=1 HERMES_NODE_DEBUG_NATIVE=UI %hermes-node-ui %s %t.ppm > %t.out 2> %t.err'; echo "EXITCODE $?" | %FileCheck --check-prefix=CODE %s
// RUN: %FileCheck %s < %t.out
// RUN: %FileCheck --implicit-check-not='[sapp:error' --implicit-check-not='[sokol-' --implicit-check-not='imgui: usage error' --check-prefix=TRACE %s < %t.err

'use strict';

const harness = require('./fixtures/imgui/harness.js');

const capturePath = process.argv[2];

// Sokol's key code for Enter, and ImGui's left mouse button.
const SAPP_KEYCODE_ENTER = 257;
const LEFT = 0;
// An empty spot inside the test window.
const BLANK = [560, 420];

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

function inside(inner, outer) {
  return inner.min[0] >= outer.min[0] && inner.min[1] >= outer.min[1] &&
      inner.max[0] <= outer.max[0] && inner.max[1] <= outer.max[1];
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

  /** A draw function: `body` inside a 600x440 window at (20, 20). */
  const inWindow = (body) => () => {
    imgui.setNextWindowPos([20, 20]);
    imgui.setNextWindowSize([600, 440]);
    imgui.begin('widgets');
    body();
    imgui.end();
  };

  /** Two frames, so that a new layout has settled before it is read. */
  async function settle(draw) {
    await h.run(draw);
    await h.run(draw);
  }

  // --- BUTTONS ---------------------------------------------------------
  {
    const names = ['button', 'smallButton', 'arrowButton', 'invisibleButton'];
    const rects = {};
    const seen = {};
    const reset = () => {
      for (const n of names)
        seen[n] = {count: 0, up: null};
    };
    reset();
    const note = (name, pressed, up) => {
      rects[name] = rect();
      if (pressed) {
        ++seen[name].count;
        seen[name].up = up;
      }
    };
    const draw = inWindow(() => {
      const up = !imgui.isMouseDown(LEFT);
      note('button', imgui.button('b1'), up);
      note('smallButton', imgui.smallButton('sb'), up);
      note('arrowButton', imgui.arrowButton('ab', imgui.Dir_Right), up);
      note('invisibleButton', imgui.invisibleButton('ib', [60, 30]), up);
    });
    await settle(draw);
    for (const name of names) {
      reset();
      await harness.clickAt(h, draw, harness.center(rects[name]));
      const others = names.filter((n) => n !== name && seen[n].count !== 0);
      if (seen[name].count !== 1 || seen[name].up !== true || others.length)
        fail('BUTTONS WRONG ' + name + ' ' + JSON.stringify(seen));
    }
    console.log('BUTTONS ok');
  }

  // --- CHECKBOX --------------------------------------------------------
  {
    const v = [false];
    let r, changes = 0;
    const draw = inWindow(() => {
      if (imgui.checkbox('cb', v))
        ++changes;
      r = rect();
    });
    await settle(draw);
    await harness.clickAt(h, draw, harness.center(r));
    const first = v[0], firstChanges = changes;
    await harness.clickAt(h, draw, harness.center(r));
    const ok = first === true && firstChanges === 1 && v[0] === false &&
        changes === 2;
    console.log(ok ? 'CHECKBOX ok' :
                     'CHECKBOX WRONG ' + first + ' ' + firstChanges + ' ' +
                         v[0] + ' ' + changes);
  }

  // --- RADIO -----------------------------------------------------------
  {
    let selected = 0, boolPresses = 0, intPresses = 0;
    const v = [0];
    let ra, rb;
    const draw = inWindow(() => {
      if (imgui.radioButton('ra', selected === 1)) {
        selected = 1;
        ++boolPresses;
      }
      ra = rect();
      if (imgui.radioButton('r0', v, 0))
        ++intPresses;
      if (imgui.radioButton('r1', v, 1))
        ++intPresses;
      rb = rect();
    });
    await settle(draw);
    await harness.clickAt(h, draw, harness.center(ra));
    await harness.clickAt(h, draw, harness.center(rb));
    const ok = selected === 1 && boolPresses === 1 && v[0] === 1 &&
        intPresses === 1;
    console.log(ok ? 'RADIO ok' :
                     'RADIO WRONG ' + selected + ' ' + boolPresses + ' ' +
                         v[0] + ' ' + intPresses);
  }

  // --- SLIDERS ---------------------------------------------------------
  {
    const values = {
      sliderFloat: [2],
      sliderInt: [3],
      sliderFloat2: [1, 2],
      sliderFloat3: [1, 2, 3],
      sliderFloat4: [1, 2, 3, 4],
    };
    const initial = JSON.parse(JSON.stringify(values));
    const rects = {}, changed = {};
    const draw = inWindow(() => {
      for (const name of Object.keys(values)) {
        if (imgui[name]('##' + name, values[name], 0, 10))
          changed[name] = true;
        rects[name] = rect();
      }
    });
    await settle(draw);
    const bad = [];
    for (const name of Object.keys(values)) {
      const r = rects[name];
      const y = Math.floor((r.min[1] + r.max[1]) / 2);
      await harness.dragFromTo(h, draw, [Math.floor(r.max[0]) - 4, y],
                               [Math.floor(r.max[0]) + 60, y]);
      const v = values[name];
      const last = v.length - 1;
      const rest = v.slice(0, last);
      if (v[last] !== 10 || !same(rest, initial[name].slice(0, last)) ||
          changed[name] !== true)
        bad.push(name + '=' + JSON.stringify(v) + '/' + changed[name]);
    }
    console.log(bad.length === 0 ? 'SLIDERS ok' : 'SLIDERS WRONG ' + bad);
  }

  // --- DRAGS -----------------------------------------------------------
  {
    const df = [0], di = [0];
    const changed = {};
    let rf, ri;
    const draw = inWindow(() => {
      if (imgui.dragFloat('##df', df))
        changed.f = true;
      rf = rect();
      if (imgui.dragInt('##di', di))
        changed.i = true;
      ri = rect();
    });
    await settle(draw);
    for (const r of [rf, ri]) {
      const from = harness.center(r);
      await harness.dragFromTo(h, draw, from, [from[0] + 40, from[1]]);
    }
    const ok = df[0] > 0 && di[0] > 0 && changed.f === true &&
        changed.i === true;
    console.log(ok ? 'DRAGS ok' :
                     'DRAGS WRONG ' + df[0] + ' ' + di[0] + ' ' +
                         JSON.stringify(changed));
  }

  // --- INPUTNUM --------------------------------------------------------
  {
    const fv = [0], iv = [0];
    const changed = {};
    let rf, ri;
    const draw = inWindow(() => {
      if (imgui.inputFloat('##if', fv))
        changed.f = true;
      rf = rect();
      if (imgui.inputInt('##ii', iv))
        changed.i = true;
      ri = rect();
    });
    await settle(draw);
    const enter = async (r, text, done) => {
      await harness.clickAt(h, draw, harness.center(r));
      for (const ch of text)
        harness.inject(h, {type: 'CHAR', charCode: ch.codePointAt(0)});
      await harness.pressKey(h, draw, SAPP_KEYCODE_ENTER, 0, done);
    };
    await enter(rf, '2.5', () => fv[0] === 2.5);
    await enter(ri, '42', () => iv[0] === 42);
    const ok = changed.f === true && changed.i === true;
    console.log(ok ? 'INPUTNUM ok' : 'INPUTNUM WRONG ' + JSON.stringify(changed));
  }

  // --- COMBO -----------------------------------------------------------
  {
    let open = false, opened = false, picked = 0, preview = 'none';
    let comboRect, itemRect;
    const draw = inWindow(() => {
      open = imgui.beginCombo('##combo', preview);
      if (!open)
        comboRect = rect();
      if (open) {
        opened = true;
        imgui.selectable('item a');
        if (imgui.selectable('item b')) {
          ++picked;
          preview = 'b';
        }
        itemRect = rect();
        imgui.endCombo();
      }
    });
    await settle(draw);
    await harness.clickAt(h, draw, harness.center(comboRect));
    await harness.until(h, draw, () => open, 'COMBO open');
    await h.run(draw);
    const pickedBefore = picked;
    await harness.clickAt(h, draw, harness.center(itemRect));
    await harness.until(h, draw, () => !open, 'COMBO closed');
    const ok = opened && pickedBefore === 0 && picked === 1 && preview === 'b';
    console.log(ok ? 'COMBO ok' :
                     'COMBO WRONG ' + opened + ' ' + pickedBefore + ' ' +
                         picked + ' ' + preview);
  }

  // --- SELREF ----------------------------------------------------------
  {
    const p = [false];
    let r, presses = 0;
    const draw = inWindow(() => {
      if (imgui.selectable('sel ref', p))
        ++presses;
      r = rect();
    });
    await settle(draw);
    await harness.clickAt(h, draw, harness.center(r));
    const first = p[0];
    await harness.clickAt(h, draw, harness.center(r));
    const ok = first === true && p[0] === false && presses === 2;
    console.log(ok ? 'SELREF ok' :
                     'SELREF WRONG ' + first + ' ' + p[0] + ' ' + presses);
  }

  // --- COLOREDIT -------------------------------------------------------
  {
    // Components of 0 and 1 survive ImGui's 8-bit round trip exactly.
    const c3 = [0, 0, 1], c4 = [0, 0, 1, 1];
    const changed = {};
    let r3, r4;
    const draw = inWindow(() => {
      if (imgui.colorEdit3('##ce3', c3))
        changed.c3 = true;
      r3 = rect();
      if (imgui.colorEdit4('##ce4', c4))
        changed.c4 = true;
      r4 = rect();
    });
    await settle(draw);
    for (const r of [r3, r4]) {
      const y = Math.floor((r.min[1] + r.max[1]) / 2);
      const x = Math.floor(r.min[0]) + 8;
      await harness.dragFromTo(h, draw, [x, y], [x + 30, y]);
    }
    const ok = c3[0] > 0 && c3[1] === 0 && c3[2] === 1 && c4[0] > 0 &&
        c4[1] === 0 && c4[2] === 1 && c4[3] === 1 && changed.c3 === true &&
        changed.c4 === true;
    console.log(ok ? 'COLOREDIT ok' :
                     'COLOREDIT WRONG ' + c3 + ' ' + c4 + ' ' +
                         JSON.stringify(changed));
  }

  // --- COLORBTN --------------------------------------------------------
  {
    let r, presses = 0;
    const draw = inWindow(() => {
      if (imgui.colorButton('cbtn', [1, 0, 0, 1], 0, [40, 40]))
        ++presses;
      r = rect();
    });
    await settle(draw);
    await harness.clickAt(h, draw, harness.center(r));
    // Away from the button, so that no tooltip covers it.
    await harness.moveTo(h, draw, BLANK);
    await h.run(draw);
    const image = await harness.capture(h, draw, capturePath);
    const c = harness.center(r);
    const pixel = harness.pixelAt(image, c[0], c[1]);
    const ok = presses === 1 && same(pixel, [255, 0, 0]) && width(r) === 40 &&
        height(r) === 40;
    console.log(ok ? 'COLORBTN ok' :
                     'COLORBTN WRONG ' + presses + ' ' + pixel + ' ' +
                         JSON.stringify(r));
  }

  // --- PROGRESS --------------------------------------------------------
  {
    let r, rDefault, overlayCalls;
    const before = calls('progressBar');
    await h.run(inWindow(() => {
      imgui.progressBar(0.5, [200, 20]);
      r = rect();
      imgui.progressBar(0.25);
      rDefault = rect();
      imgui.progressBar(0.75, [200, 20], 'three quarters');
      imgui.progressBar(0.75, undefined, null);
    }));
    overlayCalls = calls('progressBar') - before;
    const ok = width(r) === 200 && height(r) === 20 && width(rDefault) > 200 &&
        overlayCalls === 4;
    console.log(ok ? 'PROGRESS ok' :
                     'PROGRESS WRONG ' + JSON.stringify([r, rDefault]) + ' ' +
                         overlayCalls);
  }

  // --- BULLET ----------------------------------------------------------
  {
    let at, r;
    await h.run(inWindow(() => {
      imgui.text('above');
      at = imgui.getCursorScreenPos();
      imgui.bullet();
      r = rect();
    }));
    const ok = same(r.min, at) && width(r) > 0 && height(r) > 0;
    console.log(ok ? 'BULLET ok' : 'BULLET WRONG ' + at + ' ' + JSON.stringify(r));
  }

  // --- LISTBOX ---------------------------------------------------------
  {
    let shown = false, box, ra, rb, presses = 0;
    const draw = inWindow(() => {
      shown = imgui.beginListBox('##lb', [200, 100]);
      if (shown) {
        imgui.selectable('la');
        ra = rect();
        if (imgui.selectable('lb'))
          ++presses;
        rb = rect();
        imgui.endListBox();
      }
      box = rect();
    });
    await settle(draw);
    await harness.clickAt(h, draw, harness.center(rb));
    const ok = shown && width(box) === 200 && height(box) === 100 &&
        inside(ra, box) && inside(rb, box) && rb.min[1] >= ra.max[1] &&
        presses === 1;
    console.log(ok ? 'LISTBOX ok' :
                     'LISTBOX WRONG ' + shown + ' ' +
                         JSON.stringify([box, ra, rb]) + ' ' + presses);
  }

  // --- TEXTFIELDS ------------------------------------------------------
  {
    const rects = [];
    await h.run(inWindow(() => {
      imgui.inputText('##t1', ['one'], 16);
      rects.push(rect());
      imgui.inputTextWithHint('##t2', 'hint', [''], 16);
      rects.push(rect());
      imgui.inputTextMultiline('##t3', ['a\nb'], 16);
      rects.push(rect());
    }));
    const ok = rects.every((r) => width(r) > 0 && height(r) > 0) &&
        height(rects[2]) > height(rects[0]);
    console.log(ok ? 'TEXTFIELDS ok' : 'TEXTFIELDS WRONG ' + JSON.stringify(rects));
  }

  // --- FORMAT ----------------------------------------------------------
  {
    let r, drew, accepted = 0;
    await h.run(inWindow(() => {
      const before = calls('sliderFloat');
      imgui.sliderFloat('##units', [0.5], 0, 1, '%.1f units');
      r = rect();
      drew = calls('sliderFloat') === before + 1;
      const value = 'ERR_INVALID_ARG_VALUE';
      // Adversarial spellings: a length modifier, %n, '*' and '$'.
      const adversarial = ['%ld', '%hhd', '%ls', '%n', '%*d', '%5$d', '%.*f',
                           '%lf', '%Lf'];
      for (const fmt of ['%s', '%d', '%f %f', '%', 'none', ...adversarial]) {
        rejects('sliderFloat',
                () => imgui.sliderFloat('f', [0], 0, 1, fmt), value);
        rejects('sliderFloat2',
                () => imgui.sliderFloat2('f', [0, 0], 0, 1, fmt), value);
        rejects('sliderFloat3',
                () => imgui.sliderFloat3('f', [0, 0, 0], 0, 1, fmt), value);
        rejects('sliderFloat4',
                () => imgui.sliderFloat4('f', [0, 0, 0, 0], 0, 1, fmt), value);
        rejects('dragFloat',
                () => imgui.dragFloat('f', [0], 1, 0, 0, fmt), value);
        rejects('inputFloat',
                () => imgui.inputFloat('f', [0], 0, 0, fmt), value);
      }
      for (const fmt of ['%f', '%s', '%d %d', ...adversarial]) {
        rejects('sliderInt', () => imgui.sliderInt('i', [0], 0, 1, fmt), value);
        rejects('dragInt', () => imgui.dragInt('i', [0], 1, 0, 0, fmt), value);
      }
      // Accepted: a literal percent before the conversion (ImGui's
      // ImParseFormatFindStart skips '%%' the same way), and '%i'.
      const acceptedBefore = calls('sliderFloat') + calls('sliderInt');
      imgui.sliderFloat('pct', [0.5], 0, 1, '%%%.1f');
      imgui.sliderInt('i conv', [1], 0, 2, '%i');
      imgui.sliderInt('x conv', [1], 0, 2, '%04x');
      accepted = calls('sliderFloat') + calls('sliderInt') - acceptedBefore;
    }));
    const ok = drew && width(r) > 0 && accepted === 3;
    console.log(ok ? 'FORMAT ok' :
                     'FORMAT WRONG ' + drew + ' ' + accepted + ' ' +
                         JSON.stringify(r));
  }

  // --- FORMATLEN -------------------------------------------------------
  {
    // ImGui copies a conversion specification into 32-byte buffers with
    // only a plain assertion (imgui_widgets.cpp:3714, 3729), on a float
    // slider whose value changes (RoundScalarWithFormatT) and on the text
    // entry of an int drag (DataTypeApplyFromText): 30 bytes is the most it
    // takes. Each candidate is drawn; one that is accepted is then driven
    // through that copy, so a limit that is too generous aborts here.
    const float30 = '%.' + '0'.repeat(26) + '1f';
    const int30 = '%' + '0'.repeat(27) + '1d';
    const results = [];
    for (const [kind, fmt] of [['float', float30], ['float', '%.' + '0'.repeat(27) + '1f'],
                               ['int', int30], ['int', int30.slice(0, -1) + '1d']]) {
      const name = kind === 'float' ? 'sliderFloat' : 'dragInt';
      const v = [0];
      let r = null, threw = null, counted = 0;
      const draw = inWindow(() => {
        const before = calls(name);
        try {
          if (kind === 'float')
            imgui.sliderFloat('##len', v, 0, 10, fmt);
          else
            imgui.dragInt('##len', v, 1, 0, 0, fmt);
          r = rect();
        } catch (e) {
          threw = e;
        }
        counted = calls(name) - before;
      });
      await settle(draw);
      if (threw !== null) {
        results.push(fmt.length + ':' + threw.code + '/' + counted);
        continue;
      }
      if (kind === 'float') {
        const y = Math.floor((r.min[1] + r.max[1]) / 2);
        const from = harness.center(r);
        await harness.dragFromTo(h, draw, from, [Math.floor(r.max[0]) + 60, y]);
      } else {
        // Ctrl+click turns the drag into a text field (DragScalar,
        // imgui_widgets.cpp:2792), whose Enter applies the text through
        // DataTypeApplyFromText.
        const at = harness.center(r);
        await harness.moveTo(h, draw, at);
        harness.inject(h, {type: 'MOUSE_DOWN', button: LEFT, modifiers: 2,
                           mouseX: at[0], mouseY: at[1]});
        await harness.until(h, draw, (s) => s.mouseDown[LEFT], 'FORMATLEN down');
        harness.inject(h, {type: 'MOUSE_UP', button: LEFT, modifiers: 0,
                           mouseX: at[0], mouseY: at[1]});
        await harness.until(h, draw, (s) => s.wantTextInput, 'FORMATLEN input');
        harness.inject(h, {type: 'CHAR', charCode: 0x37}); // 7
        await harness.pressKey(h, draw, SAPP_KEYCODE_ENTER, 0, () => v[0] === 7);
      }
      results.push(fmt.length + ':' + v[0]);
    }
    const got = results.join(' ');
    const want = '30:10 31:ERR_INVALID_ARG_VALUE/0 30:7 31:ERR_INVALID_ARG_VALUE/0';
    console.log(got === want ? 'FORMATLEN ok' : 'FORMATLEN WRONG ' + got);
  }

  // --- REJECTED --------------------------------------------------------
  {
    let accepted = 0;
    await h.run(inWindow(() => {
      const range = 'ERR_OUT_OF_RANGE';
      for (const dir of [imgui.Dir_None, imgui.Dir_COUNT])
        rejects('arrowButton', () => imgui.arrowButton('a', dir), range);
      const wrap = imgui.SliderFlags_WrapAround;
      rejects('sliderFloat',
              () => imgui.sliderFloat('s', [0], 0, 1, '%.3f', wrap), range);
      rejects('sliderInt', () => imgui.sliderInt('s', [0], 0, 1, '%d', wrap),
              range);
      rejects('sliderFloat2',
              () => imgui.sliderFloat2('s', [0, 0], 0, 1, '%.3f', wrap), range);
      rejects('sliderFloat3',
              () => imgui.sliderFloat3('s', [0, 0, 0], 0, 1, '%.3f', wrap),
              range);
      rejects('sliderFloat4',
              () => imgui.sliderFloat4('s', [0, 0, 0, 0], 0, 1, '%.3f', wrap),
              range);
      // SliderBehavior's bounds (widgets:3346, 3358).
      const half = 3.4028234663852886e38 / 2;
      for (const [lo, hi] of [[-half * 1.01, 1], [0, half * 1.01]]) {
        rejects('sliderFloat', () => imgui.sliderFloat('s', [0], lo, hi),
                range);
        rejects('sliderFloat2', () => imgui.sliderFloat2('s', [0, 0], lo, hi),
                range);
        rejects('sliderFloat4',
                () => imgui.sliderFloat4('s', [0, 0, 0, 0], lo, hi), range);
      }
      rejects('sliderInt', () => imgui.sliderInt('s', [0], -1073741825, 0),
              range);
      rejects('sliderInt', () => imgui.sliderInt('s', [0], 0, 1073741824),
              range);
      const combo = [
        imgui.ComboFlags_NoArrowButton | imgui.ComboFlags_NoPreview,
        imgui.ComboFlags_WidthFitPreview | imgui.ComboFlags_NoPreview,
        imgui.ComboFlags_HeightSmall | imgui.ComboFlags_HeightLarge,
      ];
      for (const flags of combo)
        rejects('beginCombo', () => imgui.beginCombo('c', 'p', flags), range);
      const color = [
        imgui.ColorEditFlags_DisplayRGB | imgui.ColorEditFlags_DisplayHSV,
        imgui.ColorEditFlags_DisplayHSV | imgui.ColorEditFlags_DisplayHex,
        imgui.ColorEditFlags_InputRGB | imgui.ColorEditFlags_InputHSV,
        imgui.ColorEditFlags_PickerHueBar | imgui.ColorEditFlags_PickerHueWheel,
      ];
      for (const flags of color) {
        rejects('colorEdit3', () => imgui.colorEdit3('c', [0, 0, 0], flags),
                range);
        rejects('colorEdit4', () => imgui.colorEdit4('c', [0, 0, 0, 0], flags),
                range);
      }
      const textFlags = [
        imgui.InputTextFlags_CallbackCompletion,
        imgui.InputTextFlags_CallbackHistory,
        imgui.InputTextFlags_CallbackAlways,
        imgui.InputTextFlags_CallbackCharFilter,
        imgui.InputTextFlags_CallbackResize,
        imgui.InputTextFlags_CallbackEdit,
        imgui.InputTextFlags_WordWrap,
      ];
      for (const flags of textFlags) {
        rejects('inputFloat',
                () => imgui.inputFloat('f', [0], 0, 0, '%.3f', flags), range);
        rejects('inputInt', () => imgui.inputInt('i', [0], 1, 100, flags),
                range);
      }
      // Accepted: the edges, and what only sliders refuse.
      const before = calls('sliderInt') + calls('dragFloat') +
          calls('arrowButton') + calls('beginCombo') + calls('colorEdit3');
      imgui.sliderInt('edge', [0], -1073741824, 1073741823);
      imgui.dragFloat('wrap', [0], 1, 0, 1, '%.3f', wrap);
      imgui.arrowButton('left', imgui.Dir_Left);
      if (imgui.beginCombo('one height', null, imgui.ComboFlags_HeightSmall))
        imgui.endCombo();
      imgui.colorEdit3('one each', [0, 0, 0],
                       imgui.ColorEditFlags_DisplayHex |
                           imgui.ColorEditFlags_InputRGB |
                           imgui.ColorEditFlags_PickerHueWheel);
      accepted = calls('sliderInt') + calls('dragFloat') +
          calls('arrowButton') + calls('beginCombo') + calls('colorEdit3') -
          before;
    }));
    console.log(accepted === 5 ? 'REJECTED ok' : 'REJECTED accepted ' + accepted);
  }

  harness.checkGroupCoverage(h, 'Widgets');
  process.exit(0);
}

main().catch((e) => {
  console.log('FAILED ' + (e && e.stack));
  process.exit(1);
});

// CHECK: {{^}}BUTTONS ok{{$}}
// CHECK-NEXT: {{^}}CHECKBOX ok{{$}}
// CHECK-NEXT: {{^}}RADIO ok{{$}}
// CHECK-NEXT: {{^}}SLIDERS ok{{$}}
// CHECK-NEXT: {{^}}DRAGS ok{{$}}
// CHECK-NEXT: {{^}}INPUTNUM ok{{$}}
// CHECK-NEXT: {{^}}COMBO ok{{$}}
// CHECK-NEXT: {{^}}SELREF ok{{$}}
// CHECK-NEXT: {{^}}COLOREDIT ok{{$}}
// CHECK-NEXT: {{^}}COLORBTN ok{{$}}
// CHECK-NEXT: {{^}}PROGRESS ok{{$}}
// CHECK-NEXT: {{^}}BULLET ok{{$}}
// CHECK-NEXT: {{^}}LISTBOX ok{{$}}
// CHECK-NEXT: {{^}}TEXTFIELDS ok{{$}}
// CHECK-NEXT: {{^}}FORMAT ok{{$}}
// CHECK-NEXT: {{^}}FORMATLEN ok{{$}}
// CHECK-NEXT: {{^}}REJECTED ok{{$}}
// CHECK-NEXT: {{^}}COVERAGE Widgets ok{{$}}
// CODE: EXITCODE 0

// No case here is a usage error, so none is traced (the
// --implicit-check-not on the TRACE run).
// TRACE: {{^}}[ui] imgui: context created
