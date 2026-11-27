// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// The exported constants and the flags and enum checks they define (the
// surface table's "Constants" section and its rule G1).
//
// Cases, in the order they run, one output line each (`<CASE> ok`; any
// failure prints its own line and exits 1):
//
//  CONSTANTS for every type in bind-groups.js's constantTypes, the number
//            of exported names of that type equals the length of its
//            `names` (the table's count; imgui-bind-surface.js compares the
//            names themselves, without a display), and
//            none of the table's excluded names is exported; every sample
//            in bind-groups.js's `constants` (copied by hand from imgui.h)
//            equals the module's value. Otherwise `COUNT <type> <got> want
//            <n>`, `EXCLUDED <name>` and `WRONG <name>=<got> want <value>`.
//  FLT       imgui.FLT_MAX is a float value, FLT_MIN is positive, and
//            -FLT_MIN does not round to -0 as a float (which is what makes
//            it usable as ImGui's "align to the right").
//  HIGHBIT   which flags types export an enumerator with bit 31 set; none
//            do in this version, so the "negative value with a public high
//            bit is accepted" half of the rule has nothing to apply to, and
//            the line says so (`HIGHBIT none`). The other half is in REJECT:
//            a negative value made with `|` is read as a 32-bit integer and
//            refused by the mask, not by the integer reader.
//  REJECT    refused with ERR_OUT_OF_RANGE and not called: a window flag
//            no exported enumerator has (1 << 30), an internal one
//            (WindowFlags_ChildWindow), a non-integer (2.5), a negative
//            value with bit 31, and 2^31 and 2^32 - 1 (a flags argument is
//            a C int, so the unsigned spelling of bit 31 is out of range); beginChild's child and window flags; a
//            combined ImGuiCond (3) for setNextWindowPos/Size; a slider
//            flag in ImGuiSliderFlags_InvalidMask_ (bit 0); Col_COUNT and
//            -1 for pushStyleColor, StyleVar_COUNT for pushStyleVar.
//            Accepted and called: begin with NoTitleBar | NoResize (it
//            draws: true returned, the window active), beginChild with
//            ChildFlags_Borders, Cond_Always, the last colour and the first
//            style variable. The three `REJECT message` lines pin the text
//            of the flags, enum and `_COUNT` refusals.
//
// THE MUTATIONS THIS MUST BREAK (recorded in progress-imgui-binding.md):
//  one constant's HNIMGUI_CONST line deleted: CONSTANTS (`COUNT`);
//  a private enumerator (ImGuiWindowFlags_ChildWindow) added to a list:
//  CONSTANTS (`EXCLUDED`), and REJECT (the flag is then accepted);
//  the mask check removed from Call::flags: REJECT (`NOTHROW`);
//  the value check removed from Call::enumValue: REJECT (`NOTHROW`);
//  pushStyleColor's Col_COUNT refusal removed: REJECT (`NOTHROW`).
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

function fail(line) {
  console.log(line);
  process.exit(1);
}

async function main() {
  const h = await harness.open();
  const {imgui} = h;
  const calls = (name) => harness.calls(h, name);
  const has = (name) => Object.prototype.hasOwnProperty.call(imgui, name);

  // --- CONSTANTS -------------------------------------------------------
  {
    const bad = [];
    const counts = {};
    for (const name of Object.getOwnPropertyNames(imgui)) {
      const type = bindGroups.constantType(name);
      if (type !== undefined)
        counts[type] = (counts[type] || 0) + 1;
    }
    for (const [type, info] of Object.entries(bindGroups.constantTypes)) {
      if ((counts[type] || 0) !== info.names.length)
        bad.push('COUNT ' + type + ' ' + (counts[type] || 0) + ' want ' +
                 info.names.length);
      for (const name of info.excluded) {
        if (has(name))
          bad.push('EXCLUDED ' + name);
      }
    }
    if ((counts.FLT || 0) !== 2)
      bad.push('COUNT FLT ' + (counts.FLT || 0) + ' want 2');
    for (const [name, value] of Object.entries(bindGroups.constants)) {
      if (imgui[name] !== value)
        bad.push('WRONG ' + name + '=' + imgui[name] + ' want ' + value);
    }
    if (bad.length !== 0)
      fail(bad.join('\n'));
    console.log('CONSTANTS ok');
  }

  // --- FLT -------------------------------------------------------------
  {
    const ok = typeof imgui.FLT_MAX === 'number' &&
        Math.fround(imgui.FLT_MAX) === imgui.FLT_MAX &&
        Math.fround(imgui.FLT_MIN) === imgui.FLT_MIN && imgui.FLT_MIN > 0 &&
        Math.fround(-imgui.FLT_MIN) !== 0 &&
        // Not the double limits.
        imgui.FLT_MAX < Number.MAX_VALUE && imgui.FLT_MIN > Number.MIN_VALUE;
    if (!ok)
      fail('FLT WRONG max=' + imgui.FLT_MAX + ' min=' + imgui.FLT_MIN);
    console.log('FLT ok');
  }

  // --- HIGHBIT ---------------------------------------------------------
  {
    const high = [];
    for (const name of Object.getOwnPropertyNames(imgui)) {
      const type = bindGroups.constantType(name);
      if (type === undefined || type === 'FLT' ||
          bindGroups.constantTypes[type].kind !== 'flags')
        continue;
      if (imgui[name] < 0)
        high.push(name);
    }
    console.log('HIGHBIT ' + (high.length === 0 ? 'none' : high.join(' ')));
  }

  // --- REJECT ----------------------------------------------------------
  {
    const messages = [];
    /**
     * `fn` must throw ERR_OUT_OF_RANGE and leave `name`'s call count where
     * it was. Returns the message.
     */
    function rejects(name, fn) {
      const before = calls(name);
      const e = harness.expectThrow(fn, 'ERR_OUT_OF_RANGE');
      if (calls(name) !== before)
        fail('CALLED ' + name + ': ' + e.message);
      return e.message;
    }
    /** `fn` must reach ImGui exactly once for `name`. */
    function accepts(name, fn) {
      const before = calls(name);
      fn();
      if (calls(name) !== before + 1)
        fail('NOTCALLED ' + name);
    }

    let shown = false;
    await harness.frame(h, () => {
      imgui.begin('reject');
      messages.push(rejects('begin', () => imgui.begin('w', null, 1 << 30)));
      rejects('begin', () => imgui.begin('w', null, 1 << 24));
      rejects('begin', () => imgui.begin('w', null, 2.5));
      rejects('begin', () => imgui.begin('w', null, 2147483648));
      rejects('begin', () => imgui.begin('w', null, 4294967295));
      const negative = imgui.WindowFlags_NoTitleBar | (1 << 31);
      if (!(negative < 0))
        fail('REJECT the value is not negative: ' + negative);
      const m = rejects('begin', () => imgui.begin('w', null, negative));
      if (!m.endsWith(': 0x80000000'))
        fail('REJECT negative refused for another reason: ' + m);
      rejects('beginChild', () => imgui.beginChild('c', [0, 0], 1 << 9));
      rejects('beginChild', () => imgui.beginChild('c', [0, 0], 0, 1 << 24));
      messages.push(rejects('setNextWindowPos',
                            () => imgui.setNextWindowPos([0, 0], 3)));
      rejects('setNextWindowSize', () => imgui.setNextWindowSize([10, 10], 3));
      rejects('sliderFloat3',
              () => imgui.sliderFloat3('s', [0, 0, 0], 0, 1, '%.3f', 1));
      messages.push(rejects('pushStyleColor',
                            () => imgui.pushStyleColor(imgui.Col_COUNT, 0)));
      rejects('pushStyleColor', () => imgui.pushStyleColor(-1, 0));
      rejects('pushStyleVar',
              () => imgui.pushStyleVar(imgui.StyleVar_COUNT, 1));

      accepts('beginChild',
              () => imgui.beginChild('c', [20, 20], imgui.ChildFlags_Borders));
      imgui.endChild();
      accepts('pushStyleColor',
              () => imgui.pushStyleColor(imgui.Col_COUNT - 1, 0));
      imgui.popStyleColor();
      accepts('pushStyleVar',
              () => imgui.pushStyleVar(imgui.StyleVar_Alpha, 1));
      imgui.popStyleVar();
      imgui.end();

      accepts('setNextWindowPos',
              () => imgui.setNextWindowPos([10, 10], imgui.Cond_Always));
      accepts('setNextWindowSize',
              () => imgui.setNextWindowSize([100, 100], imgui.Cond_Always));
      accepts('begin', () => {
        shown = imgui.begin(
            'flags', null,
            imgui.WindowFlags_NoTitleBar | imgui.WindowFlags_NoResize);
      });
      imgui.end();
    });
    const windows = imgui.__test.state().windows;
    if (!(shown === true &&
          windows.some((w) => w.name === 'flags' && w.active)))
      fail('REJECT flags window not drawn: ' + shown + ' ' +
           JSON.stringify(windows));
    console.log('REJECT ok');
    for (const m of messages)
      console.log('REJECT message ' + m);
  }

  process.exit(0);
}

main().catch((e) => {
  console.log('FAILED ' + (e && e.stack));
  process.exit(1);
});

// CHECK: {{^}}CONSTANTS ok{{$}}
// CHECK-NEXT: {{^}}FLT ok{{$}}
// CHECK-NEXT: {{^}}HIGHBIT none{{$}}
// CHECK-NEXT: {{^}}REJECT ok{{$}}
// CHECK-NEXT: {{^}}REJECT message imgui.begin(): flags has bits set that no WindowFlags_ constant has: 0x40000000{{$}}
// CHECK-NEXT: {{^}}REJECT message imgui.setNextWindowPos(): cond must equal one of the Cond_ constants; got 3{{$}}
// CHECK-NEXT: {{^}}REJECT message imgui.pushStyleColor(): idx must not be Col_COUNT{{$}}
// CODE: EXITCODE 0

// No argument check is a usage error, so none is traced anywhere (the
// --implicit-check-not on the TRACE run).
// TRACE: {{^}}[ui] imgui: context created
