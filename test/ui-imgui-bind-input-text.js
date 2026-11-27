// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// The inputText family and its buffer (the design's "Buffers"): typed text
// written back, the capacity and its truncation, strings at the edges
// (Review Focus 5) and the label-id check, setKeyboardFocusHere() on a
// text field and isItemDeactivatedAfterEdit() (the halves Task 3 left).
//
// Cases, in the order they run, one output line each (`<CASE> ok`; any
// failure prints its own line and exits 1):
//
//  TYPE       inputText('n', name, 64): a click, then a, b, c typed one at
//             a time: name[0] is 'a', 'ab', 'abc' after each keystroke's
//             frame, and the call returned true in exactly those frames.
//  DEACTIVATE isItemDeactivatedAfterEdit() after that field: false while it
//             is edited, true in exactly one frame once a click elsewhere
//             takes focus away.
//  ENTER      with InputTextFlags_EnterReturnsTrue, x and y typed: the
//             string follows each keystroke while the call returns false;
//             Enter makes it return true, in exactly one frame, with the
//             string unchanged.
//  TRUNCATE   capacity 4 and 'abcdef': 'abc' written back with no
//             interaction; 'aé' at capacity 3 gives 'a' (the cut is at
//             a character boundary); capacity 1 gives ''. A string that
//             fits is not written back at all (a setter counts writes).
//  EMOJI      a field holding U+1F600: End, then x typed: the string is
//             U+1F600 followed by x.
//  SURROGATE  'a\ud800bc' at capacity 5: written back as 'a�', the
//             unpaired surrogate encoded as U+FFFD.
//  CAPACITY   capacity 0, 2^24 + 1 and 1.5: ERR_OUT_OF_RANGE, not called;
//             1 and 2^24 accepted.
//  FLAGS      each InputTextFlags_Callback* bit on each of the three:
//             ERR_OUT_OF_RANGE, not called; WordWrap on the single-line two,
//             ElideLeft and WordWrap | Password on the multi-line one: the
//             same; WordWrap alone accepted on the multi-line one.
//  HINT       inputTextWithHint draws an item and leaves its string as it
//             was (nothing written).
//  MULTILINE  inputTextMultiline: a click, End and Enter make 'l1' into
//             'l1\n'.
//  FOCUS      setKeyboardFocusHere() before a field, with no click:
//             state().wantTextInput becomes true.
//  STRINGS    button('') at a window's root: ERR_IMGUI_USAGE, not called,
//             and so for '' given to every function with a label-id check
//             and for 'x###' to button; button('##x') accepted; inside
//             pushID('p'), button('') accepted; text() of a 1 MiB string
//             draws.
//
// THE MUTATIONS THIS MUST BREAK (recorded in progress-imgui-binding.md):
//  inputText writing back only when the widget returned true: ENTER fails;
//  the buffer compared with the truncated string instead of the original:
//  TRUNCATE fails;
//  labelIdIsWindowId returning false always: STRINGS aborts (Debug build);
//  ImGui::SetKeyboardFocusHere() removed, enterImGui() kept: FOCUS fails;
//  ImGui::IsItemDeactivatedAfterEdit() replaced by false: DEACTIVATE fails.
//
// REQUIRES: ui, ui-gpu, ui-imgui
// REQUIRES: display
//
// RUN: %with-display 300 sh -c 'HERMES_NODE_UI_TEST_HOOKS=1 HERMES_NODE_DEBUG_NATIVE=UI %hermes-node-ui %s > %t.out 2> %t.err'; echo "EXITCODE $?" | %FileCheck --check-prefix=CODE %s
// RUN: %FileCheck %s < %t.out
// RUN: %FileCheck --implicit-check-not='[sapp:error' --implicit-check-not='[sokol-' --check-prefix=TRACE %s < %t.err

'use strict';

const harness = require('./fixtures/imgui/harness.js');

// Sokol's key codes (test/ui-imgui-input.js).
const SAPP_KEYCODE_ENTER = 257;
const SAPP_KEYCODE_END = 269;
// An empty spot inside the test window.
const BLANK = [560, 420];

function fail(line) {
  console.log(line);
  process.exit(1);
}

function width(r) {
  return r.max[0] - r.min[0];
}

/**
 * A one-element reference array whose element is an accessor, counting
 * the writes made to it.
 */
function recording(value) {
  const a = [];
  a.writes = 0;
  let v = value;
  Object.defineProperty(a, 0, {
    get() {
      return v;
    },
    set(x) {
      ++a.writes;
      v = x;
    },
    configurable: true,
    enumerable: true,
  });
  return a;
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
    imgui.begin('input text');
    body();
    imgui.end();
  };

  async function settle(draw) {
    await h.run(draw);
    await h.run(draw);
  }

  /** Inject one character; frames run until `done()`. */
  const type = (draw, ch, done) =>
      harness.typeText(h, draw, ch, () => done());

  // --- TYPE and DEACTIVATE ---------------------------------------------
  {
    const name = [''];
    let r;
    const changes = [];
    let trueFrames = 0, deactivated = 0;
    const draw = inWindow(() => {
      const before = name[0];
      const ret = imgui.inputText('n', name, 64);
      r = rect();
      if (imgui.isItemDeactivatedAfterEdit())
        ++deactivated;
      if (ret)
        ++trueFrames;
      if (name[0] !== before)
        changes.push([name[0], ret]);
    });
    await settle(draw);
    await harness.clickAt(h, draw, harness.center(r));
    for (const [ch, want] of [['a', 'a'], ['b', 'ab'], ['c', 'abc']])
      await type(draw, ch, () => name[0] === want);
    const expected = '[["a",true],["ab",true],["abc",true]]';
    const typed = JSON.stringify(changes);
    console.log(typed === expected && trueFrames === 3 ?
                    'TYPE ok' :
                    'TYPE WRONG ' + typed + ' true=' + trueFrames);
    const whileEditing = deactivated;
    await harness.clickAt(h, draw, BLANK);
    await harness.until(h, draw, (s) => !s.wantTextInput, 'DEACTIVATE');
    await h.run(draw);
    console.log(whileEditing === 0 && deactivated === 1 ?
                    'DEACTIVATE ok' :
                    'DEACTIVATE WRONG ' + whileEditing + ' ' + deactivated);
  }

  // --- ENTER -----------------------------------------------------------
  {
    const s = [''];
    let r;
    const changes = [];
    let trueFrames = 0, trueValue = null;
    const draw = inWindow(() => {
      const before = s[0];
      const ret = imgui.inputText('e', s, 64,
                                  imgui.InputTextFlags_EnterReturnsTrue);
      r = rect();
      if (ret) {
        ++trueFrames;
        trueValue = s[0];
      }
      if (s[0] !== before)
        changes.push([s[0], ret]);
    });
    await settle(draw);
    await harness.clickAt(h, draw, harness.center(r));
    await type(draw, 'x', () => s[0] === 'x');
    await type(draw, 'y', () => s[0] === 'xy');
    const beforeEnter = trueFrames;
    await harness.pressKey(h, draw, SAPP_KEYCODE_ENTER, 0,
                           () => trueFrames > 0);
    await harness.until(h, draw, (st) => !st.wantTextInput, 'ENTER done');
    const typed = JSON.stringify(changes);
    const ok = typed === '[["x",false],["xy",false]]' && beforeEnter === 0 &&
        trueFrames === 1 && trueValue === 'xy' && s[0] === 'xy';
    console.log(ok ? 'ENTER ok' :
                     'ENTER WRONG ' + typed + ' ' + beforeEnter + ' ' +
                         trueFrames + ' ' + trueValue);
  }

  // --- TRUNCATE --------------------------------------------------------
  {
    const four = ['abcdef'], three = ['aé'], one = ['x'];
    const fits = recording('fits');
    await h.run(inWindow(() => {
      imgui.inputText('t4', four, 4);
      imgui.inputText('t3', three, 3);
      imgui.inputText('t1', one, 1);
      imgui.inputText('tf', fits, 64);
    }));
    const ok = four[0] === 'abc' && three[0] === 'a' && one[0] === '' &&
        fits[0] === 'fits' && fits.writes === 0;
    console.log(ok ? 'TRUNCATE ok' :
                     'TRUNCATE WRONG ' +
                         JSON.stringify([four[0], three[0], one[0], fits[0],
                                         fits.writes]));
  }

  // --- EMOJI -----------------------------------------------------------
  {
    const s = ['\u{1F600}'];
    let r;
    const draw = inWindow(() => {
      imgui.inputText('emoji', s, 64);
      r = rect();
    });
    await settle(draw);
    await harness.clickAt(h, draw, harness.center(r));
    harness.inject(h, {type: 'KEY_DOWN', keyCode: SAPP_KEYCODE_END});
    harness.inject(h, {type: 'KEY_UP', keyCode: SAPP_KEYCODE_END});
    await type(draw, 'x', () => s[0] !== '\u{1F600}');
    console.log(s[0] === '\u{1F600}x' ? 'EMOJI ok' :
                                        'EMOJI WRONG ' + JSON.stringify(s[0]));
    await harness.clickAt(h, draw, BLANK);
  }

  // --- SURROGATE -------------------------------------------------------
  {
    const s = ['a\ud800bc'];
    await h.run(inWindow(() => imgui.inputText('sur', s, 5)));
    console.log(s[0] === 'a�' ? 'SURROGATE ok' :
                                     'SURROGATE WRONG ' + JSON.stringify(s[0]));
  }

  // --- CAPACITY --------------------------------------------------------
  {
    let accepted = 0;
    await h.run(inWindow(() => {
      for (const cap of [0, 16777217, 1.5, -1]) {
        rejects('inputText', () => imgui.inputText('c', ['x'], cap),
                'ERR_OUT_OF_RANGE');
        rejects('inputTextMultiline',
                () => imgui.inputTextMultiline('c', ['x'], cap),
                'ERR_OUT_OF_RANGE');
        rejects('inputTextWithHint',
                () => imgui.inputTextWithHint('c', 'h', ['x'], cap),
                'ERR_OUT_OF_RANGE');
      }
      const before = calls('inputText');
      imgui.inputText('c1', ['x'], 1);
      imgui.inputText('cmax', ['x'], 16777216);
      accepted = calls('inputText') - before;
    }));
    console.log(accepted === 2 ? 'CAPACITY ok' : 'CAPACITY accepted ' + accepted);
  }

  // --- FLAGS -----------------------------------------------------------
  {
    let accepted = 0;
    await h.run(inWindow(() => {
      const range = 'ERR_OUT_OF_RANGE';
      // The six bits of imgui.h:1366-1371, each needing a callback.
      const callbacks = [
        imgui.InputTextFlags_CallbackCompletion, // 1 << 18
        imgui.InputTextFlags_CallbackHistory, // 1 << 19
        imgui.InputTextFlags_CallbackAlways, // 1 << 20
        imgui.InputTextFlags_CallbackCharFilter, // 1 << 21
        imgui.InputTextFlags_CallbackResize, // 1 << 22
        imgui.InputTextFlags_CallbackEdit, // 1 << 23
      ];
      if (callbacks.join() !== [18, 19, 20, 21, 22, 23].map((b) => 1 << b).join())
        fail('FLAGS callback values ' + callbacks);
      const single = [...callbacks, imgui.InputTextFlags_WordWrap];
      for (const flags of single) {
        rejects('inputText', () => imgui.inputText('f', ['x'], 8, flags),
                range);
        rejects('inputTextWithHint',
                () => imgui.inputTextWithHint('f', 'h', ['x'], 8, flags),
                range);
      }
      const multi = [
        ...callbacks,
        imgui.InputTextFlags_ElideLeft,
        imgui.InputTextFlags_WordWrap | imgui.InputTextFlags_Password,
      ];
      for (const flags of multi)
        rejects('inputTextMultiline',
                () => imgui.inputTextMultiline('f', ['x'], 8, [0, 0], flags),
                range);
      const before = calls('inputTextMultiline') + calls('inputText');
      imgui.inputTextMultiline('wrap', ['x'], 8, [0, 0],
                               imgui.InputTextFlags_WordWrap);
      imgui.inputText('elide', ['x'], 8, imgui.InputTextFlags_ElideLeft);
      accepted = calls('inputTextMultiline') + calls('inputText') - before;
    }));
    console.log(accepted === 2 ? 'FLAGS ok' : 'FLAGS accepted ' + accepted);
  }

  // --- HINT ------------------------------------------------------------
  {
    const s = recording('');
    let r;
    await h.run(inWindow(() => {
      imgui.inputTextWithHint('##hint', 'type here', s, 64);
      r = rect();
    }));
    const ok = width(r) > 0 && s[0] === '' && s.writes === 0;
    console.log(ok ? 'HINT ok' : 'HINT WRONG ' + JSON.stringify(r) + ' ' +
                         s.writes);
  }

  // --- MULTILINE -------------------------------------------------------
  {
    const m = ['l1'];
    let r;
    const draw = inWindow(() => {
      imgui.inputTextMultiline('m', m, 64);
      r = rect();
    });
    await settle(draw);
    await harness.clickAt(h, draw, harness.center(r));
    harness.inject(h, {type: 'KEY_DOWN', keyCode: SAPP_KEYCODE_END});
    harness.inject(h, {type: 'KEY_UP', keyCode: SAPP_KEYCODE_END});
    await harness.pressKey(h, draw, SAPP_KEYCODE_ENTER, 0,
                           () => m[0] !== 'l1');
    console.log(m[0] === 'l1\n' ? 'MULTILINE ok' :
                                  'MULTILINE WRONG ' + JSON.stringify(m[0]));
    await harness.clickAt(h, draw, BLANK);
  }

  // --- FOCUS -----------------------------------------------------------
  {
    let focusHere = false;
    const draw = inWindow(() => {
      imgui.text('before the field');
      if (focusHere)
        imgui.setKeyboardFocusHere();
      imgui.inputText('focus', ['f'], 16);
    });
    await harness.until(h, draw, (s) => !s.wantTextInput, 'FOCUS idle');
    await h.run(draw);
    const before = imgui.__test.state().wantTextInput;
    focusHere = true;
    await h.run(draw);
    focusHere = false;
    await harness.until(h, draw, (s) => s.wantTextInput, 'FOCUS');
    console.log(before === false ? 'FOCUS ok' : 'FOCUS WRONG before=' + before);
    await harness.clickAt(h, draw, BLANK);
  }

  // --- STRINGS ---------------------------------------------------------
  {
    let messages = [], accepted = 0, bigWidth = 0, bigDrawn = false;
    const big = 'x'.repeat(1 << 20);
    await h.run(inWindow(() => {
      const usage = 'ERR_IMGUI_USAGE';
      messages.push(rejects('button', () => imgui.button(''), usage).message);
      rejects('button', () => imgui.button('x###'), usage);
      // Every other function with a label-id check, given ''.
      const cases = {
        smallButton: () => imgui.smallButton(''),
        invisibleButton: () => imgui.invisibleButton('', [10, 10]),
        arrowButton: () => imgui.arrowButton('', imgui.Dir_Left),
        checkbox: () => imgui.checkbox('', [false]),
        radioButton: () => imgui.radioButton('', true),
        sliderFloat: () => imgui.sliderFloat('', [0], 0, 1),
        sliderInt: () => imgui.sliderInt('', [0], 0, 1),
        dragFloat: () => imgui.dragFloat('', [0]),
        dragInt: () => imgui.dragInt('', [0]),
        inputText: () => imgui.inputText('', ['x'], 8),
        inputTextMultiline: () => imgui.inputTextMultiline('', ['x'], 8),
        inputTextWithHint: () => imgui.inputTextWithHint('', 'h', ['x'], 8),
        inputFloat: () => imgui.inputFloat('', [0]),
        inputInt: () => imgui.inputInt('', [0]),
        beginCombo: () => imgui.beginCombo('', 'p'),
        selectable: () => imgui.selectable(''),
        colorButton: () => imgui.colorButton('', [1, 0, 0, 1]),
        beginListBox: () => imgui.beginListBox(''),
        beginChild: () => imgui.beginChild(''),
      };
      for (const name of Object.keys(cases))
        messages.push(rejects(name, cases[name], usage).message);
      // radioButton's other form.
      rejects('radioButton', () => imgui.radioButton('', [0], 1), usage);
      const before = calls('button');
      imgui.button('##x');
      imgui.pushID('p');
      imgui.button('');
      imgui.popID();
      accepted = calls('button') - before;
      const textBefore = calls('text');
      imgui.text(big);
      bigWidth = width(rect());
      bigDrawn = calls('text') === textBefore + 1;
    }));
    const ok = accepted === 2 && bigDrawn && bigWidth > 600;
    console.log(ok ? 'STRINGS ok' :
                     'STRINGS WRONG accepted=' + accepted + ' big=' + bigDrawn +
                         '/' + bigWidth);
    console.log('STRINGS message ' + messages[0]);
    const named = messages.slice(1).map((m) => m.replace(/^imgui\.\w+\(\): /, ''));
    console.log('STRINGS distinct ' + [...new Set(named)].join(' | '));
  }

  process.exit(0);
}

main().catch((e) => {
  console.log('FAILED ' + (e && e.stack));
  process.exit(1);
});

// CHECK: {{^}}TYPE ok{{$}}
// CHECK-NEXT: {{^}}DEACTIVATE ok{{$}}
// CHECK-NEXT: {{^}}ENTER ok{{$}}
// CHECK-NEXT: {{^}}TRUNCATE ok{{$}}
// CHECK-NEXT: {{^}}EMOJI ok{{$}}
// CHECK-NEXT: {{^}}SURROGATE ok{{$}}
// CHECK-NEXT: {{^}}CAPACITY ok{{$}}
// CHECK-NEXT: {{^}}FLAGS ok{{$}}
// CHECK-NEXT: {{^}}HINT ok{{$}}
// CHECK-NEXT: {{^}}MULTILINE ok{{$}}
// CHECK-NEXT: {{^}}FOCUS ok{{$}}
// CHECK-NEXT: {{^}}STRINGS ok{{$}}
// CHECK-NEXT: {{^}}STRINGS message imgui.button(): label gives the item the window's own ID (an empty label, or one ending in '###', at the root of a window); use '##name' or pushID(){{$}}
// CHECK-NEXT: {{^}}STRINGS distinct label gives the item the window's own ID (an empty label, or one ending in '###', at the root of a window); use '##name' or pushID() | strId gives {{.*}} | descId gives {{.*}}{{$}}
// CODE: EXITCODE 0

// The guard's refusals are traced as usage errors, once each.
// TRACE: {{^}}[ui] imgui: context created
// TRACE: {{^}}[ui] imgui: usage error: an item ID equal to the window's own ID{{$}}
