// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// The Clipper group: `imgui.listClipper(count, itemHeight, callback)`,
// which owns ImGuiListClipper's Begin/Step/End loop and calls the callback
// once per range, and the rules that hold while the callback runs (the
// design's "Exceptions: List clipper").
//
// Cases, in the order they run, one output line each (`<CASE> ok`; any
// failure prints its own line and exits 1):
//
//  RANGES    10000 items of getTextLineHeightWithSpacing() in a 300-unit
//            child: the callback's ranges are contiguous, start at 0 and
//            cover about 300 / height items (between that, rounded down,
//            minus 2 and it rounded up plus 2), and calls(h, 'text') grows
//            by exactly the number covered, not by 10000.
//  END       setScrollHereY(1) after the clipper (End() leaves the cursor
//            past the last item, imgui.cpp:3521, 3449-3456): frames run
//            until the last range ends at 10000; then getScrollY() equals
//            getScrollMaxY(), which lies between (10000 - visible - 2) *
//            height and 10000 * height, `visible` being the first range's
//            length in RANGES (the exact value includes item spacing and
//            window padding, imgui.cpp:3457, 8661, and is not asserted).
//  NESTED    a clipper of 3 inside the callback of a clipper of 5, in a
//            window that shows them all: the outer callback gets [0, 5],
//            the inner one [0, 3]; the same on the next frame.
//  FRAMEFNS  render() and newFrame() called from the callback: each throws
//            ERR_IMGUI_USAGE without doing anything; the clipper finishes
//            and the frame renders.
//  THROW     a callback throwing Error('boom'): listClipper throws that
//            error, the rest of the frame runs and renders, and the next
//            frame's clipper starts at 0 again.
//  SCOPE_*   a callback that changes the window scope, returning (`-ret`)
//            and throwing Error('own') after the change (`-throw`): it
//            closes the clipper's child with endChild()
//            (SCOPE_ENDCHILD), leaves a beginChild open
//            (SCOPE_OPENCHILD), is inside a table and calls endTable()
//            (SCOPE_ENDTABLE), leaves a non-scrolling beginTable open
//            (SCOPE_OPENTABLE), ends the clipper's window and begins it
//            again (SCOPE_REBEGINWINDOW), ends the clipper's table and
//            begins one with the same ID (SCOPE_REBEGINTABLE); the last two
//            are told apart only by the window's BeginCount and the
//            table's InstanceCurrent. The clipper is abandoned (traced): the
//            returning callback's call throws ERR_IMGUI_USAGE, the throwing
//            one's its own error; either way the callback ran once, the
//            cursor of the window current after the callback is where the
//            callback left it (an abandoned clipper's End() does not seek,
//            imgui.cpp:3521-3522), and after each the next frame draws (its
//            render() returns true) and a clipper in it gets its ranges
//            from 0.
//  FROZEN    a scrolled table with a frozen header row gives the clipper a
//            first range [0, 1] and at least one more; a callback throwing
//            on that first range is called once and its error propagates.
//  POOLGROW  a clipper in a table's cell whose callback, on its first
//            range, begins and properly ends more new nested tables than
//            ImGui's table pool has room for, so that the pool's storage
//            moves (read back: __test.tableScope().poolCapacity grew), and
//            returns with the outer table and window current: no error,
//            and the callback's ranges cover every item from 0 (Codex's
//            code review, C6: the scope is compared by the table's pool
//            index and ID, not its address).
//  REJECTED  itemHeight 0, -1, NaN and 0.5 (at least 1: Step() converts
//            distance / itemHeight to int, imgui.cpp:3692-3693), count -1
//            and 2147483647 (INT_MAX, ImGui's "unknown count"), and a list
//            taller than 2^30 units (count * itemHeight; End()'s seek and
//            the content size's ImTrunc64, imgui.cpp:3549-3556, 7250):
//            ERR_OUT_OF_RANGE, not called. count 0 is accepted and calls
//            the callback never; a list of exactly 2^30 units is accepted
//            and calls it.
//  COVERAGE  every Clipper function has reached ImGui.
//
// The guard listClipper runs before Begin() (canListClipper:wrong-window)
// is refused in ui-imgui-bind-errors.js (CLIPPER_WRONG_WINDOW).
//
// THE MUTATIONS THIS MUST BREAK (recorded in progress-imgui-binding.md):
//  the scope comparison dropping the table: SCOPE_ENDTABLE-ret and
//  SCOPE_OPENTABLE-ret fail (no throw; the Debug build does not abort);
//  ItemsCount not set to -1 on abandon: SCOPE_* fail (the cursor moved);
//  the callback depth not raised (CallbackScope not counting): FRAMEFNS
//  fails (render() runs and ends the frame, so the clipper is abandoned
//  and its ERR_IMGUI_USAGE escapes);
//  the callback not called (ImGui loop kept): RANGES fails;
//  enterImGui() removed from listClipper: REJECTED fails (NOTCALLED);
//  itemHeight's check removed: REJECTED fails;
//  the callback given (start, start): RANGES fails;
//  (codex) the table compared by address again: POOLGROW fails (the
//  clipper is abandoned, ERR_IMGUI_USAGE).
// The pending-exception check removed (Step() called again after a throw)
// is not detected, FROZEN's second range included: Hermes's
// napi_call_function refuses to call while an exception is pending
// (NAPI_PREAMBLE, hermes/API/napi/hermes_napi_impl.h:597-604), so the
// callback is not run again, and the remaining Step()s end where End()
// would (progress-imgui-binding.md, "Behaviours no test detects").
//
// REQUIRES: ui, ui-gpu, ui-imgui
// REQUIRES: display
//
// RUN: %with-display 300 sh -c 'HERMES_NODE_UI_TEST_HOOKS=1 HERMES_NODE_DEBUG_NATIVE=UI %hermes-node-ui %s > %t.out 2> %t.err'; echo "EXITCODE $?" | %FileCheck --check-prefix=CODE %s
// RUN: %FileCheck %s < %t.out
// RUN: %FileCheck --implicit-check-not='[sapp:error' --implicit-check-not='[sokol-' --check-prefix=TRACE %s < %t.err
// RUN: grep -c -F 'imgui: listClipper: abandoned, the callback changed the window scope' %t.err | %FileCheck --check-prefix=ABANDON %s

'use strict';

const harness = require('./fixtures/imgui/harness.js');

function fail(line) {
  console.log(line);
  process.exit(1);
}

/** Whether `ranges` are contiguous and start at 0. */
function contiguousFromZero(ranges) {
  if (ranges.length === 0 || ranges[0][0] !== 0)
    return false;
  for (let i = 0; i < ranges.length; ++i) {
    if (ranges[i][1] <= ranges[i][0])
      return false;
    if (i > 0 && ranges[i][0] !== ranges[i - 1][1])
      return false;
  }
  return true;
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
      fail('CALLED ' + name + ': ' + e.message);
    return e;
  }

  /** A window of its own at a fixed place; `body` draws in it. */
  function windowAt(name, body) {
    imgui.setNextWindowPos([20, 20]);
    imgui.setNextWindowSize([600, 440]);
    imgui.begin(name, null,
                imgui.WindowFlags_NoTitleBar | imgui.WindowFlags_NoMove |
                    imgui.WindowFlags_NoResize |
                    imgui.WindowFlags_NoSavedSettings);
    body();
    imgui.end();
  }

  let height = 0;
  await h.run(() => {
    height = imgui.getTextLineHeightWithSpacing();
  });
  if (!(height > 0))
    fail('SETUP height ' + height);

  /**
   * A plain clipper in a 300-unit child named `child`; its ranges and the
   * scroll read-backs go into `out`.
   */
  function listInChild(scrollToEnd, out, child = 'list') {
    windowAt('clip', () => {
      imgui.beginChild(child, [0, 300]);
      out.ranges = [];
      const before = calls('text');
      imgui.listClipper(10000, height, (start, end) => {
        out.ranges.push([start, end]);
        for (let i = start; i < end; ++i)
          imgui.text('item ' + i);
      });
      out.texts = calls('text') - before;
      if (scrollToEnd)
        imgui.setScrollHereY(1);
      out.scrollY = imgui.getScrollY();
      out.scrollMaxY = imgui.getScrollMaxY();
      imgui.endChild();
    });
  }

  // --- RANGES ----------------------------------------------------------
  let visible = 0;
  {
    const out = {};
    // The child's first frame sizes it; the second is the steady state.
    await h.run(() => listInChild(false, out));
    await h.run(() => listInChild(false, out));
    const r = out.ranges;
    const covered = r.length === 0 ? 0 : r[r.length - 1][1];
    visible = r.length === 0 ? 0 : r[0][1] - r[0][0];
    const lo = Math.floor(300 / height) - 2, hi = Math.ceil(300 / height) + 2;
    const ok = contiguousFromZero(r) && covered >= lo && covered <= hi &&
        out.texts === covered;
    console.log(ok ? 'RANGES ok' :
                     'RANGES WRONG ' +
                         JSON.stringify({r, texts: out.texts, lo, hi}));
  }

  // --- END -------------------------------------------------------------
  {
    const out = {};
    await h.run(() => listInChild(true, out));
    await harness.until(h, () => listInChild(false, out), () => {
      const r = out.ranges;
      return r.length !== 0 && r[r.length - 1][1] === 10000;
    }, 'END last range at 10000');
    const lo = (10000 - visible - 2) * height, hi = 10000 * height;
    const ok = out.scrollY === out.scrollMaxY && out.scrollMaxY >= lo &&
        out.scrollMaxY <= hi;
    console.log(ok ? 'END ok' :
                     'END WRONG ' + JSON.stringify({
                       scrollY: out.scrollY,
                       scrollMaxY: out.scrollMaxY,
                       lo,
                       hi,
                     }));
  }

  // --- NESTED ----------------------------------------------------------
  {
    const results = [];
    for (let frame = 0; frame < 2; ++frame) {
      const outer = [], inner = [];
      await h.run(() => windowAt('nested', () => {
        imgui.listClipper(5, height, (s, e) => {
          outer.push([s, e]);
          for (let i = s; i < e; ++i)
            imgui.text('outer ' + i);
          imgui.listClipper(3, height, (a, b) => {
            inner.push([a, b]);
            for (let i = a; i < b; ++i)
              imgui.text('inner ' + i);
          });
        });
      }));
      results.push(JSON.stringify({outer, inner}));
    }
    const want = JSON.stringify({outer: [[0, 5]], inner: [[0, 3]]});
    console.log(results.every((r) => r === want) ?
                    'NESTED ok' :
                    'NESTED WRONG ' + results.join(' '));
  }

  // --- FRAMEFNS --------------------------------------------------------
  {
    const codes = [];
    let frameOpen;
    const drew = await h.run(() => windowAt('framefns', () => {
      imgui.listClipper(3, height, (s, e) => {
        for (const fn of [() => imgui.render(), () => imgui.newFrame()]) {
          try {
            fn();
            codes.push('none');
          } catch (e) {
            codes.push(e.code);
          }
        }
        frameOpen = imgui.__test.state().frameOpen;
        for (let i = s; i < e; ++i)
          imgui.text('f ' + i);
      });
    }));
    const ok = drew === true && frameOpen === true &&
        codes.join() === 'ERR_IMGUI_USAGE,ERR_IMGUI_USAGE';
    console.log(ok ? 'FRAMEFNS ok' :
                     'FRAMEFNS WRONG drew=' + drew + ' codes=' + codes +
                         ' frameOpen=' + frameOpen);
  }

  /**
   * Whether a frame with a plain clipper (in a child never scrolled)
   * renders, with the clipper's ranges starting at 0.
   */
  async function nextFrameWorks() {
    const out = {};
    const drew = await h.run(() => listInChild(false, out, 'unscrolled'));
    return drew === true && contiguousFromZero(out.ranges);
  }

  // --- THROW -----------------------------------------------------------
  {
    let message = 'nothing', called = 0, after = false;
    const drew = await h.run(() => windowAt('throw', () => {
      try {
        imgui.listClipper(10, height, () => {
          ++called;
          throw new Error('boom');
        });
      } catch (e) {
        message = e.message;
      }
      imgui.text('after');
      after = true;
    }));
    const next = await nextFrameWorks();
    const ok = message === 'boom' && called === 1 && after && drew === true &&
        next;
    console.log(ok ? 'THROW ok' :
                     'THROW WRONG ' +
                         JSON.stringify({message, called, drew, next}));
  }

  // --- SCOPE_* ---------------------------------------------------------
  /**
   * One frame run with rawTick, since its render() may throw: `setup()`
   * opens what the clipper sits in, the clipper's callback runs
   * `change()` (then throws Error('own') when `throwing`), and `teardown()`
   * closes what the frame's code would close. Every closer and render() are
   * wrapped: what ImGui reports about the damage is not this case's
   * subject. Returns the clipper's error, how many times the callback ran,
   * and whether the next frame works.
   */
  async function scopeCase(setup, change, teardown, throwing) {
    let error = null, called = 0, inside = null, after = null;
    const quietly = (fn) => {
      try {
        fn();
      } catch (e) {
      }
    };
    await harness.rawTick(h, () => {
      imgui.newFrame();
      imgui.setNextWindowPos([20, 20]);
      imgui.setNextWindowSize([600, 440]);
      imgui.begin('scope', null, imgui.WindowFlags_NoSavedSettings);
      setup();
      try {
        imgui.listClipper(100, height, (s, e) => {
          ++called;
          change(s, e);
          inside = imgui.getCursorScreenPos();
          if (throwing)
            throw new Error('own');
        });
      } catch (e) {
        error = e;
      }
      after = imgui.getCursorScreenPos();
      quietly(teardown);
      quietly(() => imgui.end());
      quietly(() => imgui.render());
      return false;
    });
    await harness.resync(h);
    const next = await nextFrameWorks();
    const kept = inside !== null && after !== null &&
        inside[0] === after[0] && inside[1] === after[1];
    return {error, called, next, kept, inside, after};
  }

  const scopeCases = [
    ['SCOPE_ENDCHILD', () => imgui.beginChild('c', [0, 200]),
     () => imgui.endChild(), () => {}],
    ['SCOPE_OPENCHILD', () => imgui.beginChild('c', [0, 200]),
     () => imgui.beginChild('inner', [0, 50]), () => imgui.endChild()],
    ['SCOPE_ENDTABLE', () => imgui.beginTable('t', 1),
     (s) => {
       imgui.tableNextRow();
       imgui.tableNextColumn();
       imgui.text('row ' + s);
       imgui.endTable();
     },
     () => {}],
    ['SCOPE_OPENTABLE', () => imgui.beginChild('c', [0, 200]),
     () => imgui.beginTable('inner', 1), () => imgui.endChild()],
    // The same window and table again, ended and begun with the same
    // identity: only their begin counts tell them apart.
    ['SCOPE_REBEGINWINDOW', () => {},
     () => {
       imgui.end();
       imgui.begin('scope', null, imgui.WindowFlags_NoSavedSettings);
     },
     () => {}],
    ['SCOPE_REBEGINTABLE', () => imgui.beginTable('t', 1),
     (s) => {
       imgui.tableNextRow();
       imgui.tableNextColumn();
       imgui.text('row ' + s);
       imgui.endTable();
       imgui.beginTable('t', 1);
     },
     () => imgui.endTable()],
  ];
  const abandonMessages = [];
  for (const [label, setup, change, teardown] of scopeCases) {
    const ret = await scopeCase(setup, change, teardown, false);
    const thr = await scopeCase(setup, change, teardown, true);
    const retOk = ret.error !== null && ret.error.code === 'ERR_IMGUI_USAGE' &&
        ret.called === 1 && ret.kept && ret.next;
    const thrOk = thr.error !== null && thr.error.message === 'own' &&
        thr.called === 1 && thr.kept && thr.next;
    console.log(retOk ? label + '-ret ok' :
                        label + '-ret WRONG ' +
                            JSON.stringify({
                              error: ret.error && ret.error.message,
                              called: ret.called,
                              inside: ret.inside,
                              after: ret.after,
                              next: ret.next,
                            }));
    console.log(thrOk ? label + '-throw ok' :
                        label + '-throw WRONG ' +
                            JSON.stringify({
                              error: thr.error && thr.error.message,
                              called: thr.called,
                              inside: thr.inside,
                              after: thr.after,
                              next: thr.next,
                            }));
    if (ret.error)
      abandonMessages.push(ret.error.message);
  }
  if (abandonMessages.length !== 0)
    console.log('SCOPE message ' + abandonMessages[0]);

  // --- FROZEN ----------------------------------------------------------
  // A scrolled table with a frozen header row: the clipper's first range
  // is that row alone (imgui.cpp:3571-3578, the table not yet unfrozen,
  // imgui_tables.cpp:1866, which freezes rows only once the table has
  // scrolled), so there is a second range. A callback throwing on the
  // first: called once, its error propagates, and the table goes on.
  {
    const run = async (throwing, scroll = false) => {
      const ranges = [];
      let error = null;
      const drew = await h.run(() => windowAt('frozen', () => {
        imgui.beginTable('f', 1, imgui.TableFlags_ScrollY, [0, 150]);
        imgui.tableSetupScrollFreeze(0, 1);
        try {
          imgui.listClipper(100, height, (s, e) => {
            ranges.push([s, e]);
            if (throwing)
              throw new Error('frozen');
            for (let i = s; i < e; ++i) {
              imgui.tableNextRow();
              imgui.tableNextColumn();
              imgui.text('r ' + i);
            }
          });
        } catch (e) {
          error = e;
        }
        if (scroll)
          imgui.setScrollHereY(1);
        imgui.endTable();
      }));
      return {ranges, error, drew};
    };
    await run(false, true);
    let plain = null;
    // The scroll applies on a later frame; at most 600 of them, as
    // harness.until bounds its waits.
    for (let i = 0; i < 600; ++i) {
      plain = await run(false);
      if (plain.ranges.length >= 2)
        break;
    }
    const thrown = await run(true);
    const ok = plain.error === null && plain.ranges.length >= 2 &&
        plain.ranges[0][0] === 0 && plain.ranges[0][1] === 1 &&
        thrown.error !== null && thrown.error.message === 'frozen' &&
        JSON.stringify(thrown.ranges) === '[[0,1]]' && thrown.drew === true &&
        plain.drew === true;
    console.log(ok ? 'FROZEN ok' :
                     'FROZEN WRONG ' + JSON.stringify({
                       plain: plain.ranges,
                       thrown: thrown.ranges,
                       error: thrown.error && thrown.error.message,
                     }));
  }

  // --- POOLGROW --------------------------------------------------------
  {
    const ranges = [];
    let error = null, before = 0, after = 0, nested = 0;
    const drew = await h.run(() => windowAt('pool', () => {
      if (!imgui.beginTable('outer', 1))
        fail('POOLGROW outer table not shown');
      try {
        imgui.listClipper(3, height, (s, e) => {
          ranges.push([s, e]);
          for (let i = s; i < e; ++i) {
            imgui.tableNextRow();
            imgui.tableNextColumn();
            imgui.text('row ' + i);
          }
          if (ranges.length !== 1)
            return;
          // More new tables than the pool has room for, whatever is in it.
          before = imgui.__test.tableScope().poolCapacity;
          for (let n = 0; n <= before; ++n) {
            if (imgui.beginTable('nested ' + n, 1)) {
              imgui.tableNextRow();
              imgui.tableNextColumn();
              imgui.text('n');
              imgui.endTable();
              ++nested;
            }
          }
          after = imgui.__test.tableScope().poolCapacity;
        });
      } catch (e) {
        error = e;
      }
      imgui.endTable();
    }));
    const ok = error === null && drew === true && after > before &&
        nested === before + 1 && contiguousFromZero(ranges) &&
        ranges[ranges.length - 1][1] === 3;
    console.log(ok ? 'POOLGROW ok' :
                     'POOLGROW WRONG ' + JSON.stringify({
                       error: error && error.message, before, after, nested,
                       ranges, drew,
                     }));
    console.log('POOLGROW capacity ' + before + ' -> ' + after);
  }

  // --- REJECTED --------------------------------------------------------
  {
    const messages = [];
    let zeroCalled = 0, edgeCalled = 0;
    await h.run(() => windowAt('rejected', () => {
      for (const ih of [0, -1, NaN, 0.5])
        messages.push(rejects('listClipper',
                              () => imgui.listClipper(10, ih, () => {}),
                              'ERR_OUT_OF_RANGE').message);
      for (const n of [-1, 2147483647])
        messages.push(rejects('listClipper',
                              () => imgui.listClipper(n, height, () => {}),
                              'ERR_OUT_OF_RANGE').message);
      // The list's height, count * itemHeight, at most 2^30.
      for (const [n, ih] of [[1073741825, 1], [10, 268435456]])
        messages.push(rejects('listClipper',
                              () => imgui.listClipper(n, ih, () => {}),
                              'ERR_OUT_OF_RANGE').message);
      imgui.beginChild('edge', [0, 100]);
      imgui.listClipper(536870912, 2, () => ++edgeCalled);
      imgui.endChild();
      const before = calls('listClipper');
      imgui.listClipper(0, height, () => ++zeroCalled);
      if (calls('listClipper') !== before + 1)
        fail('NOTCALLED listClipper');
    }));
    if (zeroCalled !== 0)
      fail('REJECTED count 0 called the callback ' + zeroCalled);
    if (edgeCalled === 0)
      fail('REJECTED the 2^30-unit list never called its callback');
    console.log('REJECTED ok');
    for (const m of messages)
      console.log('REJECTED message ' + m);
  }

  harness.checkGroupCoverage(h, 'Clipper');
  process.exit(0);
}

main().catch((e) => {
  console.log('FAILED ' + (e && e.stack));
  process.exit(1);
});

// CHECK: {{^}}RANGES ok{{$}}
// CHECK-NEXT: {{^}}END ok{{$}}
// CHECK-NEXT: {{^}}NESTED ok{{$}}
// CHECK-NEXT: {{^}}FRAMEFNS ok{{$}}
// CHECK-NEXT: {{^}}THROW ok{{$}}
// CHECK-NEXT: {{^}}SCOPE_ENDCHILD-ret ok{{$}}
// CHECK-NEXT: {{^}}SCOPE_ENDCHILD-throw ok{{$}}
// CHECK-NEXT: {{^}}SCOPE_OPENCHILD-ret ok{{$}}
// CHECK-NEXT: {{^}}SCOPE_OPENCHILD-throw ok{{$}}
// CHECK-NEXT: {{^}}SCOPE_ENDTABLE-ret ok{{$}}
// CHECK-NEXT: {{^}}SCOPE_ENDTABLE-throw ok{{$}}
// CHECK-NEXT: {{^}}SCOPE_OPENTABLE-ret ok{{$}}
// CHECK-NEXT: {{^}}SCOPE_OPENTABLE-throw ok{{$}}
// CHECK-NEXT: {{^}}SCOPE_REBEGINWINDOW-ret ok{{$}}
// CHECK-NEXT: {{^}}SCOPE_REBEGINWINDOW-throw ok{{$}}
// CHECK-NEXT: {{^}}SCOPE_REBEGINTABLE-ret ok{{$}}
// CHECK-NEXT: {{^}}SCOPE_REBEGINTABLE-throw ok{{$}}
// CHECK-NEXT: {{^}}SCOPE message imgui.listClipper(): the callback changed the window scope (it closed the clipper's window or table, or left one open); the clipper was abandoned{{$}}
// CHECK-NEXT: {{^}}FROZEN ok{{$}}
// CHECK-NEXT: {{^}}POOLGROW ok{{$}}
// CHECK-NEXT: {{^}}POOLGROW capacity {{[0-9]+}} -> {{[0-9]+}}{{$}}
// CHECK-NEXT: {{^}}REJECTED ok{{$}}
// CHECK-NEXT: {{^}}REJECTED message imgui.listClipper(): itemHeight must be at least 1; got 0{{$}}
// CHECK-NEXT: {{^}}REJECTED message imgui.listClipper(): itemHeight must be at least 1; got -1{{$}}
// CHECK-NEXT: {{^}}REJECTED message imgui.listClipper(): itemHeight must be a finite number{{.*}}{{$}}
// CHECK-NEXT: {{^}}REJECTED message imgui.listClipper(): itemHeight must be at least 1; got 0.5{{$}}
// CHECK-NEXT: {{^}}REJECTED message imgui.listClipper(): count must be an integer from 0 to 2147483646; got -1{{$}}
// CHECK-NEXT: {{^}}REJECTED message imgui.listClipper(): count must be an integer from 0 to 2147483646; got 2147483647{{$}}
// CHECK-NEXT: {{^}}REJECTED message imgui.listClipper(): count * itemHeight must be at most 1073741824; got 1.07374e+09{{$}}
// CHECK-NEXT: {{^}}REJECTED message imgui.listClipper(): count * itemHeight must be at most 1073741824; got 2.68435e+09{{$}}
// CHECK-NEXT: {{^}}COVERAGE Clipper ok{{$}}
// CODE: EXITCODE 0

// Each SCOPE_* case abandons its clipper once per run, twelve in all, and
// the trace says so once for each: ABANDON counts the lines, so a missing
// or a doubled one fails.
// TRACE: {{^}}[ui] imgui: context created
// ABANDON: {{^}}12{{$}}
