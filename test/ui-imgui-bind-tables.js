// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// The Tables group: column geometry, the header row in pixels, the row and
// column functions, a frozen header row in a scrolling table, the group's
// "Rejected" lines, and its coverage. The usage errors and guard refusals
// of these functions are in ui-imgui-bind-errors.js (TABLE_*).
//
// Cases, in the order they run, one output line each (`<CASE> ok`; any
// failure prints its own line and exits 1):
//
//  COLUMNS     a 3-column table with TableFlags_Borders and fixed widths
//              100, 150 and 200 (tableSetupColumn, ColumnFlags_WidthFixed):
//              from each cell's first item to the next one's, x grows by
//              exactly the column's width plus a residual that is the same
//              for both pairs of adjacent cells (ImGui's layout puts each cell
//              at the previous one's start plus its WidthGiven, which is
//              the width asked for here, plus CellSpacingX1 +
//              CellSpacingX2 + 2 * CellPaddingX, imgui_tables.cpp:1190-1198, 1282);
//              the same on the next frame.
//  HEADERS     the same table with tableHeadersRow() first, TableHeaderBg
//              (and the Header colours a hover would use) pushed to an
//              opaque blue, WindowBg and the row colours to an opaque
//              green: on the header row's middle line, the pixel at each
//              column's centre is exactly blue; at the same x in the middle
//              of the first body row it is exactly green.
//  NEXTCOLUMN  tableNextColumn() four times in a 3-column table: each
//              returns true; the first three items share a y and sit at the
//              three columns' x (COLUMNS'); the fourth is at column 0's x
//              on a lower row.
//  SETCOLUMN   after tableNextRow(), tableSetColumnIndex(2) returns true and
//              the next item is at column 2's x; tableSetColumnIndex(0)
//              then puts the next one at column 0's x, on the same row.
//  SCROLL      a ScrollY table of outer size [0, 150] with
//              tableSetupScrollFreeze(0, 1), a header row and 30 body rows:
//              after an injected wheel event over the body, waited for on
//              the body's first item moving up, the header's rectangle is
//              exactly what it was.
//  REJECTED    beginTable with 0, -1, 1.5 or 512 columns, or with ScrollX
//              and a negative innerWidth; tableSetupColumn with any of the
//              four status flags, or WidthStretch | WidthFixed;
//              tableSetupScrollFreeze with cols -1 or 512, or rows -1 or
//              128: ERR_OUT_OF_RANGE, ImGui not called. beginTable with 511
//              columns, a negative innerWidth without ScrollX, a null
//              column label and tableSetupScrollFreeze(511, 127) are
//              accepted.
//  COVERAGE    every Tables function has reached ImGui.
//
// THE MUTATIONS THIS MUST BREAK (recorded in progress-imgui-binding.md):
//  ImGui::TableHeadersRow() removed, enterImGui() kept: HEADERS fails;
//  enterImGui() removed from tableSetupScrollFreeze: REJECTED fails
//  first (`NOTCALLED`, its accepted edge), before COVERAGE;
//  tableNextColumn() returning false always (its result not written back):
//  NEXTCOLUMN fails;
//  beginTable's column-count check removed: REJECTED fails (ImGui aborts);
//  tableSetupColumn passing 0 for its width: COLUMNS fails;
//  ImGui::TableSetupScrollFreeze() removed: SCROLL fails.
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
const WIDTHS = [100, 150, 200];
const BLUE = [0, 0, 255];
const GREEN = [0, 255, 0];

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
  const rect = () => harness.rect(imgui);

  /** `fn` must throw `code` and leave `name`'s call count unchanged. */
  function rejects(name, fn, code) {
    const before = calls(name);
    const e = harness.expectThrow(fn, code);
    if (calls(name) !== before)
      fail('CALLED ' + name + ': ' + e.message);
    return e;
  }

  /** `fn` must reach ImGui exactly once through `name`. */
  function accepted(name, fn) {
    const before = calls(name);
    const result = fn();
    if (calls(name) !== before + 1)
      fail('NOTCALLED ' + name);
    return result;
  }

  /** A window of its own, without a title bar, at a fixed place. */
  function windowAt(name, body) {
    imgui.setNextWindowPos([20, 20]);
    imgui.setNextWindowSize([600, 440]);
    imgui.begin(name, null,
                imgui.WindowFlags_NoTitleBar | imgui.WindowFlags_NoMove |
                    imgui.WindowFlags_NoResize);
    body();
    imgui.end();
  }

  /** The three fixed-width columns of COLUMNS, HEADERS and NEXTCOLUMN. */
  function setupColumns() {
    WIDTHS.forEach((w, i) => imgui.tableSetupColumn(
                       'ABC'[i], imgui.TableColumnFlags_WidthFixed, w));
  }

  // --- COLUMNS ---------------------------------------------------------
  let columnX;
  {
    let xs = [];
    const draw = () => windowAt('columns', () => {
      if (!imgui.beginTable('t', 3, imgui.TableFlags_Borders))
        return;
      setupColumns();
      imgui.tableNextRow();
      xs = [];
      for (let i = 0; i < 3; ++i) {
        imgui.tableSetColumnIndex(i);
        imgui.text('x');
        xs.push(rect().min[0]);
      }
      imgui.endTable();
    });
    await h.run(draw);
    const first = xs;
    await h.run(draw);
    const pad01 = xs[1] - xs[0] - WIDTHS[0];
    const pad12 = xs[2] - xs[1] - WIDTHS[1];
    const ok = xs.length === 3 && same(xs, first) && pad01 === pad12 &&
        pad01 > 0;
    console.log(ok ? 'COLUMNS ok' :
                     'COLUMNS WRONG ' + JSON.stringify({first, xs}));
    columnX = xs;
  }

  // --- HEADERS ---------------------------------------------------------
  {
    let headerRect = null, bodyRect = null;
    const draw = () => {
      for (const col of [imgui.Col_TableHeaderBg, imgui.Col_Header,
                         imgui.Col_HeaderHovered, imgui.Col_HeaderActive])
        imgui.pushStyleColor(col, [0, 0, 1, 1]);
      for (const col of [imgui.Col_WindowBg, imgui.Col_TableRowBg,
                         imgui.Col_TableRowBgAlt])
        imgui.pushStyleColor(col, [0, 1, 0, 1]);
      windowAt('headers', () => {
        if (!imgui.beginTable('t', 3, imgui.TableFlags_Borders))
          return;
        setupColumns();
        imgui.tableHeadersRow();
        headerRect = rect();
        imgui.tableNextRow();
        imgui.tableSetColumnIndex(0);
        imgui.text('x');
        bodyRect = rect();
        imgui.endTable();
      });
      imgui.popStyleColor(7);
    };
    await h.run(draw);
    await h.run(draw);
    const image = await harness.capture(h, draw, capturePath);
    const headerY = Math.floor((headerRect.min[1] + headerRect.max[1]) / 2);
    const bodyY = Math.floor((bodyRect.min[1] + bodyRect.max[1]) / 2);
    const wrong = [];
    columnX.forEach((x, i) => {
      const cx = Math.floor(x + WIDTHS[i] / 2);
      const head = harness.pixelAt(image, cx, headerY);
      const body = harness.pixelAt(image, cx, bodyY);
      if (!same(head, BLUE))
        wrong.push('header ' + cx + ',' + headerY + '=' + head);
      if (!same(body, GREEN))
        wrong.push('body ' + cx + ',' + bodyY + '=' + body);
    });
    // The header row ends above the body row, at least 3 px from each
    // sample (the samples are away from the row edges).
    if (!(headerRect.max[1] - headerY >= 3 && headerY - headerRect.min[1] >= 3 &&
          bodyRect.min[1] >= headerRect.max[1]))
      wrong.push('geometry ' + JSON.stringify({headerRect, bodyRect}));
    console.log(wrong.length === 0 ? 'HEADERS ok' :
                                     'HEADERS WRONG ' + wrong.join(' '));
  }

  // --- NEXTCOLUMN ------------------------------------------------------
  {
    let results = [], rects = [];
    await h.run(() => windowAt('columns', () => {
      if (!imgui.beginTable('t', 3, imgui.TableFlags_Borders))
        return;
      setupColumns();
      results = [];
      rects = [];
      for (let i = 0; i < 4; ++i) {
        results.push(imgui.tableNextColumn());
        imgui.text('n' + i);
        rects.push(rect());
      }
      imgui.endTable();
    }));
    const y0 = rects[0].min[1];
    const ok = same(results, [true, true, true, true]) &&
        rects.slice(0, 3).every((r, i) => r.min[1] === y0 &&
                                          r.min[0] === columnX[i]) &&
        rects[3].min[0] === columnX[0] && rects[3].min[1] > y0;
    console.log(ok ? 'NEXTCOLUMN ok' :
                     'NEXTCOLUMN WRONG ' + JSON.stringify({results, rects}));
  }

  // --- SETCOLUMN -------------------------------------------------------
  {
    let v2, v0, r2, r0;
    await h.run(() => windowAt('columns', () => {
      if (!imgui.beginTable('t', 3, imgui.TableFlags_Borders))
        return;
      setupColumns();
      imgui.tableNextRow();
      v2 = imgui.tableSetColumnIndex(2);
      imgui.text('c');
      r2 = rect();
      v0 = imgui.tableSetColumnIndex(0);
      imgui.text('a');
      r0 = rect();
      imgui.endTable();
    }));
    const ok = v2 === true && v0 === true && r2.min[0] === columnX[2] &&
        r0.min[0] === columnX[0] && r0.min[1] === r2.min[1];
    console.log(ok ? 'SETCOLUMN ok' :
                     'SETCOLUMN WRONG ' + JSON.stringify({v2, v0, r2, r0}));
  }

  // --- SCROLL ----------------------------------------------------------
  {
    let headerRect = null, bodyRect = null;
    const draw = () => windowAt('scroll', () => {
      if (!imgui.beginTable('s', 1, imgui.TableFlags_ScrollY, [0, 150]))
        return;
      imgui.tableSetupScrollFreeze(0, 1);
      imgui.tableSetupColumn('A');
      imgui.tableHeadersRow();
      headerRect = rect();
      for (let i = 0; i < 30; ++i) {
        imgui.tableNextRow();
        imgui.tableSetColumnIndex(0);
        imgui.text('row ' + i);
        if (i === 0)
          bodyRect = rect();
      }
      imgui.endTable();
    });
    await h.run(draw);
    await h.run(draw);
    const header0 = headerRect;
    const body0 = bodyRect;
    // A point over the body, below the header row.
    const at = [Math.floor(body0.min[0] + 40),
                Math.floor(header0.max[1] + 60)];
    await harness.moveTo(h, draw, at);
    harness.inject(
        h, {type: 'MOUSE_SCROLL', scrollY: -1, mouseX: at[0], mouseY: at[1]});
    await harness.until(h, draw, () => bodyRect.min[1] < body0.min[1],
                        'SCROLL body moved');
    const ok = same(headerRect.min, header0.min) &&
        same(headerRect.max, header0.max);
    console.log(ok ? 'SCROLL ok' :
                     'SCROLL WRONG ' + JSON.stringify(
                         {header0, headerRect, body0, bodyRect}));
  }

  // --- REJECTED --------------------------------------------------------
  {
    const messages = [];
    await h.run(() => windowAt('rejected', () => {
      for (const n of [0, -1, 1.5, 512])
        messages.push(rejects('beginTable', () => imgui.beginTable('r', n),
                              'ERR_OUT_OF_RANGE').message);
      messages.push(
          rejects('beginTable',
                  () => imgui.beginTable('r', 2, imgui.TableFlags_ScrollX,
                                         [0, 100], -1),
                  'ERR_OUT_OF_RANGE').message);
      if (accepted('beginTable', () => imgui.beginTable('max', 511))) {
        imgui.tableNextRow();
        imgui.endTable();
      }
      if (accepted('beginTable',
                   () => imgui.beginTable('neg', 2, 0, [0, 0], -1))) {
        imgui.tableNextRow();
        imgui.endTable();
      }
      if (!imgui.beginTable('cols', 2, imgui.TableFlags_ScrollY, [0, 100]))
        fail('REJECTED table not shown');
      for (const status of [imgui.TableColumnFlags_IsEnabled,
                            imgui.TableColumnFlags_IsVisible,
                            imgui.TableColumnFlags_IsSorted,
                            imgui.TableColumnFlags_IsHovered])
        messages.push(rejects('tableSetupColumn',
                              () => imgui.tableSetupColumn('a', status),
                              'ERR_OUT_OF_RANGE').message);
      messages.push(
          rejects('tableSetupColumn',
                  () => imgui.tableSetupColumn(
                      'a', imgui.TableColumnFlags_WidthStretch |
                          imgui.TableColumnFlags_WidthFixed),
                  'ERR_OUT_OF_RANGE').message);
      accepted('tableSetupColumn', () => imgui.tableSetupColumn(null));
      for (const [cols, rows] of [[-1, 0], [512, 0], [0, -1], [0, 128]])
        messages.push(rejects('tableSetupScrollFreeze',
                              () => imgui.tableSetupScrollFreeze(cols, rows),
                              'ERR_OUT_OF_RANGE').message);
      accepted('tableSetupScrollFreeze',
               () => imgui.tableSetupScrollFreeze(511, 127));
      imgui.tableNextRow();
      imgui.endTable();
    }));
    console.log('REJECTED ok');
    for (const m of messages)
      console.log('REJECTED message ' + m);
  }

  harness.checkGroupCoverage(h, 'Tables');
  process.exit(0);
}

main().catch((e) => {
  console.log('FAILED ' + (e && e.stack));
  process.exit(1);
});

// CHECK: {{^}}COLUMNS ok{{$}}
// CHECK-NEXT: {{^}}HEADERS ok{{$}}
// CHECK-NEXT: {{^}}NEXTCOLUMN ok{{$}}
// CHECK-NEXT: {{^}}SETCOLUMN ok{{$}}
// CHECK-NEXT: {{^}}SCROLL ok{{$}}
// CHECK-NEXT: {{^}}REJECTED ok{{$}}
// CHECK-NEXT: {{^}}REJECTED message imgui.beginTable(): columns must be an integer from 1 to 511; got 0{{$}}
// CHECK-NEXT: {{^}}REJECTED message imgui.beginTable(): columns must be an integer from 1 to 511; got -1{{$}}
// CHECK-NEXT: {{^}}REJECTED message imgui.beginTable(): columns must be an integer{{.*}}{{$}}
// CHECK-NEXT: {{^}}REJECTED message imgui.beginTable(): columns must be an integer from 1 to 511; got 512{{$}}
// CHECK-NEXT: {{^}}REJECTED message imgui.beginTable(): innerWidth must not be negative with TableFlags_ScrollX; got -1{{$}}
// CHECK-NEXT: {{^}}REJECTED message imgui.tableSetupColumn(): flags must not hold TableColumnFlags_IsEnabled, _IsVisible, _IsSorted or _IsHovered{{$}}
// CHECK-NEXT: {{^}}REJECTED message imgui.tableSetupColumn(): flags must not hold TableColumnFlags_IsEnabled, _IsVisible, _IsSorted or _IsHovered{{$}}
// CHECK-NEXT: {{^}}REJECTED message imgui.tableSetupColumn(): flags must not hold TableColumnFlags_IsEnabled, _IsVisible, _IsSorted or _IsHovered{{$}}
// CHECK-NEXT: {{^}}REJECTED message imgui.tableSetupColumn(): flags must not hold TableColumnFlags_IsEnabled, _IsVisible, _IsSorted or _IsHovered{{$}}
// CHECK-NEXT: {{^}}REJECTED message imgui.tableSetupColumn(): flags must not hold both TableColumnFlags_WidthStretch and TableColumnFlags_WidthFixed{{$}}
// CHECK-NEXT: {{^}}REJECTED message imgui.tableSetupScrollFreeze(): cols must be an integer from 0 to 511; got -1{{$}}
// CHECK-NEXT: {{^}}REJECTED message imgui.tableSetupScrollFreeze(): cols must be an integer from 0 to 511; got 512{{$}}
// CHECK-NEXT: {{^}}REJECTED message imgui.tableSetupScrollFreeze(): rows must be an integer from 0 to 127; got -1{{$}}
// CHECK-NEXT: {{^}}REJECTED message imgui.tableSetupScrollFreeze(): rows must be an integer from 0 to 127; got 128{{$}}
// CHECK-NEXT: {{^}}COVERAGE Tables ok{{$}}
// CODE: EXITCODE 0

// No case here is a usage error, so none is traced (the
// --implicit-check-not on the TRACE run).
// TRACE: {{^}}[ui] imgui: context created
