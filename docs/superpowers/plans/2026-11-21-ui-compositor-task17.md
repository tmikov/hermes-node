# Task 17: the full gated suite, written out (17a-17f)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Finish Slice D of the UI compositor: make every window test refuse
Sokol error lines by one rule that a test enforces, pin
`hnui_get_window()` after a late `createWindow` rejection, remove the two
assertions that depend on how the platform thread happens to batch injected
events, verify all of it on Metal, and close the slice in the documents and
the tracker.

**Architecture:** No product code changes. Three Linux tasks change tests
(one adds a Python checker and its lit test, one adds a window test, one
rewrites two assertions around the existing `ui.__waitPlatformFrames`
barrier); two Mac rounds verify them on Metal and run the whole suite; one
Linux task records the result. Every new assertion comes with a named
mutation that was applied on Linux while this plan was written, and the
outcome is recorded beside it.

**Tech stack:** LLVM lit and FileCheck (`test/lit.cfg`), Python 3 for the
checker, the reference addon in `test/fixtures/hnui-demo/`, `utils/lit-run.sh`,
`utils/with-display.py`, ditz2 (`dz`) for the tracker.

**Spec:** `docs/superpowers/specs/2026-09-18-ui-compositor-design.md`
("Testing", "Lifecycle"). Expands the bullet "Task 17 -- The full gated
suite" of `docs/superpowers/plans/2026-09-21-ui-compositor.md`. Written
2026-11-21 on Linux, from that bullet, `progress-ui-compositor.md`, the
tracker (`dz list`) and the tree as Task 16 left it.

## Global Constraints

- Everything is behind `HERMES_NODE_ENABLE_UI`; nothing here changes a build
  without it, except that `test/ui-stderr-rule.js` (17a) is ungated and so
  runs in every configuration.
- Assertions come from traces or from what was executed, never from timing.
  A test that needs "the platform thread got that far" waits on
  `ui.__waitPlatformFrames(n)` or `ui.__waitPlatformQuitRequests(total)`.
- Every RUN line that starts the binary keeps a time limit:
  `%with-display <seconds>` for a window, `%timeout <seconds>` otherwise.
- An exclusion is an `--implicit-check-not`, or a FileCheck run of its own.
  A `-NOT` between `-DAG` lines or after the last positive line cannot see
  what precedes its anchor.
- Every exclusion and every new assertion has a mutation that turns it red
  while every positive line still matches. The mutations are named
  (M1-M3, S, T, K, W) in "Mutations used by this plan".
- New files carry `Copyright (c) Tzvetan Mikov.` and the MIT header.
- No commit hash in any committed file. Name a commit by its subject.
- Commit messages: ASCII, imperative subject of at most 50 characters,
  trailer `Co-Authored-By: Claude Fable 5.2 <noreply@anthropic.com>`.
- Before any commit, on Linux: `./utils/format.sh -a` and
  `cmake --build cmake-build-asan --target check-hermes-node`.
- Never run two lit invocations over one build directory at once.
- The Mac is reached only through written requests. A Mac task's block is
  the request: exact commands, expected output. The Mac agent does not run
  the formatter and edits nothing outside what its task names.

## Review Focus

Failure modes this task's tests do not exercise, most likely first, each
with what covers it or why nothing does:

1. **A healthy Metal run prints a Sokol line Linux cannot predict.** No
   Mac report records the whole stderr of a healthy run. 17a adds
   exclusions to five tests that run on the Mac (`ui-close.js`,
   `ui-exit-paths.js`, `ui-terminate.js`, `ui-metal-maintenance.js`,
   `ui-metal-maintenance-wake.js`) from Linux. -- 17d Step 1 takes a census
   of every `[sapp:` and `[sokol-` line before anything else and stops on a
   surprise.
2. **A window test added later without the exclusion.** A sweep rots the
   day after it is done. -- 17a's `test/ui-stderr-rule.js` fails on any
   `%with-display` RUN line whose stderr is not read with the two
   exclusions; its mutation is 17a Step 6.
3. **A Sokol error on a path no test takes** (`CLIPBOARD_STRING_TOO_BIG`,
   `DROPPED_FILE_PATH_TOO_LONG`, an X11 clipboard-ownership failure). An
   exclusion sees only what a run prints. Nothing here adds such a run;
   the rule is what makes one cheap to add.
4. **The barrier times out under load.** `__waitPlatformFrames` gives up
   after 10 s and returns false. Both rewritten assertions print the
   barrier's answer (`KEY-QUEUED true`, `barrier true`), so a timeout is a
   red line that says so, not a wrong order blamed on the product.
5. **The third late-rejection cause, surface allocation**, is the
   backend's and cannot be provoked from a script. 17b covers the other
   two; this one is covered by reading: all three share one `abandon`
   lambda.

## What was and was not verified while writing this

Run on Linux, 2026-11-21, in `cmake-build-asan` at "Record the Mac
verification of Task 16", each with the tree restored afterwards:

- The survey in "Survey" below, by reading every RUN line and then by the
  17a checker itself: 23 nonconforming window RUN lines in 9 files before
  the sweep, 0 after.
- Mutations M1 and M2 against the tree as it stands: 13 of the 19 window
  test files that run on Linux go red; the six that stay green are the gap.
  Against the sweep: M1 19 red, M2 19 red, M3 18 red
  (`ui-sokol-validation.js` stays green, as it must).
- `test/ui-late-rejection.js` exactly as given in 17b: 6 of 6 passed.
  Mutation W: ASAN heap-use-after-free in `onWorkerLoopTurn`, status 1.
- Mutation S alone against today's tests: `ui-close.js` red 3 of 3
  (REPLACED), everything else green. S+T against today's `ui-events.js`:
  green 6 of 6, which is the hole. Against the 17c rewrite: S green 3 of 3
  over all UI tests; T red; S+T red; K red; S+K red; and a 32-run loop of
  `ui-events.js`, `ui-close.js` and `ui-event-exceptions.js` on the clean
  build: `32 0`.
- A healthy GL run prints exactly one Sokol line,
  `[sapp:warning] item 53: LINUX_X11_QUERY_SYSTEM_DPI_FAILED: ...`, in every
  window run (41 stderr files examined).

**Nothing here was run on macOS.** Statements about Metal that are not
quoted from a Mac report are marked `UNVERIFIED:`. The totals expected of
the Mac are derived from Task 16f's measured ones by arithmetic and are
marked as such.

## How this task is executed

As Task 16 was (`docs/superpowers/plans/2026-11-19-ui-compositor-task16.md`,
"How this task is executed"): the Linux driver owns `work-imgui` and makes
every commit; the Mac agent receives the commit under test on the channel
branch, named by subject, and answers with a report. In Task 17 the Mac
implements nothing. If a Mac round finds a test that needs a different
check line on Metal, the Mac agent proposes the line in its report, inside
a `-metal` check prefix or a Metal-only file, and the driver commits it.

**Linux commands:**

```bash
cd /home/tmikov/work/hermes-node-imgui; S=$(pwd); B=$S/cmake-build-asan
cmake --build $B --target hermes-node hermes-node-ui hnui_demo_addon 2>&1 | tail -1
utils/lit-run.sh $B -v $S/test/<name>.js            # named tests
utils/lit-run.sh $B $S/test/ui-*.js 2>&1 | tail -30  # every UI test
cmake --build $B --target check-hermes-node 2>&1 | tail -15   # whole suite
```

**Mac commands** (zsh or bash, BSD userland):

```bash
cd <checkout>; S=$(pwd); B=$S/cmake-build-asan-ui; R=$S/cmake-build-release-ui
cmake --build $B --target hermes-node hermes-node-ui hnui_demo_addon mac_pasteboard_tool 2>&1 | tail -1
HERMES_NODE_UI_EXPECT_DISPLAY=1 caffeinate -dimsu utils/lit-run.sh $B -v $S/test/<name>.js
HERMES_NODE_UI_EXPECT_DISPLAY=1 caffeinate -dimsu cmake --build $B --target check-hermes-node 2>&1 | tail -15
```

**A mutation build names both binaries**, `hermes-node` and
`hermes-node-ui`. With only the second rebuilt in a dirty tree,
`test/ui-bundle-argv.js` fails with `generation mismatch`, which has
nothing to do with the mutation (measured).

## Mutations used by this plan

One helper and one shell function per mutation, the same text on both
machines (bash or zsh; every function was applied on Linux as written).
Each is undone with `git checkout tools` and a rebuild of both binaries.

```bash
mut() { python3 - "$@" <<'EOF'
import sys
path, old, new = sys.argv[1:4]
s = open(path).read()
assert s.count(old) == 1, (path, old, s.count(old))
open(path, 'w').write(s.replace(old, new))
EOF
}
P=tools/hermes-node-ui/ui_platform.cpp
E=tools/hermes-node-ui/ui_events.cpp
READY='  uiTrace("init_cb: shared context ready");'
M1() { mut $P "$READY" "  sokolLog(\"sapp\", 1, 0, \"mutation\", 0, nullptr, nullptr);
$READY"; }
M2() { mut $P "$READY" "  uiSokolLog(\"sokol-present\", 2, 0, \"mutation\", 0, nullptr, nullptr);
$READY"; }
M3() { mut $P "$READY" "  uiSokolLog(\"sokol-produce\", 1, 0, \"mutation\", 0, nullptr, nullptr);
$READY"; }
S() {
  mut $P '#include <atomic>' $'#include <atomic>\n#include <chrono>\n#include <thread>'
  mut $P $'  InjectedPlatformEvent injected;\n  if (!gInjector.takeNext(&injected))\n    return;\n' \
    $'  InjectedPlatformEvent injected;\n  static bool tookLast = false;\n  if (tookLast) {\n    tookLast = false;\n    std::this_thread::sleep_for(std::chrono::milliseconds(50));\n  }\n  if (!gInjector.takeNext(&injected))\n    return;\n  tookLast = true;\n'
  mut $P '  } while (gInjector.takeNext(&injected));' '  } while (false);'
}
T() { mut $E $'  //    does after every callback into JavaScript.\n  uiRunTickCallback(env);' $'  //    does after every callback into JavaScript.'; }
K() { mut $E $'    if (uiClosing()) {\n      const char *name = jsEventName(event.type);' $'    if (false) {\n      const char *name = jsEventName(event.type);'; }
W() { mut tools/hermes-node-ui/ui_module.cpp '  hnui_window *w = new hnui_window();' $'  hnui_window *w = new hnui_window();\n  gWindow = w;'; }
```

- **M1, a `sokol_app` error in every window run.** In `onInit()`
  (`ui_platform.cpp`, shared by both backends), just before the
  `init_cb: shared context ready` trace. Prints
  `[sapp:error] item 0: mutation (sokol:0)`.
- **M2, a presenting-instance warning.** Same place. Prints
  `[sokol-present:warning] item 0: mutation (sokol:0)`. A warning, to show
  the rule refuses every level from a `sokol_gfx` instance.
- **M3, a producing-instance error.** Same place. Prints
  `[sokol-produce:error] item 0: mutation (sokol:0)`.
- **S, split every injected batch across frames 50 ms apart.**
  `drainInjectedEvents()` takes one event per call and sleeps before the
  next. The sleep is what makes it bite under Xvfb, where frames come as
  fast as the platform thread can run them: one event per frame without
  it left every test green (measured), because the second event still
  landed while the first one's dispatch pass was running. On a vsynced
  display the frames are 16 ms apart already.
- **T, no microtask checkpoint between two events.** Removes step 5 of
  `deliver()` (`ui_events.cpp`), the `uiRunTickCallback(env)` call.
- **K, deliver events while CLOSING.** Disables the `uiClosing()` check
  in `uiDispatchEvents()`.
- **W, publish the window before it is complete.** Stores `gWindow` right
  after `new hnui_window()` in `onSharedContextReady()`.

## Scope decisions

What the Task 17 bullet listed, and what became of each:

| Item | Decision |
|---|---|
| `display` feature that starts Xvfb | Done in Task 8 (Ruling 34) and Slice C's fix wave. Nothing left. |
| macOS gating that does not silently skip | Done in Task 16a. Nothing left. |
| Resize-generation, lifecycle, conformance assertions | Done in Tasks 11d, 14b, 15 and Slice C's fix wave (`ui-resize-generation.js`, `ui-real-events.js`, `ui-exit-paths.js`, `ui-close.js`, `ui-conformance.js`). Nothing left. |
| Stderr-check sweep of four tests | **17a**, widened: the survey found nine files, not four, and the rule gets a test that enforces it (added; not in the bullet). |
| `hnui_get_window()` NULL after a late rejection | **17b**, with a second provocation the bullet did not name (`process.uptime`), because it reaches the abandon path that has a reference to delete (added). |
| Two back-to-back `__injectEvent` calls taken by one drain | **17c**: the two assertions that depend on it are rewritten so they do not. No new hook. |
| `CompletionGate` has no JS surface | **Dropped, and the bullet is stale.** See "D" below. |
| Mac: new tests on Metal, Release suite, three final runs | **17d, 17e.** |
| Dock Quit, display sleep, the `display` canary with its promise broken | "Needs the user at the Mac", outside the automated steps. |
| Documents, tracker | **17f.** |

**D, `holdProducer`/`holdConsumer`.** The bullet says neither has a JS
surface. The producer half has had one since Task 11d:
`ui.__holdProducerCompletions()` and `ui.__releaseProducerCompletions()`
(`tools/hermes-node-ui/ui_test_hooks.cpp`), used by
`test/ui-resize-generation.js` INFLIGHT to keep a frame of a retired
generation in flight across a resize. Only `holdConsumer` has none. No
assertion Task 17 still owes needs it: the one property a held consumer
read would pin, that a generation's last reference is dropped off the owner
thread and its destruction posted, is already asserted without a hold
(`owner=0` then `owner=1` in `ui-resize-generation.js`), because the
consumer's read is naturally the last reference. The lifecycle assertions
the bullet meant exist since Task 15 and use the two platform barriers.
Left out; 17f corrects the bullet's sentence.

**Staying open in the tracker, not dropped** (17f Step 5 touches each):
Intel Macs (01a1fbb5-8505); the outcome of the CI display probe
(01a1fbb5-84ef); fullscreen on macOS (01a1fbb5-84af); the in-flight
maintenance guard (01a1fbb5-852f); no main menu, so Cmd-Q does nothing
(01a1fbb5-84c4); logout waiting on a cancelled close (01a1fbb5-84da); a
real Dock Quit (01a1fbb5-851a) and display sleep (01a1fbb5-8545) unless
the user's list below was done; the input queue after a late rejection
(01a1d5c0-d3b1); lit's missing per-test timeout (01a0d797-e326). **NVIDIA's
driver, native Xorg and Intel GL have no issue at all today** -- the spec
says NVIDIA is untested, and nothing in `dz/` does -- so 17f files one.

## Survey: which window tests exclude Sokol error lines

**The loggers.** All three go through `uiSokolLog`
(`lib/compositor-gpu/sokol_log.cpp`), which prints
`[<instance>:<level>] item <id>: <message> (<file>:<line>)`. Instances:
`sapp` (`sokolLog`, `ui_platform.cpp`), `sokol-produce` (`producerLog`,
`gl_backend.cpp` / `metal_backend.mm`), `sokol-present` (`presentLog`,
`gl_present.cpp` / `metal_present.mm`). Levels: `panic`, `error`,
`warning`, `info`. A panic prints a second line and `_exit(1)`s.

**The rule already in use**, in 21 window RUN lines of 16 files:
`--implicit-check-not='[sapp:error' --implicit-check-not='[sokol-'` on the
FileCheck run that reads the binary's stderr. It refuses a `sokol_app`
error and anything at all from either `sokol_gfx` instance. It does not
refuse `[sapp:warning`, and cannot: every healthy GL run prints
`LINUX_X11_QUERY_SYSTEM_DPI_FAILED` at that level. It does not name
`[sapp:panic` because a `sokol_app` panic happens inside `sapp_run()`
before `init_cb`, so no window test can reach its first positive line.

**What a healthy run prints.** GL (measured): the one `[sapp:warning]`
line, nothing else. Metal (measured by 17d Step 1 on 2026-11-21, on the
one Apple-silicon Mac; this sentence was `UNVERIFIED:` until then): no
`[sapp:` line at any level and no `[sokol-present` line, so Metal has no
counterpart to GL's DPI warning. The only `[sokol-` lines in a run of all
41 UI files were the five that `ui-sokol-panic.js` and
`ui-sokol-validation.js` provoke on purpose. The census is quoted in
"Task 17d as built".

A window RUN line is a `// RUN:` line containing `%with-display`. 44 of
them in 25 files:

| File | Window runs | Before | After 17a |
|---|---|---|---|
| `ui-abi-sizes`, `ui-clipboard`, `ui-clipboard-mac`, `ui-commands`, `ui-conformance`, `ui-early-events`, `ui-events`, `ui-metal-clear`, `ui-metal-occlusion`, `ui-real-events`, `ui-resize-generation`, `ui-wrong-thread` | 1 each | both | unchanged |
| `ui-demo-visual`, `ui-raf` | 2 each | both | unchanged |
| `ui-event-exceptions`, `ui-present` | 3 each | both | unchanged |
| `ui-close` | 8 | neither | both |
| `ui-exit-paths` | 6 | neither | both |
| `ui-terminate` (Metal only) | 3 | neither | both |
| `ui-metal-maintenance`, `ui-metal-maintenance-wake` (Metal only) | 1 each | `[sokol-` only | both |
| `ui-create-window` | 1 | stderr not read | both; tracing turned on for a positive line |
| `ui-real-events-demo` (GL only) | 1 | stderr not read | both; tracing turned on |
| `ui-sokol-panic` | 1 | none | **exception:** `[sapp:error`, `[sokol-present`, `[sokol-produce:error` |
| `ui-sokol-validation` | 1 | none | **exception:** `[sapp:error`, `[sokol-present` |

The two exceptions make the **producing** instance panic on purpose.
`ui-sokol-panic.js` calls that instance's logger with a panic item, so a
`[sokol-produce:panic]` line is its subject and no `[sokol-produce:error]`
precedes it. `ui-sokol-validation.js` provokes a real validation failure,
whose `[sokol-produce:error]` lines are its subject too. Neither has any
business hearing from `sokol_app` or the presenting instance.

Not window tests, so outside the rule: `ui-exit-tty`, `ui-route-refusal`,
`ui-null-window`, `ui-version-check`, `ui-exit-late-require`,
`ui-windowless`, `ui-bundle-argv`, `ui-module-requirable` (no window is
requested, or the request is refused before `sapp_run()`, so no Sokol
logger exists), and the feature canaries (`ui-backend-feature`,
`ui-binary-exists`, `ui-display-feature`, `ui-display-busy`,
`ui-sokol-validation-feature`, `ui-sokol-validation-ci`).

## File map

| File | Task | What |
|---|---|---|
| `utils/check-ui-stderr-rule.py` | 17a, create | The rule as a program: every `%with-display` RUN line's stderr is read with the exclusions. |
| `test/ui-stderr-rule.js` | 17a, create | Runs the checker over `test/`. Ungated. |
| `test/ui-close.js`, `ui-exit-paths.js`, `ui-terminate.js`, `ui-metal-maintenance.js`, `ui-metal-maintenance-wake.js`, `ui-create-window.js`, `ui-real-events-demo.js`, `ui-sokol-panic.js`, `ui-sokol-validation.js` | 17a, modify | RUN lines. |
| `test/ui-late-rejection.js` | 17b, create | `hnui_get_window()` after a late rejection, two causes. |
| `test/ui-events.js`, `test/ui-close.js` | 17c, modify | TICKS and REPLACED. |
| `test/ui-event-exceptions.js`, `tools/hermes-node-ui/ui_platform.cpp` | 17c, modify | Comments only. |
| This file | 17d, 17e, 17f | "As built" sections. |
| `docs/superpowers/plans/2026-09-21-ui-compositor.md`, `progress-ui-compositor.md`, `CLAUDE.md`, `dz/issues/*.md` | 17f | Closing the slice. |

---

## Task 17a: one stderr rule for every window test

**Who:** Linux. (Three of the files it edits run only on the Mac; 17d
runs them.)

**Files:**
- Create: `utils/check-ui-stderr-rule.py`, `test/ui-stderr-rule.js`.
- Modify: the nine test files in the survey's lower rows.

**Steps:**

- [ ] **Step 1: The checker.** Create `utils/check-ui-stderr-rule.py`
  (executable, the repository's Python header and
  `Copyright (c) Tzvetan Mikov.`):

  ```python
  """Check that every window test excludes Sokol's error lines.

    check-ui-stderr-rule.py <test dir>

  A window test is a test/ui-*.js RUN line that runs under %with-display.
  Each must send the binary's stderr to a file of its own (`2> %t...`), and
  some %FileCheck RUN line of the same test must read that file with

    --implicit-check-not='[sapp:error' --implicit-check-not='[sokol-'

  so that a Sokol error printed by a run that otherwise behaves is a
  failure. The exceptions provoke a Sokol message on purpose and exclude
  everything but the instance they provoke; see NARROWED.

  Prints one line per violation and exits 1 if there was any.
  """

  import glob
  import os
  import re
  import sys

  SAPP = "--implicit-check-not='[sapp:error'"
  ALL_GFX = "--implicit-check-not='[sokol-'"
  # These two make the PRODUCING sokol_gfx instance panic, so its lines are
  # what they assert; the presenting instance must still be silent.
  NARROWED = {
      'ui-sokol-panic.js': "--implicit-check-not='[sokol-present'",
      'ui-sokol-validation.js': "--implicit-check-not='[sokol-present'",
  }

  STDERR_FILE = re.compile(r'2> ?(%t[\w.]*)')


  def check(path):
      name = os.path.basename(path)
      gfx = NARROWED.get(name, ALL_GFX)
      with open(path, encoding='utf-8') as f:
          runs = [(n, line) for n, line in enumerate(f, 1)
                  if line.startswith('// RUN:')]
      problems = []
      for n, line in runs:
          if '%with-display' not in line:
              continue
          captured = STDERR_FILE.findall(line)
          if not captured:
              problems.append((n, 'runs under %with-display without '
                                  'sending stderr to a file (2> %t...)'))
              continue
          for err in captured:
              reader = re.compile(r'< ?' + re.escape(err) + r'(?![\w.])')
              if not any('%FileCheck' in other and reader.search(other) and
                         SAPP in other and gfx in other
                         for _, other in runs):
                  problems.append(
                      (n, 'no %FileCheck run reads ' + err + ' with ' +
                          SAPP + ' ' + gfx))
      return [(path, n, what) for n, what in problems]


  def main():
      if len(sys.argv) != 2:
          sys.stderr.write(__doc__)
          return 2
      problems = []
      files = sorted(glob.glob(os.path.join(sys.argv[1], 'ui-*.js')))
      for path in files:
          problems.extend(check(path))
      for path, n, what in problems:
          print('%s:%d: %s' % (path, n, what))
      print('ui stderr rule: %d file(s), %d violation(s)' %
            (len(files), len(problems)))
      return 1 if problems else 0


  if __name__ == '__main__':
      sys.exit(main())
  ```

- [ ] **Step 2: Run it and watch it fail.**

  ```bash
  python3 utils/check-ui-stderr-rule.py test | tail -1; echo "rc=$?"
  # expect: ui stderr rule: 39 file(s), 23 violation(s)
  ```

  The 23 are 8 in `ui-close.js`, 6 in `ui-exit-paths.js`, 3 in
  `ui-terminate.js`, and one each in the other six files (measured).

- [ ] **Step 3: The lit test.** Create `test/ui-stderr-rule.js`. No
  `REQUIRES`: it reads text and needs no binary, so it also runs in a
  build without the UI, where the rule would otherwise go unwatched.

  ```js
  // Copyright (c) Tzvetan Mikov.
  //
  // This source code is licensed under the MIT license found in the
  // LICENSE file in the root directory of this source tree.

  // **The forcing function for the window tests' stderr rule.** Every RUN
  // line that opens a window (`%with-display`) must have its stderr read by
  // a FileCheck run carrying
  //
  //   --implicit-check-not='[sapp:error' --implicit-check-not='[sokol-'
  //
  // so that a Sokol error in a run that otherwise behaves is a failure.
  // utils/check-ui-stderr-rule.py is the rule; its docstring gives the two
  // exceptions and why. Without this test the rule holds until the next
  // window test is written from a file that predates it: Task 17 found it
  // missing from 23 RUN lines in nine files.
  //
  // `[sapp:warning` is not excluded: a healthy GL run prints one,
  // LINUX_X11_QUERY_SYSTEM_DPI_FAILED.
  //
  // Mutation: delete the two exclusions from one RUN line of
  // test/ui-events.js; this test fails naming that file and line.
  //
  // RUN: %python %source_dir/utils/check-ui-stderr-rule.py %source_dir/test | %FileCheck %s

  // CHECK-NOT: {{^.*}}.js:{{[0-9]+}}:
  // CHECK: {{^}}ui stderr rule: {{[1-9][0-9]*}} file(s), 0 violation(s){{$}}
  ```

  (The `CHECK-NOT` precedes the only positive line, which is the last
  line of output, so it sees everything; the count line alone would do,
  and the `-NOT` is there to put the offending line in FileCheck's error.)

- [ ] **Step 4: The mechanical edits.** In `test/ui-close.js` (8 lines),
  `test/ui-exit-paths.js` (6) and `test/ui-terminate.js` (3), on every
  line that begins `// RUN: %FileCheck --check-prefix=<X>-TRACE `, insert
  `--implicit-check-not='[sapp:error' --implicit-check-not='[sokol-' `
  directly after that `--check-prefix` argument. In
  `test/ui-metal-maintenance.js` and `test/ui-metal-maintenance-wake.js`,
  change `--check-prefix=TRACE --implicit-check-not='[sokol-'` to
  `--check-prefix=TRACE --implicit-check-not='[sapp:error' --implicit-check-not='[sokol-'`.
  Add one sentence to each file's RUN-line comment saying the pair is the
  suite-wide rule (`test/ui-stderr-rule.js`).

- [ ] **Step 5: The four tests the bullet named.**

  `test/ui-sokol-panic.js`, the LOGGERERR run becomes:

  ```
  // RUN: %FileCheck --check-prefix=LOGGERERR --implicit-check-not='[sapp:error' --implicit-check-not='[sokol-present' --implicit-check-not='[sokol-produce:error' %s < %t.logger.err
  ```

  `test/ui-sokol-validation.js`, the ERR run becomes:

  ```
  // RUN: %FileCheck --check-prefix=ERR --implicit-check-not='[sapp:error' --implicit-check-not='[sokol-present' %s < %t.err
  ```

  Each gets a comment: it provokes the producing instance, so that
  instance's lines are asserted and everything else is still refused.

  `test/ui-create-window.js` reads no stderr today and runs without
  tracing, so there is no positive line to hang an exclusion on. Its two
  RUN lines become three, in the `sh -c` shape every later window test
  uses:

  ```
  // RUN: %with-display 120 sh -c 'HERMES_NODE_DEBUG_NATIVE=UI %hermes-node-ui %s > %t.out 2> %t.err'; echo "EXITCODE $?" | %FileCheck --check-prefix=CODE %s
  // RUN: %FileCheck --check-prefix=OUT %s < %t.out
  // RUN: %FileCheck --check-prefix=ERR --implicit-check-not='[sapp:error' --implicit-check-not='[sokol-' %s < %t.err
  ```

  and one check line, `// ERR: window ready`. Rewrite the header
  paragraph that begins "stderr goes to its own file" to say what the
  file is now read for.

  `test/ui-real-events-demo.js`: add `HERMES_NODE_DEBUG_NATIVE=UI ` before
  `%hermes-node-ui` inside the `sh -c` (the test hooks stay off, which is
  the point of that test), and a third RUN line:

  ```
  // RUN: %FileCheck --check-prefix=DEMOERR --implicit-check-not='[sapp:error' --implicit-check-not='[sokol-' %s < %t.demo.err
  ```

  with `// DEMOERR: quit requested: cancelled, forwarded as close` and
  `// DEMOERR: close: not cancelled`. (Both lines were read from a traced
  run of that exact command on Linux. `window ready` is deliberately not
  one of them: the close request can arrive before it.)

- [ ] **Step 6: Run.**

  ```bash
  python3 utils/check-ui-stderr-rule.py test | tail -1
  # expect: ui stderr rule: 40 file(s), 0 violation(s)
  utils/lit-run.sh $B $S/test/ui-*.js 2>&1 | tail -4
  # expect: 31 passed, 9 unsupported, 0 failed (40 files)
  ```

  *Mutation of the checker's test:* delete the pair from
  `test/ui-events.js` line 69; `utils/lit-run.sh $B -v $S/test/ui-stderr-rule.js`
  is red and its output names `ui-events.js:67`, the `%with-display` line
  whose stderr is no longer covered. Restore.

- [ ] **Step 7: The mutations that prove each exclusion can fire.** For
  each of M1, M2, M3: apply, build both binaries, run every UI test,
  restore.

  ```bash
  utils/lit-run.sh $B $S/test/ui-*.js 2>&1 | grep -c '^FAIL'
  # expect: M1 19, M2 19, M3 18
  ```

  The 19 are exactly the survey's files that run on Linux: `ui-abi-sizes`,
  `ui-clipboard`, `ui-close`, `ui-commands`, `ui-conformance`,
  `ui-create-window`, `ui-demo-visual`, `ui-early-events`,
  `ui-event-exceptions`, `ui-events`, `ui-exit-paths`, `ui-present`,
  `ui-raf`, `ui-real-events`, `ui-real-events-demo`,
  `ui-resize-generation`, `ui-sokol-panic`, `ui-sokol-validation`,
  `ui-wrong-thread`. Under M3 `ui-sokol-validation` is the one that stays
  green, because a `[sokol-produce:error]` line is what it expects. Before
  this task M1 and M2 each turned 13 red (measured); these numbers were
  measured against a prototype of Steps 4 and 5.

- [ ] **Step 8: Format, run `check-hermes-node`, commit.**

  ```bash
  ./utils/format.sh -a
  cmake --build cmake-build-asan --target check-hermes-node 2>&1 | tail -15
  # expect: 456 unit tests; JS 233 passed, 10 unsupported, 0 failed (243)
  git add utils/check-ui-stderr-rule.py test/ui-stderr-rule.js test/ui-*.js
  git commit   # "Exclude Sokol error lines in every window test"
  ```

---

## Task 17b: `hnui_get_window()` after a late rejection

**Who:** Linux. 17d runs it on Metal.

**How a late rejection arises.** `createWindow` is settled on the JS
thread by `onSharedContextReady()` (`tools/hermes-node-ui/ui_module.cpp`),
posted from `init_cb` once Sokol's window and the shared context exist. It
builds the `hnui_window` in a local, `w`, and three steps can then fail,
each through the `abandon` lambda, which deletes `w` and calls
`rejectWith()`: `createGenerationSurfaces`, `uiCreateWindowObject`
(`ui_events.cpp`: it calls `globalThis.require('events')`) and
`computeFrameOrigin` (it calls `process.uptime()`). `gWindow = w` comes
after all three. `hnui_get_window(env)` is `uiWindowForEnv(env)`, and the
reference addon exposes it as `addon.hasWindow()`
(`test/fixtures/hnui-demo/demo_addon.c`), which `test/ui-null-window.js`
already uses for a runtime that never asked for a window.

**The experiment** (Linux, 2026-11-21, scratch script, deleted). With
`globalThis.require = undefined` assigned between the `createWindow()`
call and its `await`: the Promise rejects with `could not create the window
object: globalThis.require is not a function, so EventEmitter could not be
loaded`; `addon.hasWindow()` is false before and after; no
`requestAnimationFrame` global was installed; a second `createWindow()`
throws "a window already exists in this process"; the trace shows
`init_cb: shared context ready`, `generation 1: 3 surfaces at 200x150`,
`lifecycle: FAILED`, `lifecycle: event-loop handle released (createWindow
rejected)`, and the script returning ends through `lifecycle: runtime
returned, exiting with status 0`. With `process.uptime = undefined`
instead: `could not read process.uptime(), which frame timestamps are
measured against`, and the same on every other count. The provocation
cannot race: `onSharedContextReady` is a function posted to the thread
that is running the script.

**Files:**
- Create: `test/ui-late-rejection.js`.

**Steps:**

- [ ] **Step 1: Write the test**, exactly:

  ```js
  // Copyright (c) Tzvetan Mikov.
  //
  // This source code is licensed under the MIT license found in the
  // LICENSE file in the root directory of this source tree.

  // A createWindow() rejected LATE leaves no window behind: hnui_get_window()
  // answers NULL afterwards, as hnui.h says it does for a runtime that
  // "called it and was rejected".
  //
  // Late means in onSharedContextReady() (ui_module.cpp), on the JS thread,
  // after Sokol's window, the shared context and generation 1's surfaces all
  // exist. Three steps can fail there, and a script can provoke two of them
  // by taking something away between the createWindow() call and its
  // `await`:
  //
  //  REQUIRE  the window object is built by calling
  //           `globalThis.require('events')`; with the global cleared that
  //           step fails before the object exists.
  //  ORIGIN   the frame origin is read by calling `process.uptime()`; with
  //           the property cleared that step fails after the window object
  //           exists and is held, so this is the abandon path that has a
  //           reference to delete.
  //
  // (The third, surface allocation, is the backend's and cannot be provoked
  // from a script.) The hnui_window is built in a local and published to
  // gWindow only after every step succeeded, which is the line this pins.
  //
  // Also asserted, because they are the rest of "no window": the frame
  // globals were not installed, a second createWindow() still throws (one
  // window per process counts the request, not the outcome), and a script
  // that handles the rejection and returns ends with status 0 (Ruling 72:
  // the referenced handle is released).
  //
  // Mutation: in onSharedContextReady(), move `gWindow = w;` up to just
  // after `new hnui_window()`. Under ASAN the worker's per-turn check then
  // reads the deleted window (heap-use-after-free, status 1, nothing after
  // BEFORE); without ASAN the line reads `AFTER true`.
  //
  // REQUIRES: ui, ui-gpu
  // REQUIRES: display
  //
  // RUN: %with-display 120 sh -c 'HERMES_NODE_DEBUG_NATIVE=UI %hermes-node-ui %s %hnui_demo_addon require > %t.require.out 2> %t.require.err'; echo "EXITCODE $?" | %FileCheck --check-prefix=CODE %s
  // RUN: %FileCheck --check-prefixes=CHECK,REQUIRE %s < %t.require.out
  // RUN: %FileCheck --check-prefix=TRACE --implicit-check-not='[sapp:error' --implicit-check-not='[sokol-' --implicit-check-not='window ready' --implicit-check-not=AddressSanitizer %s < %t.require.err
  //
  // RUN: %with-display 120 sh -c 'HERMES_NODE_DEBUG_NATIVE=UI %hermes-node-ui %s %hnui_demo_addon origin > %t.origin.out 2> %t.origin.err'; echo "EXITCODE $?" | %FileCheck --check-prefix=CODE %s
  // RUN: %FileCheck --check-prefixes=CHECK,ORIGIN %s < %t.origin.out
  // RUN: %FileCheck --check-prefix=TRACE --implicit-check-not='[sapp:error' --implicit-check-not='[sokol-' --implicit-check-not='window ready' --implicit-check-not=AddressSanitizer %s < %t.origin.err

  // CHECK: BEFORE false
  // REQUIRE-NEXT: REJECTED could not create the window object: globalThis.require is not a function, so EventEmitter could not be loaded
  // ORIGIN-NEXT: REJECTED could not read process.uptime(), which frame timestamps are measured against
  // CHECK-NEXT: AFTER false
  // CHECK-NEXT: RAF undefined
  // CHECK-NEXT: SECOND threw
  // CODE: EXITCODE 0

  // The rejection really was the late one: the platform thread had finished
  // init_cb, so Sokol's window existed.
  // TRACE: init_cb: shared context ready
  // TRACE: lifecycle: FAILED
  // TRACE: lifecycle: event-loop handle released (createWindow rejected)
  // TRACE: lifecycle: runtime returned, exiting with status 0

  'use strict';

  const ui = require('hermes-node:ui');
  const addon = require(process.argv[2]);
  const mode = process.argv[3];

  async function main() {
    console.log('BEFORE ' + addon.hasWindow());
    const savedRequire = globalThis.require;
    const savedUptime = process.uptime;
    const pending = ui.createWindow(
        {width: 200, height: 150, title: 'hermes-node-ui late rejection'});
    // Between the call and its await: onSharedContextReady() runs on a later
    // loop turn and finds no function here.
    if (mode === 'require')
      globalThis.require = undefined;
    else
      process.uptime = undefined;
    let outcome;
    try {
      await pending;
      outcome = 'RESOLVED';
    } catch (e) {
      outcome = 'REJECTED ' + e.message;
    }
    globalThis.require = savedRequire;
    process.uptime = savedUptime;
    console.log(outcome);
    console.log('AFTER ' + addon.hasWindow());
    console.log('RAF ' + typeof globalThis.requestAnimationFrame);
    let second = 'did-not-throw';
    try {
      await ui.createWindow({width: 100, height: 100});
    } catch (e) {
      second = 'threw';
    }
    console.log('SECOND ' + second);
    // No process.exit(): returning is the subject of the last TRACE line.
  }

  main().catch((e) => {
    console.log('FAILED ' + (e && e.stack));
    process.exit(1);
  });
  ```

  The four TRACE lines are all printed by shared code (`ui_platform.cpp`,
  `ui_module.cpp`), so they need no per-backend prefix.

- [ ] **Step 2: Run it.**

  ```bash
  for i in 1 2 3 4 5 6; do utils/lit-run.sh $B -v $S/test/ui-late-rejection.js 2>&1 | grep '^PASS\|^FAIL'; done
  # expect: PASS six times (measured, this file, 6 of 6)
  python3 utils/check-ui-stderr-rule.py test | tail -1
  # expect: ui stderr rule: 41 file(s), 0 violation(s)
  ```

  This test passes as soon as it is written: it pins behaviour that
  exists. The red run is Step 3.

- [ ] **Step 3: Mutation W.** Apply, build both binaries, run the test.
  Expected: FAIL; `%t.require.out` holds `BEFORE false` and nothing else,
  the status is 1, and the stderr file has
  `ERROR: AddressSanitizer: heap-use-after-free` in `onWorkerLoopTurn`
  (measured). Restore and rebuild. Then M1: the test is red on its
  `[sapp:error` exclusion. Restore and rebuild.

- [ ] **Step 4: Format, run `check-hermes-node`, commit.**

  ```bash
  ./utils/format.sh -a
  cmake --build cmake-build-asan --target check-hermes-node 2>&1 | tail -15
  # expect: 456 unit tests; JS 234 passed, 10 unsupported, 0 failed (244)
  git add test/ui-late-rejection.js
  git commit   # "Test hnui_get_window after a late rejection"
  ```

---

## Task 17c: two events in one dispatch pass, without luck

**Who:** Linux. 17d runs it on Metal.

**What depends on the assumption, exactly.** `ui.__injectEvent` pushes one
event onto `gInjector`; the platform thread empties that queue once per
frame in `drainInjectedEvents()` (`ui_platform.cpp`), holding
`gDispatchPosted` so the batch gets one posted dispatch. Two calls made
back to back are taken by one drain only if no frame's drain runs between
them. Three places lean on that:

| Where | What it needs | If the batch splits |
|---|---|---|
| `test/ui-close.js` REPLACED: `QUIT_REQUESTED` then `KEY_DOWN` | The key behind the close in the **same** pass, so the pass returns into it after CLOSING and traces `event keydown: not delivered (closing)` | **Red**: the key arrives after the dispatcher closed, and the trace reads `event dispatch: refused (closing)` instead. Measured with S: 3 of 3. |
| `test/ui-events.js` TICKS: `KEY_DOWN` then `CHAR` in phase one | The char behind the key in the same pass, so only `deliver()`'s `process._tickCallback()` can put the promise callback first | **Green, and vacuous**: between two passes the loop runs the callback anyway. Measured with S+T: 6 of 6 green with the call it pins removed. |
| `test/ui-event-exceptions.js` FATAL: `F1` then `F2`, comment "One batch, so both are queued before the first is delivered" | Nothing | Unaffected. If the exception were swallowed, the process would live and `F2` would be delivered by a later pass, print `AFTER` and exit 0 -- red either way. Only the comment is wrong. |

The rest of `ui-events.js` does not depend on it (ORDER is the injection
order whatever the passes; REMOVAL and TRACKED are per event; MINIMIZED
was already built around a `setImmediate` for this reason), nor does
`ui-clipboard.js` / `ui-clipboard-mac.js`'s paste-then-marker.

**The choice: rewrite the two assertions; add no hook.** The alternative
was an atomic `ui.__injectEvents([...])`. It would guarantee one *drain*,
and what the tests need is one *dispatch pass*, which a drain does not
give: `drainInjectedEvents()` takes the batching flag only if no dispatch
is already posted, and when one is -- for a real event, which a real
display delivers whenever the pointer crosses the window -- that dispatch
can run on the JS thread while the platform thread is still converting the
batch, empty the queue between two pushes and return. Closing that would
mean an atomic push into `InputQueue` as well, product code changed for a
test's convenience. The existing barrier gives the stronger property with
no new code: inject the second event **from inside the first event's
listener** and then call `ui.__waitPlatformFrames(1)`, which blocks the JS
thread until the platform thread has finished a frame begun after the
call. That frame's drain converts the event and pushes it onto `gInput`
while the pass is still on the stack, and `EventDispatcher::dispatch()`
drains "pass after pass until a drain comes back empty", so the same call
delivers it. `test/ui-exit-paths.js` already uses exactly this shape. It
holds whatever the frame timing, which mutation S demonstrates.

**Files:**
- Modify: `test/ui-close.js`, `test/ui-events.js`,
  `test/ui-event-exceptions.js` (comment),
  `tools/hermes-node-ui/ui_platform.cpp` (comment).

**Steps:**

- [ ] **Step 1: Show the hole first.** Apply S, build both binaries:

  ```bash
  for i in 1 2 3; do utils/lit-run.sh $B $S/test/ui-*.js 2>&1 | grep '^FAIL'; done
  # expect: FAIL: hermes-node :: ui-close.js, three times, and no other
  ```

  Add T on top of S, rebuild:

  ```bash
  for i in 1 2 3; do utils/lit-run.sh $B $S/test/ui-events.js 2>&1 | grep '^PASS\|^FAIL'; done
  # expect: PASS three times -- TICKS is green with the call it pins removed
  ```

  Restore (`git checkout tools`) and rebuild.

- [ ] **Step 2: `test/ui-close.js`, REPLACED.** Replace the body of
  `if (mode === 'replaced') { ... }`:

  ```js
      if (mode === 'replaced') {
        win.on('close', () => {
          console.log('CLOSE-EVENT');
          // Injected from inside the pass that is delivering the close, and
          // the barrier holds this listener until the platform thread has
          // converted and queued it: the KEY_DOWN is then behind the close
          // in the SAME dispatch pass, whatever the frame timing.
          ui.__injectEvent({type: 'KEY_DOWN', keyCode: SAPP_KEYCODE_A});
          console.log('KEY-QUEUED ' + ui.__waitPlatformFrames(1));
        });
        win.on('keydown', () => console.log('KEY-AFTER-CLOSE'));
        ui.__injectEvent({type: 'QUIT_REQUESTED'});
        return;
      }
  ```

  Add `// REPLACED-NEXT: KEY-QUEUED true` between `// REPLACED: CLOSE-EVENT`
  and `// REPLACED-NEXT: REPLACED-EXIT args=0`. The REPLACED-TRACE lines do
  not change. In the header, replace the paragraph that begins "Nothing
  enforces 'the same dispatch pass'" (through "expects `event keydown: not
  delivered (closing)`.") with: the KEY_DOWN is injected from the `close`
  listener and `__waitPlatformFrames(1)` holds that listener until it is
  queued, so it is behind the close in the same pass by construction; and
  the first sentence of REPLACED says "delivered in the same dispatch pass
  as a KEY_DOWN injected from its own listener".

- [ ] **Step 3: `test/ui-events.js`, TICKS.** Four edits.

  Add `const SAPP_KEYCODE_F7 = 296;` after `SAPP_KEYCODE_F6`.

  Delete phase one's promise: the `win.on('keydown', (e) => { if
  (e.keyCode === SAPP_KEYCODE_A) { Promise.resolve().then(...) } })`
  listener, and in `checkOrder()` the four-line `if
  (line.startsWith('promise:'))` skip, whose only producer that was. Update
  `checkOrder`'s doc comment to match.

  Replace the three statements that compute and report TICKS from phase
  one's `log` with a burst of its own:

  ```js
    // TICKS, in a burst of its own. The CHAR is injected from inside the
    // keydown listener, and the barrier holds that listener until the
    // platform thread has converted and queued it -- so the CHAR is behind
    // the KEY_DOWN in the SAME dispatch pass, whatever the frame timing, and
    // only the checkpoint between two events of one pass can put the promise
    // callback ahead of it. (Two events injected back to back are usually
    // taken by one drain, but nothing makes them: split across two passes,
    // the promise callback runs between the passes whether or not deliver()
    // runs a checkpoint, and this line would pass with the call removed.)
    let ticksBarrier = null;
    await new Promise((resolve) => {
      win.on('keydown', (e) => {
        if (e.keyCode !== SAPP_KEYCODE_F7)
          return;
        Promise.resolve().then(() => addon.logLine('promise:keydown'));
        ui.__injectEvent({type: 'CHAR', charCode: 0x62});
        ticksBarrier = ui.__waitPlatformFrames(1);
      });
      win.once('char', () => resolve());
      ui.__injectEvent({type: 'KEY_DOWN', keyCode: SAPP_KEYCODE_F7});
    });
    const ticksLog = addon.takeEventLog();
    const promiseAt = ticksLog.indexOf('promise:keydown');
    const charAt = ticksLog.findIndex((line) => line.startsWith('native:char'));
    report(
        'TICKS',
        ticksBarrier === true && promiseAt !== -1 && charAt !== -1 &&
            promiseAt < charAt,
        'barrier ' + ticksBarrier + ', promise at ' + promiseAt + ', char at ' +
            charAt + ' log=' + ticksLog.join('|'));
  ```

  It stays where TICKS is reported today, between MIRROR and CLOSE, so the
  `CHECK:` lines keep their order. In the header, TICKS becomes: "a promise
  callback queued by a keydown listener runs before a CHAR that the same
  listener queued behind it, in one dispatch pass".

- [ ] **Step 4: The two comments.** `test/ui-event-exceptions.js`: replace
  "One batch, so both are queued before the first is delivered." with
  "Injected together. Nothing below needs them in one dispatch pass: if
  the exception were swallowed the process would live, and the second key
  would be delivered by this pass or a later one." In `ui_platform.cpp`,
  `drainInjectedEvents()`'s comment, replace the clause "-- which is what
  lets test/ui-events.js tell 'promise callbacks ran between two events'
  apart from 'they ran after the dispatch'" with "(a convenience, not a
  guarantee a test may rely on: a test that needs two events in one pass
  injects the second from the first one's listener and waits on
  `__waitPlatformFrames`)".

- [ ] **Step 5: Run, clean build.**

  ```bash
  cmake --build $B --target hermes-node hermes-node-ui 2>&1 | tail -1
  for i in $(seq 1 32); do utils/lit-run.sh $B $S/test/ui-events.js $S/test/ui-close.js $S/test/ui-event-exceptions.js 2>&1 | grep -c '^FAIL'; done | sort | uniq -c
  # expect: "32 0" (measured against a prototype of Steps 2 and 3)
  ```

- [ ] **Step 6: The mutations, in the five combinations that matter.**
  Build both binaries each time; restore between.

  | Applied | `ui-events.js` | `ui-close.js` | Measured |
  |---|---|---|---|
  | S | green | green | every UI test green, 3 of 3 (30 of 30 then; 32 once 17a and 17b are in) |
  | T | **red**: `TICKS WRONG barrier true, promise at 4, char at 2` | green | 3 of 3 |
  | S + T | **red** (was green in Step 1) | green | 3 of 3 |
  | K | green | **red**: `KEY-AFTER-CLOSE` after `REPLACED-EXIT args=0` | 3 of 3 |
  | S + K | green | **red** | 3 of 3 |

- [ ] **Step 7: Format, run `check-hermes-node`, commit.**

  ```bash
  ./utils/format.sh -a
  cmake --build cmake-build-asan --target check-hermes-node 2>&1 | tail -15
  # expect: 456 unit tests; JS 234 passed, 10 unsupported, 0 failed (244)
  git add test/ui-close.js test/ui-events.js test/ui-event-exceptions.js tools/hermes-node-ui/ui_platform.cpp
  git commit   # "Stop relying on one drain for injected events"
  ```

---

## Task 17d: the new and changed tests on Metal

**Who:** Mac runs the block; **Linux commits** the record (and any
Metal-only check line the Mac proposes).

Starts from "Stop relying on one drain for injected events". The request
carries the code block of "Mutations used by this plan" verbatim; the
block below calls its functions by name (`M1`, `S`, ...).

**Mac verification:**

```bash
cmake --build $B --target hermes-node hermes-node-ui hnui_demo_addon mac_pasteboard_tool 2>&1 | tail -1

# Step 1 -- the census. Before anything else, and stop if it surprises.
rm -f $B/test/Output/ui-*
HERMES_NODE_UI_EXPECT_DISPLAY=1 caffeinate -dimsu utils/lit-run.sh $B $S/test/ui-*.js 2>&1 | tail -8
# expect: 37 passed, 4 unsupported, 0 failed, of 41 files. (Derived: Task
#   16f's Release run was 34 + 5 of 39, and its fifth unsupported,
#   ui-sokol-validation, runs in this build; 17a and 17b add two files.)
#   Unsupported: ui-clipboard, ui-real-events-demo, ui-sokol-validation-ci,
#   ui-display-busy.
grep -h '^\[s[a-z-]*:' $B/test/Output/ui-*.err | sed -E 's/item [0-9]+/item N/; s/ \([^()]*\)$//' | sort | uniq -c
# expect, UNVERIFIED: only the deliberate lines of ui-sokol-panic and
#   ui-sokol-validation -- "[sokol-produce:panic] item N: raised deliberately
#   by hnui_demo_addon", three or so "[sokol-produce:error] item N:
#   VALIDATE_BEGINPASS_..." and "[sokol-produce:panic] item N:
#   VALIDATION_FAILED: ...". No [sapp: line and no [sokol-present line.
#   Report the whole output either way: it is the first record of what a
#   healthy Metal run prints.
# IF a test failed on an exclusion 17a added (ui-close, ui-exit-paths,
#   ui-terminate, ui-metal-maintenance, ui-metal-maintenance-wake,
#   ui-create-window): report the line, the file it is in and the RUN that
#   printed it, and stop. Do not weaken the exclusion.

# Step 2 -- the rule's checker runs here too (BSD userland, system python3).
python3 utils/check-ui-stderr-rule.py test | tail -1
# expect: ui stderr rule: 41 file(s), 0 violation(s)

# Step 3 -- the exclusions can fire. For each of M1, M2, M3: apply, then
cmake --build $B --target hermes-node hermes-node-ui 2>&1 | tail -1
HERMES_NODE_UI_EXPECT_DISPLAY=1 caffeinate -dimsu utils/lit-run.sh $B $S/test/ui-*.js 2>&1 | grep '^FAIL' | sort > /tmp/hnui-m.txt; wc -l < /tmp/hnui-m.txt
git checkout tools
# expect: M1 24, M2 24, M3 23. The 24: ui-abi-sizes, ui-clipboard-mac,
#   ui-close, ui-commands, ui-conformance, ui-create-window, ui-demo-visual,
#   ui-early-events, ui-event-exceptions, ui-events, ui-exit-paths,
#   ui-late-rejection, ui-metal-clear, ui-metal-maintenance,
#   ui-metal-maintenance-wake, ui-metal-occlusion, ui-present, ui-raf,
#   ui-real-events, ui-resize-generation, ui-sokol-panic,
#   ui-sokol-validation, ui-terminate, ui-wrong-thread. Under M3,
#   ui-sokol-validation is the one that stays green.
# IF instead every window test reports UNSUPPORTED under a mutation, the
#   `display` probe (utils/display_probe.py) is reading stderr; report that
#   and run the listed tests one by one instead.
# Attach the three lists.

# Step 4 -- the late rejection.
cmake --build $B --target hermes-node hermes-node-ui 2>&1 | tail -1
for i in 1 2 3 4 5 6; do HERMES_NODE_UI_EXPECT_DISPLAY=1 utils/lit-run.sh $B $S/test/ui-late-rejection.js 2>&1 | grep '^PASS\|^FAIL'; done
# expect: PASS six times
# then mutation W, rebuilt:
HERMES_NODE_UI_EXPECT_DISPLAY=1 utils/lit-run.sh $B -v $S/test/ui-late-rejection.js 2>&1 | tail -25
git checkout tools
# expect: FAIL, with an AddressSanitizer heap-use-after-free report (on Linux
#   it is in onWorkerLoopTurn). Report where it is on Metal.

# Step 5 -- one dispatch pass, on a vsynced display. Rebuild clean first.
cmake --build $B --target hermes-node hermes-node-ui 2>&1 | tail -1
for i in $(seq 1 32); do HERMES_NODE_UI_EXPECT_DISPLAY=1 utils/lit-run.sh $B $S/test/ui-events.js $S/test/ui-close.js $S/test/ui-event-exceptions.js 2>&1 | grep -c '^FAIL'; done | sort | uniq -c
# expect: "32 0"
# then, each rebuilt, three runs of ui-events.js and ui-close.js, restore between:
#   S      expect: both PASS
#   T      expect: ui-events FAIL (TICKS WRONG ...), ui-close PASS
#   S + T  expect: ui-events FAIL, ui-close PASS
#   K      expect: ui-events PASS, ui-close FAIL (KEY-AFTER-CLOSE)
#   S + K  expect: ui-events PASS, ui-close FAIL
# S is the one this platform decides: under vsync the frames are 16 ms apart
#   without it. Say whether S's 50 ms sleep made any difference to the result.

# Step 6 -- leave the tree as found.
git status --porcelain     # expect: empty
cmake --build $B --target hermes-node hermes-node-ui 2>&1 | tail -1
```

**Steps (Linux):**

- [x] **Step 1:** Send the block; wait for the report.
- [x] **Step 2:** If the census found a Sokol line a healthy Metal run
  prints, decide per line: a real defect is filed (`dz add --type bug`)
  and the exclusion stays, with the test red on the Mac until it is fixed;
  a line that is the platform's ordinary speech (as the DPI warning is
  GL's) can only be at `warning` or `info` level for `sapp`, which the
  rule does not refuse -- so a refused line is a defect or a
  `[sokol-` line, and either is reported to the user before anything is
  relaxed.
- [x] **Step 3:** Add "Task 17d as built" to this file: the census output
  verbatim, the three red counts with the lists, where W's report was,
  the loop's result and the five-row table as measured on Metal. Update
  the "Survey" section's `UNVERIFIED:` sentence with what was found.
- [x] **Step 4:** Format, run `check-hermes-node`, commit ("Record the Mac
  check of Task 17 on Metal").

---

## Task 17e: the whole suite on the Mac, Release once and ASAN three times

**Who:** Mac runs the block; **Linux commits** the record. May travel in
the same request as 17d when 17d's Step 1 raised nothing; **the three ASAN
runs must be the last thing run, on the last commit.**

**Mac verification:**

```bash
# Release, once. The directory exists from Task 16f; reconfigure nothing.
cmake --build $R 2>&1 | tail -3
# expect: no warning in lib/compositor-gpu or tools/hermes-node-ui
HERMES_NODE_UI_EXPECT_DISPLAY=1 caffeinate -dimsu cmake --build $R --target check-hermes-node 2>&1 | tail -15
# expect, PREDICTED not measured: unit tests all passed (456 in the ASAN
#   build; report the Release number); JS 238 passed, 6 unsupported,
#   0 failed, of 244.
HERMES_NODE_UI_EXPECT_DISPLAY=1 caffeinate -dimsu utils/lit-run.sh $R --show-unsupported $S/test 2>&1 | grep '^UNSUPPORTED'
# expect: bundle-yargs, ui-clipboard, ui-real-events-demo,
#   ui-sokol-validation-ci, ui-display-busy, and ui-sokol-validation (a
#   Release build compiles Sokol's validation layer out). Any other name is
#   reported with the feature it lacks; a test gated on a Debug or ASAN
#   build would be such a name, and none is known.
pbpaste | shasum     # before and after the run; expect the same digest

# ASAN, three times, last.
for i in 1 2 3; do
  HERMES_NODE_UI_EXPECT_DISPLAY=1 caffeinate -dimsu cmake --build $B --target check-hermes-node 2>&1 | tail -12
done
# expect, three times: 456 unit tests passed; JS 239 passed, 5 unsupported,
#   0 failed, of 244. (Derived: 237 + 5 of 242 after Task 16, plus
#   ui-stderr-rule and ui-late-rejection, both of which run here.)
HERMES_NODE_UI_EXPECT_DISPLAY=1 caffeinate -dimsu utils/lit-run.sh $B --show-unsupported $S/test 2>&1 | grep '^UNSUPPORTED'
# expect exactly: bundle-yargs, ui-clipboard, ui-real-events-demo,
#   ui-sokol-validation-ci, ui-display-busy
git status --porcelain     # expect: empty
```

A red run naming `test-inspect.js` or `test-repl-history.js` is one of the
two known flakes (CLAUDE.md, "Known flaky tests"): confirm with the
six-run loop given there and report it as such, without rerunning the
suite to hide it.

**Steps (Linux):**

- [x] **Step 1:** Send the block; wait for the report.
- [x] **Step 2:** Add "Task 17e as built" to this file with the Release
  totals and unsupported list, and the three ASAN totals.
- [x] **Step 3:** Tracker: `dz comment 01a1fbb5-8545` with the Release
  result (the whole suite, once, the totals). The issue stays open for
  display sleep unless the user's list was done; if it was, close it here.
  Stage `dz/`.
- [x] **Step 4:** Format, run `check-hermes-node`, commit ("Record the Mac
  suite runs for Task 17").

---

## Task 17f: close Slice D

**Who:** Linux.

**Files:**
- Modify: `docs/superpowers/plans/2026-09-21-ui-compositor.md`,
  `docs/superpowers/plans/progress-ui-compositor.md`, `CLAUDE.md`, this
  file, `dz/issues/*.md`.

**Steps:**

- [ ] **Step 1: Final Linux numbers.**

  ```bash
  cmake --build cmake-build-asan --target check-hermes-node 2>&1 | tail -15
  # expect: 456 unit tests; JS 234 passed, 10 unsupported, 0 failed, of 244
  utils/lit-run.sh cmake-build-asan --show-unsupported $S/test 2>&1 | grep '^UNSUPPORTED' | sort
  # expect the ten that were unsupported after Task 16: bundle-yargs,
  #   ui-clipboard-mac, ui-display-busy, ui-display-feature, ui-metal-clear,
  #   ui-metal-maintenance, ui-metal-maintenance-wake, ui-metal-occlusion,
  #   ui-sokol-validation-ci, ui-terminate
  ```

  (Before Task 17: 456 unit; JS 232 passed, 10 unsupported, 0 failed, of
  242. The nine UI names were measured while writing this plan;
  `bundle-yargs` is the tenth, from Task 16f's record.)

- [ ] **Step 2: The main plan.** In
  `docs/superpowers/plans/2026-09-21-ui-compositor.md`: tick Task 17 with
  its sub-tasks by commit subject, in the form Task 16's paragraph uses;
  the Slice D line under "Slices" becomes "Task 17 done (date)"; add a
  "Slice D complete" paragraph after the Task 15 one with the totals of
  both machines; in the Task 17 bullet, correct the `CompletionGate`
  sentence (the producer half has had `__holdProducerCompletions` since
  Task 11d; only `holdConsumer` has no JS surface, and nothing needed
  one); in "Known gaps", replace the Task 17 sentence with what stays
  open, by tracker id.
- [ ] **Step 3: The progress file.** Status row D becomes complete with
  its date; add "What Task 17 actually produced" (the rule and its test,
  the late-rejection test, the two rewritten assertions and why a hook was
  not added, the Metal census, the Release run); record the totals for
  both machines; in the obligations paragraph, mark Task 17's as
  discharged, naming the test for each.
- [ ] **Step 4: CLAUDE.md, UI Compositor section.** Three edits. The
  status sentence: Task 17 done, written out in this file. A new bullet
  after the FileCheck `-NOT` one: every `%with-display` RUN line's stderr
  is read with `--implicit-check-not='[sapp:error'
  --implicit-check-not='[sokol-'`, `test/ui-stderr-rule.js` enforces it
  through `utils/check-ui-stderr-rule.py`, the two exceptions are in that
  script, and `[sapp:warning` is not refused because a healthy GL run
  prints one. In the bullet "Assertions come from tracing, never from
  timing", one sentence: two `__injectEvent` calls made back to back are
  not guaranteed one dispatch pass; a test that needs that injects the
  second from the first one's listener and waits on
  `__waitPlatformFrames(1)`.
- [ ] **Step 5: The tracker** (`dz`, per CLAUDE.md "Which `dz` to run";
  stage `dz/` with this commit).
  - `dz add --type task "The GL backend is untested on NVIDIA's driver, native Xorg and Intel GL"`
    with a body saying what was measured (Mesa llvmpipe under Xvfb in the
    suite, and the AMD measurement the spec records for sync objects
    across the share group), that the spec calls NVIDIA "the most likely
    of any to" differ, and how to close it (run `check-hermes-node` with
    the UI on under each, with `HERMES_NODE_UI_EXPECT_DISPLAY=1` on a
    native X server, and record totals).
  - `dz comment 01a1d5c0-d3b1`: `test/ui-late-rejection.js` is now a
    reproducible way into the first of that issue's three states; the
    defect itself is untouched.
  - `dz comment` on each of 01a1fbb5-84af, 01a1fbb5-84ef, 01a1fbb5-8505,
    01a1fbb5-852f: "Slice D closed on (date) without this; still open",
    one line each, so the issue's log shows it was looked at and kept.
  - 01a1fbb5-851a and 01a1fbb5-8545: updated from the user's list below
    if it was done, otherwise one line saying it was not.
  - Close nothing that was not measured.
- [ ] **Step 6: This file.** Tick every step; make sure each task has its
  "as built" section; in "Open questions" strike what the Mac rounds
  answered.
- [ ] **Step 7: Format, run `check-hermes-node`, commit** ("Close Slice D
  of the UI compositor"). `git status --porcelain` is empty.

---

## Needs the user at the Mac

Not part of any automated step: each needs a person at the machine, or a
person's consent to something the agent must not do unasked (sleeping the
display, locking the screen). Each has a tracker issue that records the
result. `S`, `B` as in the Mac commands above.

1. **A real Dock Quit** (01a1fbb5-851a).

   ```bash
   HERMES_NODE_DEBUG_NATIVE=UI python3 utils/with-display.py 300 env -u HERMES_NODE_UI_TEST_NO_ACTIVATE \
     $B/bin/hermes-node-ui $S/test/fixtures/hnui-demo/demo.js $B/hnui_demo_addon.node js 2> /tmp/hnui-dock.err; echo "status $?"
   # at the Mac: right-click the program's Dock icon, choose Quit
   grep -n 'app: terminate requested\|quit requested\|close:\|lifecycle: CLOSING' /tmp/hnui-dock.err
   ```

   Expected if the request reaches `-[HnuiApplication terminate:]`: the
   window closes, `status 0`, and a line starting
   `app: terminate requested;`. If the window closes with no such line,
   AppKit's own path ran: record the status and the last twenty trace
   lines. Either way the result goes into the issue.

2. **Display sleep with a window open** (01a1fbb5-8545).

   ```bash
   HERMES_NODE_DEBUG_NATIVE=UI python3 utils/with-display.py 300 env -u HERMES_NODE_UI_TEST_NO_ACTIVATE \
     $B/bin/hermes-node-ui $S/test/fixtures/hnui-demo/demo.js $B/hnui_demo_addon.node js 2> /tmp/hnui-sleep.err &
   sleep 5; pmset displaysleepnow; sleep 15; caffeinate -u -t 2; sleep 5
   # at the Mac: unlock if asked, then close the window with its close button
   wait; echo "status $?"
   grep -n 'swapchain invalid\|drawable\|\[sapp:\|\[sokol-\|lifecycle' /tmp/hnui-sleep.err | head -40
   ```

   Nothing is predicted with confidence; this is a measurement. What to
   record: whether `present: swapchain invalid; nothing submitted` appears
   and whether it precedes the first frame without a drawable; whether
   frames resume after the wake; any `[sokol-` line; the status.

3. **The `display` canary with its promise broken.** Lock the screen
   (Control-Command-Q), then from an ssh session:

   ```bash
   HERMES_NODE_UI_EXPECT_DISPLAY=1 utils/lit-run.sh $B $S/test/ui-display-feature.js $S/test/ui-present.js 2>&1 | tail -6
   ```

   Expected: `ui-display-feature.js` FAILS and `ui-present.js` is
   UNSUPPORTED. No `caffeinate` here: it would wake the display and does
   not unlock. Unlock afterwards and rerun the same line: both pass. The
   result is recorded in "Task 17f as built" and in the main plan's Task
   17 paragraph, where the check was left undone.

## Open questions

Places where this plan guesses, stated so the guess can be overruled:

1. **What a healthy Metal run prints on stderr is unknown.** Assumed: no
   `[sapp:` and no `[sokol-present` line. 17d Step 1 settles it before
   anything depends on it.
2. **The Mac totals are arithmetic, not measurements**: 37 + 4 of 41 UI
   files, 239 + 5 of 244 for the suite, 24 / 24 / 23 red under M1-M3.
   Each starts from a number Task 16f measured; the UI-file one starts
   from its Release run, since the ASAN build's UI-only count was never
   reported.
3. **The Release suite's totals on the Mac are a prediction** (238 + 6),
   extended from one window-tests-only run. The unit count there has
   never been reported.
4. **`test/ui-stderr-rule.js` and its checker are an addition** the Task
   17 bullet did not ask for. They are here because a sweep with nothing
   enforcing it is the kind of thing this project has had to redo; the
   test is ungated, so it changes the JS total of every configuration by
   one. Drop Steps 1-3 of 17a (and subtract one from every JS total
   below it) if that is not wanted.
5. **The ORIGIN case of 17b is an addition** to the bullet's "ten-line
   test". It costs one more window run.
6. **`[sapp:panic` and `[sapp:warning` are left out of the rule** on the
   reasoning in "Survey". A stricter rule that refuses every `[sapp:`
   line but the one known GL warning is possible and was not chosen.
7. **Whether mutations M1-M3 disturb the macOS `display` probe** was
   reasoned (the probe looks for `present: drawable acquired` and
   `DISPLAY_PROBE_OK`), not run. 17d Step 3 says what to do if they do.
8. **The Mac round's mechanics** (channel branch, who commits) are
   assumed unchanged from Task 16.
9. **Filing a new issue for NVIDIA, native Xorg and Intel GL** is this
   plan's decision; the brief said those stay in the tracker, and no such
   issue exists today.
10. **17d and 17e each end in a Linux commit that records a Mac report.**
    If recording commits are not wanted as separate history, both fold
    into 17f's.
11. **Five Metal-run tests are edited on Linux without being runnable
    there.** The edits are textual and the checker reads them, but their
    first execution is 17d.

## Self-review

- **Coverage of the bullet and the progress file.** Stderr sweep: 17a.
  Late-rejection test: 17b. The injected-batch assumption: 17c. The
  tracker's test-work items and the undone canary check: 17e and "Needs
  the user at the Mac". `CompletionGate`: "Scope decisions", D. Nothing
  else is assigned to Task 17 in either document.
- **Names used across tasks.** `mut`, M1-M3, S, T, K, W are defined once
  and used by 17a, 17b, 17c and 17d under the same letters. The file
  counts: 39 UI files today, 40 after 17a, 41 after 17b; JS totals 242,
  243, 244.
- **No step leaves a choice open** except 17d Step 2, which is a decision
  the Mac's census has to inform and which goes to the user if the
  census surprises.

## Task 17d as built

Task 17d is the Mac's run of the 17d block and one Linux commit, "Record
the Mac check of Task 17 on Metal", which added this section. Measured on
2026-11-21 on the Apple-silicon Mac (Apple M4 Max, macOS 27.2), in the
ASAN UI build, at "Stop relying on one drain for injected events", with
the screen unlocked throughout. The Mac made no commit, and no RUN line
needed a Metal-only form. Every prediction in the block held; the one
place the report differs from the block's text is W's exit status, below.

- **Step 1, the census: 37 passed, 4 unsupported, 0 failed, of 41**, as
  derived. The four unsupported are the four predicted: `ui-clipboard`,
  `ui-display-busy`, `ui-real-events-demo`, `ui-sokol-validation-ci`. The
  block's `rm -f $B/test/Output/ui-*` complained that
  `ui-bundle-argv.js.tmp.uiargv` is a directory; every `.err` file was
  still removed and regenerated (52 after the run). The `uniq -c` output,
  whole and verbatim:

  ```
     1 [sokol-produce:error] item N: VALIDATE_BEGINPASS_COLORATTACHMENTVIEW_SAMPLECOUNT: sg_begin_pass: when resolve attachments are provided, the color attachment sample count must be > 1
     1 [sokol-produce:error] item N: VALIDATE_BEGINPASS_COLORATTACHMENTVIEW_TYPE: sg_begin_pass: color attachment view has wrong type (must be sg_view_desc.color_attachment)
     1 [sokol-produce:error] item N: VALIDATE_BEGINPASS_RESOLVEATTACHMENTVIEW_TYPE: sg_begin_pass: resolve attachment view has wrong type (must be sg_view_desc.resolve_attachment)
     1 [sokol-produce:panic] item N: raised deliberately by hnui_demo_addon
     1 [sokol-produce:panic] item N: VALIDATION_FAILED: validation layer checks failed
  ```

  A second grep, for `[sapp:` or `[sokol-` anywhere in a line of any
  `ui-*` output file, found them in two files only:
  `ui-sokol-panic.js.tmp.logger.err` (one line) and
  `ui-sokol-validation.js.tmp.err` (four). So **a healthy Metal run
  prints no `[sapp:` line at any level and no `[sokol-present` line**.
  Open question 1's assumption held, and Step 2 (Linux) had nothing to
  decide: no line was refused that a healthy run prints.
- **Step 2, the checker on BSD userland:** `ui stderr rule: 41 file(s),
  0 violation(s)`.
- **Step 3, the exclusions fire: M1 24 red, M2 24 red, M3 23 red**, as
  derived. Each run had the same four unsupported, so the mutations did
  not disturb the `display` probe (open question 7). Each list is the
  block's list of 24: `ui-abi-sizes`, `ui-clipboard-mac`, `ui-close`,
  `ui-commands`, `ui-conformance`, `ui-create-window`, `ui-demo-visual`,
  `ui-early-events`, `ui-event-exceptions`, `ui-events`, `ui-exit-paths`,
  `ui-late-rejection`, `ui-metal-clear`, `ui-metal-maintenance`,
  `ui-metal-maintenance-wake`, `ui-metal-occlusion`, `ui-present`,
  `ui-raf`, `ui-real-events`, `ui-resize-generation`, `ui-sokol-panic`,
  `ui-sokol-validation`, `ui-terminate`, `ui-wrong-thread`. Under M3
  `ui-sokol-validation` is the one that stays green. All 71 failures name
  the mutation line as FileCheck's `found here`; none failed for another
  reason. One first error per mutation, as reported:
  - M1, `ui-terminate`: `DEFAULT-TRACE-NOT: excluded string found in
    input`, from `--implicit-check-not='[sapp:error'`, at
    `[sapp:error] item 0: mutation (sokol:0)`.
  - M2, `ui-metal-maintenance-wake`: `TRACE-NOT`, from
    `--implicit-check-not='[sokol-'`, at
    `[sokol-present:warning] item 0: mutation (sokol:0)`.
  - M3, `ui-close`: `DEFAULT-TRACE-NOT`, from
    `--implicit-check-not='[sokol-'`, at
    `[sokol-produce:error] item 0: mutation (sokol:0)`.

  That covers the five Metal-run tests 17a edited without being able to
  run them (open question 11): each passed clean and went red under the
  mutations.
- **Step 4, the late rejection:** `ui-late-rejection.js` passed 6 of 6
  on the clean build. Under W it failed, and the AddressSanitizer report
  is a heap-use-after-free **in `onWorkerLoopTurn`**, the same place as
  on Linux, reached from `uv__run_check`; the window was freed by the
  `abandon` lambda in `onSharedContextReady()` and allocated in
  `onSharedContextReady()`. The status differs from Linux's: the REQUIRE
  run aborted with 134 (`Abort trap: 6`) where Linux exits 1, so lit
  stopped at `CODE: EXITCODE 0`. The test's header says "status 1"; on
  the Mac it is 134. Red either way.
- **Step 5, one dispatch pass under vsync:** the 32-run loop over
  `ui-events.js`, `ui-close.js` and `ui-event-exceptions.js` printed
  `32 0`. The five mutation rows, three runs each:

  | Applied | `ui-events.js` | `ui-close.js` | Runs |
  |---|---|---|---|
  | S | green | green | 3 of 3 |
  | T | **red** | green | 3 of 3 |
  | S + T | **red** | green | 3 of 3 |
  | K | green | **red** | 3 of 3 |
  | S + K | green | **red** | 3 of 3 |

  T and S + T print the line Linux printed: `TICKS WRONG barrier true,
  promise at 4, char at 2`. K and S + K print `KEY-AFTER-CLOSE` after
  `REPLACED-EXIT args=0`; FileCheck's first error there is
  `REPLACED-NEXT: RAF 0 is not on the line after the previous match`, so
  the `-NEXT` sequence reports it before the
  `--implicit-check-not=KEY-AFTER-CLOSE` exclusion is consulted.
  **S's 50 ms sleep made no difference under vsync**: every row reads the
  same with S as without it. Whether S actually split a batch on that
  display was not checked; the rows show only that nothing changed.
- **Step 6:** `git status --porcelain` empty, clean binaries rebuilt.

**What this does not cover**, from the Mac's report:

- The census saw the paths these 41 files take on one display. A Sokol
  error on a path no test takes is unseen, as Review Focus item 3 says.
- Step 3 proves the exclusions fire on a line printed in `onInit`. A line
  printed later in a run is caught by the same flag; no test injects one.
- Step 5 ran on one vsynced display. A display without vsync, or a
  ProMotion one, was not tried.

## Task 17e as built

Task 17e is the Mac's run of the 17e block, one test fix it led to, and
one Linux commit, "Record the Mac suite runs for Task 17", which added
this section. Measured on 2026-11-21 on the same Mac, after 17d, at
"Stop relying on one drain for injected events". The JS totals in both
builds are the ones predicted. The unit total in Release was not: one
unit test failed there, every time, and it was the test that was wrong.

**Release (`cmake-build-release-ui`), once.**

- **The build:** no warning in `lib/compositor-gpu` or
  `tools/hermes-node-ui`; the only warning is `ld`'s `ignoring duplicate
  libraries`. With a caveat: the directory was last built at Task 16's
  tip and no product source changed since, so this build recompiled the
  version-dependent files plus `ui_module.cpp`, `ui_platform.cpp` and
  `ui_events.cpp`. `lib/compositor-gpu` was not recompiled, so "no
  warning there" is Task 16's build of it.
- **Unit: 455 passed, 1 failed, of 456.** This is the first Release unit
  run on the Mac, so the count the block asked for is 456 tests, the
  same as the ASAN build. The failure is
  `SurfaceTableTest.ProducerConsumerStressCompletesRealCycles`:
  `Expected: (taken.load()) > (100), actual: 5 vs 100`, "the consumer
  must have taken real frames".
- **JS: 238 passed, 6 unsupported, 0 failed, of 244**, as predicted.
  `check-hermes-node` stops at the unit failure, so the Mac ran it a
  second time with `-- -k 0` to be sure the JS half ran: the same unit
  failure (`9 vs 100`) and 238 + 6 again.
- **The six unsupported are the six predicted:** `bundle-yargs`,
  `ui-clipboard`, `ui-display-busy`, `ui-real-events-demo`,
  `ui-sokol-validation-ci`, and `ui-sokol-validation` (a Release build
  compiles Sokol's validation layer out, so Release Metal runs have no
  validation-layer coverage).
- **The clipboard's digest** was the same before and after, and the same
  again after the ASAN runs.

**The failing unit test was a test defect, and is fixed** in "Give the
surface table stress test's consumer its turns". In isolation in Release
it failed 8 of 8, with the consumer having taken 5, 3, 1, 1, 2, 1, 4 and
2 frames, in about 7 ms a run; its other two checks passed (no violation,
and the producer published more than 1000 frames). The same test passed
in the ASAN binary. The producer is a loop of 200,000 acquire-and-publish
calls under the table's lock; a `std::mutex` is not fair, and in an
optimized build on that machine the producer finished before the
consumer had won the lock more than a handful of times. So the check
`taken > 100` depended on scheduling, which this plan's Global
Constraints rule out. Nothing in the failure pointed at the surface
table itself. The fix leaves the stress loop as it was and, after it,
publishes one frame at a time and waits for the consumer to take each,
until it has taken more than 100; the wait is bounded by a count of
yields, not by a clock.

The Mac's re-check at that commit:

- The test alone, Release: OK 8 of 8, in 12 to 25 ms.
- `check-hermes-node-unit`: **456 passed in Release and 456 in ASAN.**
- The test's own named mutation, in Release (drop
  `if (slot.state != SlotState::kFree) continue;` from `acquireLocked`):
  failed 3 of 3 on the violations count (12233, 7189, 3866). So the new
  tail did not cost the test the defect it exists to catch.

Not covered by that re-check: it ran on one fast, idle machine, not
under load, which is where the yield bound would be tested; and the
`taken > 100` count now comes partly from the paced tail, so the
contention loop by itself still gives the consumer no guaranteed share
of the lock in Release.

**ASAN (`cmake-build-asan-ui`), three times, last.** Each run: **456
unit tests passed; JS 239 passed, 5 unsupported, 0 failed, of 244**, as
derived. The five unsupported are the five predicted: `bundle-yargs`,
`ui-clipboard`, `ui-display-busy`, `ui-real-events-demo`,
`ui-sokol-validation-ci`. Neither known flake fired. The worktree was
clean afterwards.

**What ran on which commit.** The Release JS suite and the three ASAN
whole-suite runs were on "Stop relying on one drain for injected
events", the commit before the test fix. After the fix only the unit
suites were rerun (Release and ASAN) and the one test alone. The Release
JS suite was not rerun after it, and neither were the three ASAN
whole-suite runs; the fix changes `unittests/CompositorTest.cpp` and
nothing else. So "the three ASAN runs must be the last thing run, on the
last commit" holds for every file except that one unit test, and the
Release build has not had a single run in which both halves were green
together.

**Tracker.** 01a1fbb5-8545 covered two things, display sleep and the
whole suite under Release. The second is the run above. The first was
measured the same day by the user at the Mac ("Task 17f as built" has
the record). Both of the issue's closing conditions being met, it is
closed, with a comment that carries both measurements and what they
leave unmeasured.

