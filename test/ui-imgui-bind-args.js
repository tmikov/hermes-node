// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// The binding's argument conventions: what a wrong argument throws, and
// that a rejected call does not reach ImGui (its `__test.bindCalls()`
// count does not move). Also the Frame group's behaviour, and its
// coverage.
//
// Cases, in the order they run, one output line each (`<CASE> ok`; any
// failure prints its own line and exits 1):
//
//  EITHER    before the first newFrame(), the out-of-frame functions
//            return values of the right type; getDisplaySize(out) returns
//            `out` itself, filled.
//  NOFRAME   every bound function that needs a frame, with valid
//            arguments, outside a frame: ERR_IMGUI_NO_FRAME, not called.
//            A function in bind-groups.js with no sample arguments here is
//            `NOSAMPLE <name>`, so each later task must add its own.
//  ARGTYPE   a value of the wrong type, or a missing one: TypeError,
//            ERR_INVALID_ARG_TYPE; the message names the function and the
//            argument (checked on begin, and on the three functions that
//            dispatch on an argument's type, whose message names both
//            forms).
//  REFARRAY  reference arrays that are not what they claim (Review Focus
//            1): too short is ERR_OUT_OF_RANGE; a wrong element type, a
//            hole, a Float32Array where an Array is required, and an array
//            whose index-0 accessor shortens it are ERR_INVALID_ARG_TYPE.
//  FLOAT     NaN, Infinity and 1e300 as a float, and a slider bound past
//            FLT_MAX/2: ERR_OUT_OF_RANGE.
//  NUL       a string holding U+0000: ERR_INVALID_ARG_VALUE.
//  REENTRANT an index accessor that calls imgui.render() while checkbox
//            reads it: that call throws ERR_IMGUI_REENTRANT and checkbox
//            goes on; an accessor that throws on read stops checkbox before
//            ImGui; one that throws on write propagates after ImGui.
//  OPTIONAL  begin('w'), begin('w', undefined, 0) and begin('w', null) all
//            draw: true returned, the window active in state().windows.
//  FRAME     showDemoWindow and showMetricsWindow open their windows and
//            write their pOpen back; showDemoWindow() with no argument
//            works; getDisplaySize() after a frame is the window's size.
//  COVERAGE  the Frame group has reached ImGui throughout.
//
// THE MUTATIONS THIS MUST BREAK (recorded in progress-imgui-binding.md):
//  the re-entrancy flag never set: REENTRANT fails;
//  enterImGui() moved before the argument reads: every "not called" check
//  fails;
//  an argument check removed (the element-type check of refBool):
//  REFARRAY fails;
//  the write-back of showDemoWindow's pOpen removed: FRAME fails;
//  ShowMetricsWindow() removed with enterImGui() kept: FRAME fails;
//  enterImGui() removed from getDeltaTime: COVERAGE fails.
//
// REQUIRES: ui, ui-gpu, ui-imgui
// REQUIRES: display
//
// RUN: %with-display 300 sh -c 'HERMES_NODE_UI_TEST_HOOKS=1 HERMES_NODE_DEBUG_NATIVE=UI %hermes-node-ui %s > %t.out 2> %t.err'; echo "EXITCODE $?" | %FileCheck --check-prefix=CODE %s
// RUN: %FileCheck %s < %t.out
// RUN: %FileCheck --implicit-check-not='[sapp:error' --implicit-check-not='[sokol-' --implicit-check-not='imgui: usage error' --check-prefix=TRACE %s < %t.err

'use strict';

const harness = require('./fixtures/imgui/harness.js');
const bindGroups = require('./fixtures/imgui/bind-groups.js');

/** The functions that work outside a frame (Frame: either). */
const EITHER = [
  'getTime',
  'getFrameRate',
  'getDeltaTime',
  'getDisplaySize',
  'wantCaptureMouse',
  'wantCaptureKeyboard',
];

/**
 * Valid arguments for every bound function that needs a frame, so that
 * the frame check, not an argument check, is what refuses it.
 */
const SAMPLES = {
  render: [],
  showDemoWindow: [],
  showMetricsWindow: [],
  begin: ['w'],
  end: [],
  beginChild: ['c'],
  endChild: [],
  setNextWindowPos: [[0, 0]],
  setNextWindowSize: [[10, 10]],
  beginGroup: [],
  endGroup: [],
  setCursorScreenPos: [[0, 0]],
  pushID: ['a'],
  popID: [],
  text: ['t'],
  button: ['b'],
  checkbox: ['c', [false]],
  sliderFloat3: ['s', [0, 0, 0], 0, 1],
  getItemRectMin: [],
  getItemRectMax: [],
  pushStyleColor: [0, 0],
  popStyleColor: [],
  pushStyleVar: [0, 1],
  popStyleVar: [],
};

/** Names that are not called: a property, and the frame opener itself. */
const NOT_CALLED = ['version', 'newFrame'];

function fail(line) {
  console.log(line);
  process.exit(1);
}

async function main() {
  const h = await harness.open();
  const {imgui} = h;
  const calls = (name) => harness.calls(h, name);

  /**
   * `fn` must throw `code` and leave `name`'s call count where it was.
   * Returns the error.
   */
  function rejects(name, fn, code) {
    const before = calls(name);
    const e = harness.expectThrow(fn, code);
    if (calls(name) !== before)
      fail('CALLED ' + name + ' ' + code + ': ' + e.message);
    return e;
  }

  // Inside one window, so that a call that is wrongly let through has a
  // window to land in rather than failing for another reason.
  const inWindow = (fn) => harness.frame(h, () => {
    imgui.begin('args');
    fn();
    imgui.end();
  });

  // --- EITHER ----------------------------------------------------------
  {
    const bad = [];
    for (const name of ['getTime', 'getFrameRate', 'getDeltaTime']) {
      if (typeof imgui[name]() !== 'number')
        bad.push(name);
    }
    for (const name of ['wantCaptureMouse', 'wantCaptureKeyboard']) {
      if (typeof imgui[name]() !== 'boolean')
        bad.push(name);
    }
    const size = imgui.getDisplaySize();
    if (!Array.isArray(size) || size.length !== 2 ||
        typeof size[0] !== 'number' || typeof size[1] !== 'number')
      bad.push('getDisplaySize');
    const out = [];
    const got = imgui.getDisplaySize(out);
    if (got !== out || out[0] !== size[0] || out[1] !== size[1])
      bad.push('getDisplaySize(out)');
    if (imgui.__test.state().frameOpen)
      bad.push('frameOpen');
    console.log(bad.length === 0 ? 'EITHER ok' : 'EITHER WRONG ' + bad);
  }

  // --- NOFRAME ---------------------------------------------------------
  {
    for (const name of bindGroups.all()) {
      if (NOT_CALLED.includes(name) || EITHER.includes(name))
        continue;
      if (!(name in SAMPLES))
        fail('NOSAMPLE ' + name);
      rejects(name, () => imgui[name](...SAMPLES[name]),
              'ERR_IMGUI_NO_FRAME');
    }
    console.log('NOFRAME ok');
  }

  // --- ARGTYPE ---------------------------------------------------------
  {
    let message;
    const dispatched = [];
    await inWindow(() => {
      message = rejects('begin', () => imgui.begin(1),
                        'ERR_INVALID_ARG_TYPE').message;
      rejects('checkbox', () => imgui.checkbox('a', true),
              'ERR_INVALID_ARG_TYPE');
      rejects('text', () => imgui.text(), 'ERR_INVALID_ARG_TYPE');
      rejects('sliderFloat3', () => imgui.sliderFloat3('s', [1, 2, 3], 'x', 1),
              'ERR_INVALID_ARG_TYPE');
      // Dispatched arguments: the message names both forms.
      dispatched.push(rejects('pushStyleColor',
                              () => imgui.pushStyleColor(0, 'x'),
                              'ERR_INVALID_ARG_TYPE').message);
      dispatched.push(rejects('pushStyleVar', () => imgui.pushStyleVar(0, 'x'),
                              'ERR_INVALID_ARG_TYPE').message);
      dispatched.push(rejects('pushID', () => imgui.pushID(true),
                              'ERR_INVALID_ARG_TYPE').message);
    });
    console.log('ARGTYPE ok');
    console.log('ARGTYPE message ' + message);
    for (const m of dispatched)
      console.log('ARGTYPE dispatch ' + m);
  }

  // --- REFARRAY --------------------------------------------------------
  {
    // Three elements, of which reading index 0 truncates the array to one:
    // the later elements are then read as undefined.
    const shrinking = [0, 0, 0];
    Object.defineProperty(shrinking, 0, {
      get() {
        shrinking.length = 1;
        return 0.5;
      },
      configurable: true,
      enumerable: true,
    });
    await inWindow(() => {
      rejects('checkbox', () => imgui.checkbox('a', []), 'ERR_OUT_OF_RANGE');
      rejects('checkbox', () => imgui.checkbox('a', [1]),
              'ERR_INVALID_ARG_TYPE');
      rejects('sliderFloat3', () => imgui.sliderFloat3('s', [1, 2], 0, 1),
              'ERR_OUT_OF_RANGE');
      // eslint-disable-next-line no-sparse-arrays
      rejects('sliderFloat3', () => imgui.sliderFloat3('s', [, 1, 2], 0, 1),
              'ERR_INVALID_ARG_TYPE');
      rejects('sliderFloat3',
              () => imgui.sliderFloat3('s', new Float32Array(3), 0, 1),
              'ERR_INVALID_ARG_TYPE');
      rejects('sliderFloat3', () => imgui.sliderFloat3('s', shrinking, 0, 1),
              'ERR_INVALID_ARG_TYPE');
    });
    if (shrinking.length !== 1)
      fail('REFARRAY the accessor did not run');
    console.log('REFARRAY ok');
  }

  // --- FLOAT -----------------------------------------------------------
  {
    await inWindow(() => {
      rejects('sliderFloat3', () => imgui.sliderFloat3('s', [NaN, 0, 0], 0, 1),
              'ERR_OUT_OF_RANGE');
      rejects('sliderFloat3', () => imgui.sliderFloat3('s', [0, 0, 0], 0,
                                                       Infinity),
              'ERR_OUT_OF_RANGE');
      rejects('sliderFloat3', () => imgui.sliderFloat3('s', [0, 1e300, 0], 0,
                                                       1),
              'ERR_OUT_OF_RANGE');
      rejects('sliderFloat3', () => imgui.sliderFloat3('s', [0, 0, 0], 1e300,
                                                       1),
              'ERR_OUT_OF_RANGE');
      // Within float range, beyond SliderBehavior's FLT_MAX / 2.
      rejects('sliderFloat3', () => imgui.sliderFloat3('s', [0, 0, 0], -3e38,
                                                       1),
              'ERR_OUT_OF_RANGE');
    });
    console.log('FLOAT ok');
  }

  // --- NUL -------------------------------------------------------------
  {
    await inWindow(() => {
      rejects('text', () => imgui.text('a\0b'), 'ERR_INVALID_ARG_VALUE');
    });
    console.log('NUL ok');
  }

  // --- REENTRANT -------------------------------------------------------
  {
    let reentrantCode = 'none';
    const reentering = [];
    Object.defineProperty(reentering, 0, {
      get() {
        try {
          imgui.render();
        } catch (e) {
          reentrantCode = e.code;
        }
        return false;
      },
      set(v) {},
      configurable: true,
      enumerable: true,
    });
    const throwsOnRead = [];
    Object.defineProperty(throwsOnRead, 0, {
      get() {
        throw new Error('boom');
      },
      set(v) {},
      configurable: true,
      enumerable: true,
    });
    const throwsOnWrite = [];
    Object.defineProperty(throwsOnWrite, 0, {
      get() {
        return false;
      },
      set(v) {
        throw new Error('write boom');
      },
      configurable: true,
      enumerable: true,
    });
    const thrown = (fn) => {
      try {
        fn();
      } catch (e) {
        return e.message;
      }
      return 'nothing';
    };
    let readMessage, writeMessage, readMoved, writeMoved, reenterMoved;
    await inWindow(() => {
      let before = calls('checkbox');
      imgui.checkbox('r', reentering);
      reenterMoved = calls('checkbox') - before;
      before = calls('checkbox');
      readMessage = thrown(() => imgui.checkbox('r', throwsOnRead));
      readMoved = calls('checkbox') - before;
      before = calls('checkbox');
      writeMessage = thrown(() => imgui.checkbox('r', throwsOnWrite));
      writeMoved = calls('checkbox') - before;
    });
    const ok = reentrantCode === 'ERR_IMGUI_REENTRANT' && reenterMoved === 1 &&
        readMessage === 'boom' && readMoved === 0 &&
        writeMessage === 'write boom' && writeMoved === 1;
    console.log(
        ok ? 'REENTRANT ok' :
             'REENTRANT WRONG code=' + reentrantCode + ' reenter=' +
                 reenterMoved + ' read=' + readMessage + '/' + readMoved +
                 ' write=' + writeMessage + '/' + writeMoved);
  }

  // --- OPTIONAL --------------------------------------------------------
  {
    const results = [];
    await harness.frame(h, () => {
      results.push(imgui.begin('o1'));
      imgui.end();
      results.push(imgui.begin('o2', undefined, 0));
      imgui.end();
      results.push(imgui.begin('o3', null));
      imgui.end();
    });
    const windows = imgui.__test.state().windows;
    const active = (name) => windows.some((w) => w.name === name && w.active);
    const ok = results.every((r) => r === true) && active('o1') &&
        active('o2') && active('o3');
    console.log(
        ok ? 'OPTIONAL ok' :
             'OPTIONAL WRONG ' + results + ' ' + JSON.stringify(windows));
  }

  // --- FRAME -----------------------------------------------------------
  {
    // pOpen arrays that record what is written into them.
    const writes = {demo: [], metrics: []};
    const recording = (key) => {
      const a = [];
      Object.defineProperty(a, 0, {
        get() {
          return true;
        },
        set(v) {
          writes[key].push(v);
        },
        configurable: true,
        enumerable: true,
      });
      return a;
    };
    const drew = await harness.frame(h, () => {
      imgui.showDemoWindow(recording('demo'));
      imgui.showMetricsWindow(recording('metrics'));
    });
    const windows = imgui.__test.state().windows;
    const active = (name) => windows.some((w) => w.name === name && w.active);
    const bare = await harness.frame(h, () => {
      imgui.showDemoWindow();
      imgui.showMetricsWindow(null);
    });
    const size = imgui.getDisplaySize();
    const ok = drew && bare && active('Dear ImGui Demo') &&
        active('Dear ImGui Metrics/Debugger') &&
        JSON.stringify(writes) === '{"demo":[true],"metrics":[true]}' &&
        size[0] === h.win.width && size[1] === h.win.height &&
        imgui.getDeltaTime() > 0 && imgui.getTime() > 0;
    console.log(
        ok ? 'FRAME ok' :
             'FRAME WRONG drew=' + drew + '/' + bare + ' writes=' +
                 JSON.stringify(writes) + ' size=' + size +
                 ' windows=' + JSON.stringify(windows));
  }

  harness.checkGroupCoverage(h, 'Frame');
  process.exit(0);
}

main().catch((e) => {
  console.log('FAILED ' + (e && e.stack));
  process.exit(1);
});

// CHECK: {{^}}EITHER ok{{$}}
// CHECK-NEXT: {{^}}NOFRAME ok{{$}}
// CHECK-NEXT: {{^}}ARGTYPE ok{{$}}
// CHECK-NEXT: {{^}}ARGTYPE message imgui.begin(): name must be a string{{$}}
// CHECK-NEXT: {{^}}ARGTYPE dispatch imgui.pushStyleColor(): col must be a number or an array of 4 numbers{{$}}
// CHECK-NEXT: {{^}}ARGTYPE dispatch imgui.pushStyleVar(): val must be a number or an array of 2 numbers{{$}}
// CHECK-NEXT: {{^}}ARGTYPE dispatch imgui.pushID(): strId must be a string, or intId a number{{$}}
// CHECK-NEXT: {{^}}REFARRAY ok{{$}}
// CHECK-NEXT: {{^}}FLOAT ok{{$}}
// CHECK-NEXT: {{^}}NUL ok{{$}}
// CHECK-NEXT: {{^}}REENTRANT ok{{$}}
// CHECK-NEXT: {{^}}OPTIONAL ok{{$}}
// CHECK-NEXT: {{^}}FRAME ok{{$}}
// CHECK-NEXT: {{^}}COVERAGE Frame ok{{$}}
// CODE: EXITCODE 0

// No argument check is a usage error, so none is traced anywhere (the
// --implicit-check-not on the TRACE run).
// TRACE: {{^}}[ui] imgui: context created
