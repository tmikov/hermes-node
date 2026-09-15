# Implementation Progress

Tracks `docs/superpowers/plans/2026-09-13-native-compilation.md` (implementation
plan) and its companion design doc
`docs/superpowers/specs/2026-09-13-native-compilation-design.md`. Full
task-by-task review history (reviewer findings, fix rounds, commit-by-commit
detail) lives in the session ledger,
`.superpowers/sdd/2026-09-13-native-compilation/progress.md`; this file is
the durable record that survives after that ledger's session ends.

## Status

| Task | Description | Status |
| --- | --- | --- |
| 1 | Hermes -- growable `SHUnit` array | done |
| 2 | Hermes -- `shermes -source-name=<name>` | done |
| 3 | Hermes -- `hermes_init_sh_unit` | done |
| 4 | Fix the scanner/compiler language-flag divergence | done |
| 5 | A capturing, classifying subprocess runner | done |
| 6 | The kit carries `shermes`, the SH headers, and `ccflag:` | done |
| 7 | The unit table in the generated assembly | done |
| 8 | `kBundleFlagNativeUnits` | done |
| 9 | `lib/build-native` -- staging and command construction | done |
| 10 | `lib/build-native` -- the job pool and the unit table | done |
| 11 | Native mode in the producer | done |
| 12 | The `build-native` subcommand | done |
| 13 | Run time -- the unit table reaches `__bundleLoad` | done |
| 14 | The compile-flag parity forcing function | done |
| 15 | Closed world, addons, and WebAssembly | done |
| 16 | The worked example, the measurements, and the documentation | done |
| 17 | Classify native compile failures instead of hard-failing all | done |
| 18 | Final whole-branch review fix wave | done |

Task 18 closed the final whole-branch review's four Important findings and
seven Minor ones: the failure-policy header doc still describing the
premise Task 17 disproved; `checkIncbinPath()` unreachable on the native
path (and duplicated in `linkResponseFile()`); a `cc`-stage rejection
misclassified as a source rejection, which a realistic clang-OOM trigger
(measured 3.66 GB peak RSS on the 1,500-module graph) would turn into a
green build whose artifact throws a `SyntaxError`; and an in-place native
addon dropped from the build's own bookkeeping (no `native:` line, wrong
count) for the most ordinary invocation there is -- `build-native app.js -o
app` run inside the addon's own directory. See the session ledger
(`.superpowers/sdd/2026-09-13-native-compilation/progress.md`) for the full
list and per-finding detail.

All sixteen original tasks are complete. Every task was reviewed individually
(1-3 in the Hermes submodule, 4-15 in the outer repo); this task (16) is
measurement and documentation and was not independently code-reviewed,
since it adds no runtime-reachable code path -- `examples/tetris/run.sh`'s
new arm is exercised by running it (see below).

## Commit ranges

Hermes submodule (`hermes/`): `fb45b34..df1faf8` (Tasks 1-3), then this
task's own commit for the GC benchmark.

Outer repository: `f99af8d..ecdad94` (Tasks 4-15), then this task's own
commit for the example, spec, progress file, `CLAUDE.md` and dz issues. Task
17 (the failure-policy correction) is its own later commit, no Hermes
submodule change.
Each Hermes-submodule commit was followed by an outer commit moving the
gitlink, per the two-step convention this plan established from Task 1
onward.

## What shipped

`hermes-node build-native <entry.js> -o <file> [options]` compiles a
program's whole `require()` graph to native code with Static Hermes
(`shermes`) and links a standalone executable with the same kit
`--build-exe` uses. It reuses the bytecode bundle producer's discovery,
resolution and classification code path end to end (`lib/bundle/
bundle_build.cpp` gained a native mode rather than a parallel copy) --
closed world, the resolver, addon sidecars, preloads, baked VM options, and
the failure-tolerant-vs-fatal split are all the same code the bytecode path
uses, with the native-specific pieces (staging, `shermes`/`cc` invocation,
the job pool, the unit table) living in `lib/build-native/`, which is
VM-free.

See the "End-to-end" and "Full-GC pause versus unit count" subsections of
the spec's Measurements section for the numbers; they are not duplicated
here.

## Divergences from the plan, found during this task

- **`examples/flow-bundler` cannot be used for the ~1,500-module
  measurement the plan called for, at all.** Its own entry
  (`bundler/buildBundleCLI.js`) is Flow-typed ESM and fails to parse under
  `build-native` with the identical error the example's own README already
  documents for `--build-bundle` (`'from' expected`) -- confirmed by
  running it, not inferred, since both paths share one scanner and one
  Hermes parser. Substituted a throwaway fixture in the same directory that
  requires flow-bundler's real `node_modules` dependencies directly
  (`@babel/core`, `@babel/preset-env`, three `@babel/plugin-transform-*`
  packages, `@babel/register`, `@babel/types`, `@babel/generator`,
  `prettier`, `string-width`, `babel-plugin-syntax-hermes-parser`,
  `hermes-estree`, `hermes-transform`) -- a real dependency graph, just
  entered from a different file, landing at 1,745 modules (1,541 compiled).
  The fixture was not committed (`examples/flow-bundler/node_modules` is
  gitignored and the fixture lived there only for the duration of the
  measurement).
- **That substitute graph could not be built either, on the first try.**
  `@babel/core/lib/config/files/import.cjs` is `module.exports = function
  (filepath) { return import(filepath); }` -- dynamic `import()` inside a
  `.cjs` file, a Hermes parse error. The bytecode path tolerates this (it
  is not the entry, so it is packaged as a module that throws if ever
  required); `build-native` does not -- any `shermes` failure on any
  discovered module is a hard build error by design (see "Failure policy"
  in `CLAUDE.md`'s new section), and this file is discovered by static scan
  regardless of whether the program's execution ever reaches it. Since
  nearly every `@babel/core`-based plugin graph reaches this exact file,
  the practical consequence is that `build-native` cannot compile most
  real-world Babel-based programs today without a workaround. For this
  measurement the file was locally replaced (outside git, in the gitignored
  `node_modules`) with an equivalent that throws synchronously instead of
  calling `import()`. Filed as a dz issue (see below) rather than fixed --
  fixing it is a scope question (skip-and-stub the unreachable module the
  way the bytecode path does? refuse to reach it only when truly dead code?)
  that this plan did not sign up to answer.
- **The `-O0` "~4x faster" prediction did not hold at tetris scale.**
  Measured ~10% faster, not ~4x. The full explanation is in the spec's
  Measurements section; in short, the prediction holds for
  optimizer-dominated compiles and tetris's build time is dominated by one
  large file's C-frontend cost (`lodash.js`) plus many small files' fixed
  process-spawn overhead, neither of which `-O0` touches much.
- **The whole-artifact size ratio is not one constant.** 1.09x at tetris
  scale (22 modules), 3.07x at ~1,500-module scale, because the linked
  runtime (~12.5 MB either way) is a shrinking fraction of a larger
  program. The spec's original framing ("smaller than 7.4x because the
  runtime dominates") is only the small-scale end of this; the large-scale
  end approaches the single-file 7.4x figure. Both numbers are now in the
  spec rather than one.
- **Startup time shows no measurable native-vs-bytecode difference at
  either scale measured** (tetris: 15.81 ms vs 16.07 ms; ~1,500 modules:
  136.47 ms vs 133.82 ms, native even nominally faster there, within
  noise). This does not contradict the GC-pause-versus-unit-count finding
  below -- a single extra full GC costing under 1 ms at 1,000 units is not
  observable against tens-to-hundreds of milliseconds of process startup
  noise. It says the per-unit GC cost, at this benchmark's realistic
  symbol-table size, is not yet large enough to show up in an end-to-end
  number; it does not say the cost is zero.

## The GC benchmark (Task 16, Step 2b)

`hermes/unittests/VMRuntime/StaticHUnitTest.cpp` gained
`DISABLED_FullGCPauseVersusUnitCount`. It hand-registers 1, 100 and 1,000
`SHUnit`s, each with a real 200-entry symbol table and small property
caches, sharing one ASCII pool and one strings table across all units (only
each unit's own `symbols` array, property caches and index variable are
distinct -- that is what the GC actually walks per unit; the shared tables
measure the real cost without 1,000x redundant string data). Run by hand
with `--gtest_also_run_disabled_tests`; not wired into `check-hermes`,
because it prints rather than asserts.

**One implementation trap, found by running it, not by reading the code
first:** calling `_sh_unit_init` directly (as the brief's sketch showed)
segfaults in `GCScope::createMarker` on a null "current scope" pointer.
`_sh_unit_init` itself opens no `GCScope`; only its caller normally does
(`_sh_unit_init_guarded`'s top-level `GCScope gcScope{runtime};`), and
`_sh_ljs_create_closure` (reached while running the unit's body) needs one
already open to nest its own `GCScopeMarkerRAII` under. Every existing
fixture in this file went through the guarded entry point already, which is
why this was never hit before. Fixed by calling `_sh_unit_init_guarded`
instead, matching every other call site in this file.

Results (macOS arm64; Release and `cmake-build-asan`, five repeated Release
runs stable to a few percent):

| units | Release | ASAN+handle-sanitizer |
| --- | --- | --- |
| 1 | 0.148 ms | 1.62 ms |
| 100 | 0.217 ms | 2.47 ms |
| 1,000 | 0.925 ms | 11.4 ms |

The curve is linear once a small fixed cost is subtracted (~0.14 ms base,
~0.78 µs per additional 200-symbol unit in Release). This is the design's
largest open worry, and the answer is: yes, pause time grows with unit
count as predicted, but the absolute cost at a realistic per-unit symbol
count is small -- under a millisecond of extra full-GC pause at 1,000
units. It would need either much larger per-unit symbol tables or frequent
forced full collections to become visible against ordinary program noise.

## What is deliberately left undone

Filed as dz issues rather than fixed, because each is either a scope
question this plan did not sign up to answer or a defect nobody is working
on yet:

- Structured `CallSite`s from a native stack carry no location at any
  `-g` level (`e.stack`, the plain string, is correct at `-g2`).
- `build-native` has no object cache: every build recompiles every module,
  every time, even unchanged ones.
- `sh_unit_additional_memory_size()` undercounts every unit's heap
  footprint (`StaticHUnit.cpp:206` counts only `sizeof(unit->runtime_ext)`,
  the pointer, never the pointed-to allocation) -- wrong by a growing
  amount as unit count grows, per the Step 2b numbers above.
- ~~`build-native` cannot compile a program that reaches
  `@babel/core/lib/config/files/import.cjs`~~ -- fixed by Task 17. The
  premise this bullet rested on (a `shermes` failure past the scanner
  cannot be classified) was measured wrong: `CommandResult`'s existing
  `Exited`/`Signalled`/`SpawnFailed`/`WaitFailed` split is exactly the
  classification needed, and a source rejection (`Exited`, non-zero, with
  diagnostics) on a non-entry, non-preload module is now packaged as a
  throwing stub, same as the bytecode path's own second stub site. See
  "Failure policy" in `CLAUDE.md` and dz `01a0a0d6-03d4`.

Also not in scope, per the design and unchanged by this task: built-in
JavaScript stays interpreted bytecode, no cross-module optimization (each
module is its own `shermes` compilation unit, so the inliner cannot cross a
`require()` boundary -- see `CLAUDE.md`), no runnable native container (a
native build's payloads are empty; only a linked executable runs), and
macOS releases ship no kit at all (pre-existing, `single-executable`
plan), so a produced macOS `hermes-node` cannot self-host `build-native`
there today.

## Self-review (against the plan's own checklist)

- Spec coverage table: every spec section maps to a task; no gaps found.
- Known divergences 1-7 in the plan's Self-Review section: verified still
  accurate, items 5 and 7 are this task's own (the GC benchmark measured
  as scoped; the undercount issue filed rather than fixed).
- Full lit suite and `check-hermes-node-unit`: both green under
  `cmake-build-asan`, see the commit for exact figures; the two documented
  flaky tests (`test-inspect.js`, `test-repl-history.js`) were not observed
  failing in this run.
- `./utils/format.sh -f`: clean before commit.
- Measured numbers are in `CLAUDE.md` and the spec, not left as
  placeholders.
