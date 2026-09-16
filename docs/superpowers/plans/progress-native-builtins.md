# Implementation Progress

Tracks `docs/superpowers/plans/2026-09-14-native-builtins.md` (implementation
plan) and its companion design doc
`docs/superpowers/specs/2026-09-14-native-builtins-design.md`. Full
task-by-task review history (reviewer findings, fix rounds, commit-by-commit
detail) lives in the session ledger,
`.superpowers/sdd/2026-09-14-native-builtins/progress.md`; this file is the
durable record that survives after that ledger's session ends.

## The corpus's first finding: native built-ins lose error stacks

Filed as dz `01a0a417-48e6`. An error raised through `internal/errors`'
`hideStackFrames()` wrapper -- every `ERR_*` validation error Node's lib
throws, and every `AssertionError` from
`assert.strictEqual`/`deepStrictEqual` -- carries no stack frames at all in a
`build-native` artifact with the default native built-ins, where the same
artifact built `--bytecode-builtins` names the failing user line. Only the
built-ins' compilation mode differs; the user's own modules are native in
both. Measurements and the suspected mechanism are in the issue.

It is diagnosability, not correctness -- the error, its `code` and its
message are all right -- and no corpus test fails because of it, which is why
the suite below is green with it open.

**The current behaviour is asserted, not merely filed.**
`test/build-native-builtins.js` builds one program twice and requires the
native artifact to report no stack frames and the `--bytecode-builtins` one
to report some, following `test/build-native.js:77-88`'s precedent for a
known gap. Two prefixes rather than one line, because neither alone
distinguishes "native loses the stack" from "nothing here has a stack".
Fixing the defect fails that test, which is the point.

The language-flag divergence was checked before anything else, being the
failure this corpus was built to catch and the one that shows no diagnostic
either way. Task 5's `shermes` invocation does carry
`${HERMES_NODE_JS_LANGUAGE_FLAGS}`, and a `let`-in-loop closure in a native
artifact captures per iteration (`0,1,2`), so nothing here is that.

## Status

| Task | Description | Status |
| --- | --- | --- |
| 1 | Compile Hermes's own bootstrap JavaScript natively | done |
| 2 | Let an embedded module be a native unit | done |
| 3 | Share the built-in module id transform | done |
| 4 | Generate a native embedded module registry | done |
| 5 | Build the built-in modules as native units | done |
| 6 | Ship the native built-ins archive in the kit | done |
| 7 | Link the native built-ins by default | done |
| 8 | Pin the claim with a magic-count test; measure the cost | done |
| 9 | Build, seed and green the native test corpus | done |
| 10 | Document it | done |

All ten tasks are complete; the plan is finished.

## The three numbers

Everything else here can be recomputed from the tree. These cannot, cheaply:

| | |
| --- | --- |
| per-artifact size delta (Task 8) | **+7,271,120 bytes** (hello world: 12,626,776 -> 19,897,896), and **+7,269,488** on a real 137-module program -- a constant, not a proportion |
| executed corpus (Task 9) | **112** tests of 138 candidates, **27** excluded in four classes (bare `internal/*` require 10, re-spawns the binary 13, observes the binary 1, reads its own source 3) |
| `check-hermes-node-native` wall clock | **9.0 s** from clean, 16 threads, macOS arm64 Release (10.3 s including re-cutting the kit) |

Two more worth having beside them: the built-ins compile in **8.2 s** wall /
**47.6 s** CPU at `-j16` on a 16-core macOS arm64 (187 modules, zero failures,
zero warnings), and `HERMESVM_INTERNAL_JAVASCRIPT_NATIVE=ON` costs
`hermes-node` itself **+337,904 bytes**.

"Zero warnings" only became a claim worth making in the fix wave. Until then
`shermes` was invoked with `-w`, which silences everything, so the statement
was unobservable rather than false; it now passes the bytecode pipeline's own
`-Wno-undefined-variable`, which leaves all 187 clean and produces a
byte-identical archive.

## Commit range

Outer repository: `0084601..e01274f` (Tasks 1-7), then each later task's own
commit.

## Task 8

Added `test/fixtures/native/count-magic.py`, which parses a Mach-O (thin or
fat) binary's own header and counts occurrences of the Hermes bytecode magic
`0x1F1903C103BC1FC6` independently per architecture slice -- averaging across
slices was rejected explicitly, since 0 and 2 average to 1 and would pass a
binary with one entirely-wrong slice. Sanity-checked against
`cmake-build-release/bin/hermes-node` before trusting it in a test: reports
`MAGIC 0 188` (187 embedded modules + Hermes's own `ExtensionsBytecode`,
`InternalJavaScript` native since Task 1) and exits 0 against an expectation
of 188, exits 1 against an expectation of 1.

`test/build-native-builtins.js` builds the same entry twice with
`build-native` -- once with the default (native built-ins) and once with
`--bytecode-builtins` -- runs both and diffs their output, then asserts the
default artifact carries exactly 1 bytecode blob per slice (Hermes's
`ExtensionsBytecode`, unreachable from this repo) and the `--bytecode-builtins`
artifact carries at least 180 (a floor, not an exact count, since the point of
that line is only that the flag did something). `REQUIRES: linker-available,
shermes-available`, so a checkout with no kit reports it UNSUPPORTED rather
than failing.

### Measured size delta

Machine: macOS arm64. Build: `cmake-build-release` (Release, Clang), same kit
Task 7's report used.

Trivial entry (`console.log("hello")`, one user module, run from `/tmp`):

| build | bytes |
| --- | --- |
| `--bytecode-builtins` | 12,626,776 |
| native built-ins (default) | 19,897,896 |
| delta | +7,271,120 |

Matches Task 7's own hello-world measurement (12,626,904 / 19,898,216, delta
+7,271,312) to within a few hundred bytes -- the residual is consistent with
the two runs using different entry file names/paths, which land in the
generated unit table and the embedded `HERMES_NODE_VERSION_STRING`/build
metadata, not a regression.

Real program: `examples/ditz2` (137 of its own CommonJS modules, all compiled
natively regardless of the flag -- `--bytecode-builtins` only changes which
built-in registry is linked). `node_modules` and the `ditz2` submodule were
already present in this checkout, so `./build-cjs.sh` was run and both
artifacts were built from `dist-cjs/cli/main.js`:

| build | bytes |
| --- | --- |
| `--bytecode-builtins` | 15,197,928 |
| native built-ins (default) | 22,467,416 |
| delta | +7,269,488 |

The producer emitted zero warnings for either build (fully static
`require()` graph -- tsc output, like `build-native`'s other worked
example). Both binaries ran (`./dz-native list`, `./dz-bytecode list`) and
produced the same ditz2 issue listing. Magic counts:
`count-magic.py dz-native 1` -> `MAGIC 0 1`;
`count-magic.py dz-bytecode 180 9999` -> `MAGIC 0 188`.

The delta is nearly identical between the trivial program and a 137-module
real one (+7,271,120 vs. +7,269,488): the built-in registry's native code is
a fixed cost paid once per binary, independent of how large the user's own
program is. The design expected a fixed cost but hedged on its size, so this
confirms the shape rather than the number.

## Task 9

`check-hermes-node-native` re-runs the ordinary JS tests through
`build-native`, one linked executable per test, so real programs exercise the
natively compiled `fs`, `path`, `net`, `util` and the rest. Selected by
`test/native/corpus.txt`, reconciled against the tree by
`check-hermes-node-native-corpus`, which the suite depends on.

### What the corpus turned out to be

`check-corpus: 138 candidates, 112 in corpus, 27 excluded` -- the three
numbers do not sum, because one exclusion (`test-process-version.js`) has a
wrappable RUN line and a second RUN line beside it, so it is listed without
ever having been a candidate. Full clean run (work tree
removed first, Release, macOS arm64, 16 threads): **9.0 s wall clock**, 112
expected passes. Each artifact is a real 19.9 MB native executable carrying
one bytecode blob, checked with Task 8's `count-magic.py`.

Node-ported coverage holds: **39 of the 45** node-ported candidates run, so
the part of the corpus that exercises Node's own lib code hardest is intact
rather than excluded away.

### The six built-ins with no direct coverage

`querystring`, `dgram`, `https`, `tls`, `cluster` and `diagnostics_channel`
are compiled natively and no corpus test reaches any of them directly. Every
test that would have is in the re-spawn class below, so this gap is a
consequence of that one and closes with it. Named here and not only in
CLAUDE.md because this file is the record that outlives the ledger, and
because the list is the shopping list for whoever closes it: a test for any
of the six has to avoid `process.execPath` to be addable at all.

Nothing here says the six are broken -- they are compiled by the same
pipeline as the 181 that are covered, and a failure in that pipeline would
not single them out. What is missing is the evidence, not the function.

### Three RUN shapes, and the fifteen tests the first two hid

`corpus_util.py` recognises `%hermes-node %s | %FileCheck %s`, bare
`%hermes-node %s`, and the node-ported `TEST_THREAD_ID=$$ %hermes-node %s`.
The bare shape was missing at first, and the consequence is worth recording
because it is the failure mode the two manifests exist to prevent, arriving
through the one door they do not watch: a file matching no shape is a
candidate for nothing, so the checker says nothing about it. Fifteen
top-level tests were invisible that way, and with them the only direct
coverage of crypto, repl, querystring, dgram, https, tls, cluster and
diagnostics_channel -- crypto being one of the natively compiled built-ins.
Nine of the fifteen now run.

### Exclusions, by class

Every exclusion except the seven seeded before the first run was reached by
building and running the test. The ones that produced a failure were then
built a second time with `--bytecode-builtins` and the two runs diffed: all
of those fail identically both ways, which is what makes them properties of
bundling or of the harness rather than of native compilation. The ones that
re-spawn themselves never got that far -- see below.

| class | n | what it is |
| --- | --- | --- |
| requires an `internal/*` module by bare specifier | 10 | the closed world's builtin classifier does not answer for `internal/*`, so the require is `MODULE_NOT_FOUND`. These tests exist to poke at hermes-node's own internals and are the ones least able to survive bundling. |
| re-spawns the running binary | 13 | `process.execPath` or `process.argv[0]`, or a `--node-version` RUN line a produced executable cannot accept. Seven were seeded before the first run; six more were found by vetting. |
| observes the running binary without spawning | 1 | `test-module-builtin.js` asserts `Module._resolveFilename(__filename)` returns `__filename`, which the closed world refuses. |
| reads its own source file | 3 | `fs.open`/`readdir`/`stat` of `__filename`: the producer packages code, not source text. |

### The re-spawn class is a fork bomb, not a failing test

A produced executable ignores every argument -- that is what keeps
`process.argv.slice(2)` meaning what it means -- so a test that runs
`process.execPath` with a script or a flag gets another copy of *itself*,
which does the same again. `test-child-process-exec-timeout.js` found this
the expensive way during the first suite run: it spawns three children per
level, and reached roughly **6,000 live processes** before it was killed,
which needed the artifact deleted and repeated `pkill` rounds.

The five found afterwards (`test-cli-eval.js`, `test-inspect.js`,
`test-process-stdio.js`, `test-repl-entry.js`, `test-sigint-watchdog.js`)
were vetted one at a time under a guard that runs the artifact in its own
session, samples the process count, and kills the group by `killpg` the
moment it climbs. Every one of them tripped it. Anyone adding a
`build-native` wrapper elsewhere should build that guard first, not after.

That guard is now committed as `run_guarded()` in `run-native.py`, and two
measurements from the fix wave are worth keeping. Its process ceiling of 64
was checked rather than guessed: logging every poll across a full 112-test
run put the largest group at **3** (a build's `shermes` and `cc`) and every
artifact's at 1. And the branch that actually fires is the *timeout*, not the
count -- about one run in thirty has an artifact hang past 120 s under
16-way load, which before the guard hung lit indefinitely rather than failing
a test. `test-fs-event-wrap.js` is separately flaky under the same load,
2 of 4 runs with the guard and 1 of 4 without, so it is the suite's own
load-sensitivity and not the guard's doing.

Two of the six are the project's known flaky tests, `test-inspect.js` and
`test-repl-history.js`. Both are excluded here for reasons of their own -- a
fork bomb and an `internal/repl` require -- so this suite adds no new
flakiness, by accident rather than by design.

### The socket-path tests are recovered, and the first rationale was wrong

An earlier draft of this file excluded `test-net-pipe-connect-errors.js` and
`test-net-server-listen-path.js`, claiming a digest-only work directory would
leave "about 11 bytes of headroom" against `sockaddr_un.sun_path` and that
how much was left depended on the checkout path. Both halves were wrong, and
the measurement is the correction:

| | bytes | limit | spare |
| --- | --- | --- | --- |
| `common.PIPE`, relative spelling | 73 | 104 | 31 |
| `tmpdir.path + '/0.txt'`, absolute | 86 | 104 | 18 |

`common.PIPE` is `min(relative, absolute)` (`common/index.js:313-315`) and
the suite's cwd is the test's `Output` directory, so the relative spelling
wins and the checkout path is not in it at all -- checkout depth lengthens
only the candidate that loses. That was the half worth getting right.

The other half survives in weaker form: `test-net-pipe-connect-errors.js`
builds `tmpdir.path + '/0.txt'` itself, with no relative spelling to fall
back on, so its budget *is* checkout-dependent. With the work directory named
`<exec root>/native/build/<digest>` it measured 96 of 104 -- passing here,
eight bytes from failing on a deeper checkout. Hence the two changes: the
work directory is the digest alone (`slug()` in `corpus_util.py`, which
documents why it is unreadable), and the work root is `<exec root>/nb` rather
than `<exec root>/native/build`. Together those bring it to 86 with 18 to
spare. Both tests pass.

One margin in the same file is tighter than either row above, and is unused
only by luck. The EACCES branch binds `common.PIPE` itself, and `common.PIPE`
is `min(relative, absolute)`: the relative spelling wins under lit and
measures 65, but the **absolute** one it beat measures **99 of 104** with a
five-digit `TEST_THREAD_ID` (measured on a checkout at
`/Users/tmikov/prog/hermes-node`, so five bytes of spare). Which spelling
`min()` returns is decided by lit's cwd, and unlike the relative form the
absolute one lengthens with checkout depth -- and by two more again on Linux,
where `pid_max` allows seven digits in the socket name where macOS allows
five. So a deeper checkout does not merely shrink the recorded margins; it
can also flip which spelling is chosen, onto the one with the least room.

### One harness detail that is not in the brief

The custom `lit.formats.ShTest` subclass lives in
`test/native/corpus_format.py`, not inside `test/litnative.cfg`: lit `exec()`s
a config file inside `lit.TestingConfig`'s namespace, so a class defined there
pickles as `lit.TestingConfig.CorpusShTest` and cannot be sent to a worker
process. lit ships the whole config, `test_format` included, across its
multiprocessing pool.

## Full suite

Task 8: `cmake --build cmake-build-release --target check-hermes-node`: 221
lit tests (the previous 220 plus that task's), 442 unit tests, all passing.
Neither `test-inspect.js` nor `test-repl-history.js` (the two documented
flaky tests) failed in this run.

Task 9: unchanged, 221 lit and 442 unit, green. `check-hermes-node-native` is
a separate target and was not pulled into it.

## Task 10

`CLAUDE.md` gained a "Native built-ins" subsection at the end of "Native
Compilation", and the phase 1 section's "Not in scope" row was **corrected**
rather than supplemented: it said built-in JavaScript stays interpreted
bytecode, which this plan made false, and a file that contradicts itself is
worse than one that says nothing.

The `ExtensionsBytecode` remainder was filed as dz `01a0a5cd-d63a` rather
than written up here or in `CLAUDE.md`: it is an unfixed limitation, and
those live in the tracker by that file's own rule. `CLAUDE.md` carries the
pointer and the one fact a reader needs to interpret the magic-count test
(one blob per slice, not zero).

Two things the corpus does not verify are stated in `CLAUDE.md` alongside
what it does, because a suite described only by its passing count reads as
broader coverage than it is: six natively compiled built-ins
(`querystring`, `dgram`, `https`, `tls`, `cluster`,
`diagnostics_channel`) have no direct coverage at all, their only wrappable
tests being in the re-spawn class; and the gate sees only the three RUN
shapes in `corpus_util.py`, which is how fifteen tests were once invisible
to both manifests.

## Formatting

Tasks 8, 9 and 10 add only `.py`, `.js`, `.cfg`, `.txt` and `.md` files -- all
outside `./utils/format.sh`'s C/C++ filter -- so the
clang-format-18-unavailable situation recorded for earlier tasks in the
session ledger does not apply; nothing was skipped.
