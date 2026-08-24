# Implementation Progress

Tracks progress on `docs/superpowers/plans/2026-08-12-bytecode-compile-cache-plan.md`
(implementation plan) and its companion design doc
`docs/superpowers/specs/2026-08-12-bytecode-compile-cache-design.md`.

## Status

| Step | Description | Status |
|------|-------------|--------|
| Task 1 | Library skeleton, CRC32, key and generation naming | done |
| Task 2 | Cache root resolution and generation pruning | done |
| Task 3 | `CompileCache` class tying key, entry and generation together | done |
| Task 4 | Enable the compile cache at bootstrap behind flags and env vars | done |
| Task 5 | Consult the compile cache from the CJS loader hook | done |
| Task 6 | Consult the compile cache from the module loader | done |
| Task 7 | Compile cache robustness tests (corruption, syntax errors, `--inspect`, TS) | done |
| Task 8 | (folded into review rounds for Tasks 5-7; no separate task file) | done |
| Task 9 | Measure and document | done |

All nine tasks are complete. The measurement and documentation work in
Task 9 itself changed no production code -- only this progress file and a
new `CLAUDE.md` section. Task 9's own investigation did surface a real bug
(see "Bug found" below), which a separate follow-up commit fixed.

## Task 9: Measurement

### Method

`examples/flow-bundler` bundles a Flow fixture into 6 output bundles via
Babel, requiring ~2841 CommonJS modules (18.6 MB of source) across the run.
Measured with `cmake-build-release` (ASAN is 10x+ too slow for this
workload) on the same machine used for the original pre-cache baseline
(flow-bundler ~6.3 s total, ~3.15 s of which was Hermes compiling, before
any caching existed).

Exact commands (from the task-9 brief, Step 2), run from
`examples/flow-bundler`:

```bash
export HERMES_PARSER_NATIVE_ADDON=$(cd ../.. && pwd)/cmake-build-release/external/hermes-parser-native/hermes-parser.node
rm -rf /tmp/hncc-bench out
time ../../cmake-build-release/bin/hermes-node --compile-cache=/tmp/hncc-bench \
  -r ./babel-register.js ./bundler/buildBundleCLI.js -c ./build.config.js > /dev/null   # cold
rm -rf out
time ../../cmake-build-release/bin/hermes-node --compile-cache=/tmp/hncc-bench \
  -r ./babel-register.js ./bundler/buildBundleCLI.js -c ./build.config.js > /dev/null   # warm
du -sh /tmp/hncc-bench
find /tmp/hncc-bench -type f | wc -l
```

### Results (actual, not predicted)

The cold/warm protocol was run four times total (see the task-9 report,
`.superpowers/sdd/2026-08-12-bytecode-compile-cache-plan/task-9-report.md`,
for full transcripts of all four as Runs A-D): the exact brief commands
were run twice -- once at the very start of the session (true cold OS
file cache) and once again, as the final recorded pass, after the OS file
cache had been warmed by earlier runs -- plus two more rounds of
controlled trials in between to chase down why the first pass showed
unusually high variance. The two below ("final clean pass" and "very
first invocation") are the two literal-brief-protocol runs; the three
controlled trials that follow are the extra rounds; see "Deviations and
concerns" below for why the variance happened.

**Literal brief protocol, final clean pass** (OS file cache already warm
from earlier passes in the same session -- this is the steady-state number
a developer would actually see after the very first invocation ever):

- Cold (empty compile-cache dir, cache populated during the run): **4.247 s**
- Warm (compile-cache dir from the cold run, untouched): **2.643 s**
- Cache size: **16 MB**
- Cache entry count: **1506 files**
- Reduction: ~38% (warm is 62% of cold wall time)

**Very first invocation of the session** (both the OS page cache for
source files AND the compile cache were cold -- the true "never run
before" case):

- Cold: **8.006 s**
- Warm: **4.196 s**
- Cache size: **16 MB**, **1507 files**
- Reduction: ~48%

**Three additional repeated cold/warm trials**, with OS file cache
pre-warmed by an untimed `--no-compile-cache` run first (isolates the
compile-cache effect from disk I/O noise):

| Trial | Cold | Warm | Reduction |
|-------|------|------|-----------|
| 1 | 4.367 s | 2.704 s | 38% |
| 2 | 4.333 s | 2.752 s | 36% |
| 3 | 4.302 s | 2.675 s | 38% |

Warm was materially and reproducibly faster than cold in every trial (36%
to 48%). No investigation into a bug was needed, but see below for a
different discrepancy that was investigated.

**`HERMES_NODE_DEBUG_NATIVE=COMPILE_CACHE` on a fully-warmed cache** (extra
verification, not required by the brief since warm was already clearly
faster, but run anyway to confirm hits rather than misses): 2841 compile
attempts logged, **2837 hits**, only **4 distinct misses** (a config file
that is `require()`-d twice by the Babel loader itself, plus one Babel
internal file loaded via a code path that this cache does not intercept).
Confirms the warm-run speedup is from real cache hits, not a lucky
coincidence.

**No-cache baseline for context** (`--no-compile-cache`, OS file cache
warm): consistently **~5.95-6.0 s**, matching the historical pre-cache
baseline of ~6.3 s well.

Note that the cold figure is not a no-cache baseline. The cache is
write-through, so `save()` runs synchronously right after each miss:
within one run, the ~1335 repeat compiles (~2841 consults against ~1506
distinct paths, see "Cache entry count" below) are served from entries
already written earlier in that same run, including during the cold run
itself, whose cache directory starts empty but does not stay empty.
Cold-vs-warm therefore measures the cross-run benefit alone and
understates the cache's total effect relative to running with no cache at
all.

### Correctness check

```
$ ./examples/flow-bundler/run.sh cmake-build-release
PASS: 6 bundles match expected/
```

### Deviations and concerns

- **Absolute times are noisy and depend heavily on OS page-cache state**,
  not just the compile cache. The very first invocation of the session
  (true cold OS file cache) took 8.0 s cold / 4.2 s warm; once the 18.6 MB
  of source was resident in the OS page cache from prior runs, cold dropped
  to ~4.3 s and warm to ~2.7 s, and stayed there across repeated trials.
  The *relative* improvement (cold vs. warm, same OS-cache state) is the
  meaningful, reproducible number: consistently 36-48% depending on
  conditions, comfortably clearing "materially faster."
- **The prediction's absolute warm figure (3.2-3.5 s) was not hit either
  way**: the steady-state warm run (2.643 s) beat the prediction, the
  very-first-run warm number (4.196 s) missed it on the slow side. Both are
  real numbers depending on OS-cache state; neither is wrong, they measure
  different things. Worth flagging since the plan's prediction did not
  anticipate this axis of variance.
- **Cache-enabled cold is faster than no-cache-at-all, even though it does
  strictly more work (compiles AND writes ~1506 files to disk).** No-cache
  baseline: ~5.95-6.0 s. Cache-enabled cold (same OS-cache state): ~4.3 s.
  Investigated via code reading, not guessing: the no-cache path
  (`lib/bindings/node_contextify.cpp`, `compileFunctionForCJSLoaderCb`)
  calls `napi_run_script`, which compiles at Hermes's default (full) debug
  info level. The cache-enabled path (hit or miss) always goes through
  `hermes_compile_to_bytecode` / `hermes_run_bytecode` instead, which
  compiles at a lower debug-info setting (see the `--inspect` note in the
  new CLAUDE.md section -- entries compile at `DebugInfoSetting::THROWING`,
  not `ALL`). So part of the cold-run advantage over no-cache-at-all is a
  cheaper compile flag that comes bundled with turning the cache on, not
  purely disk-cache-avoidance. This is existing, intentional behavior from
  earlier tasks (not something Task 9 changed), but it means the cold-vs-
  no-cache gap should not be read as "the cache write itself is free" --
  it conflates two effects. The cold-vs-warm comparison, both at the same
  debug-info level, isolates that one confound -- though as noted above
  it has its own caveat (cold already contains a same-run cache benefit
  for repeat paths), so it is a clearer comparison than cold-vs-no-cache,
  not a wholly clean one. It's what's reported above as the headline
  number.
- **Cache entry count (1506-1507) is well under the ~2841 predicted.**
  Investigated: 2841 is the total number of `compileFunctionForCJSLoader`
  invocations logged across the run (the bundler builds 6 separate output
  bundles, each re-`require()`-ing shared `node_modules` files through its
  own module graph, since the bundler does not share Node's `require`
  cache across bundle builds). The compile cache key is the file's
  absolute path, not its content, so repeated compiles of the same path
  across the 6 bundles collapse onto the same ~1506 cache entries. This
  is correct, intentional cache behavior, not a bug -- confirmed by the
  hit/miss log showing 2837 hits against only 1506 distinct hit paths.
  The design's "~2841 entries" prediction assumed one entry per compile
  call; the real relationship is one entry per distinct path.
- Cache size (16 MB) landed inside the predicted 15-20 MB range.
- `git diff --stat` over the whole branch, per the verification checklist,
  is not diffed against a merge-base by this progress file -- it was
  checked manually at commit time via `git status`, which showed no
  changes under `libjs/`, `libjs-node/`, or `hermes/`.

### Bug found and fixed: `~/.cache/hermes-node` was not left absent (verification checklist item)

The plan's verification checklist requires: "`rm -rf ~/.cache/hermes-node &&
cmake --build cmake-build-asan --target check-hermes-node` leaves
`~/.cache/hermes-node` absent." At the time this was found, that did
**not** hold on this branch. `check-hermes-node` still passed (281/281
tests green), but the directory was recreated during the run regardless.

**This has since been fixed in commit `180b670`** ("Disable compile cache
in the inspector's second runtime"), by a separate agent following up on
this finding. The fix sets `inspectorConfig.disableCompileCache = true;`
explicitly in `lib/runtime/hermes_node_runtime.cpp`, since the inspector's
second runtime only ever evaluates `require('inspector-server')` and all
built-in JS is already embedded as precompiled bytecode -- there is
nothing for it to gain from a cache. `test/compile-cache-inspect.js` was
also strengthened to point the default cache root at a temp
`XDG_CACHE_HOME` and assert nothing appears there, so the test now
exercises the inspector runtime's own config instead of only checking the
parent's `--compile-cache` directory (which is what let the original
regression slip past that test). The root-cause investigation below is
kept as a record of what was found and how; the "Not fixed" framing that
originally followed it no longer applies.

**Root cause** (found by bisecting which test caused it, then by a
temporary diagnostic `fprintf` in `createCompileCache` -- reverted before
committing, `git diff` on that file is clean): when `--inspect` or
`--inspect-brk` is used, `runHermesNode()`
(`lib/runtime/hermes_node_runtime.cpp`) spawns a background thread that
runs a **second, nested** `runHermesNode(inspectorConfig)` to host the
`inspector-server` module (around line 674-680):

```cpp
HermesNodeConfig inspectorConfig;
inspectorConfig.evalCode = "require('inspector-server');";
inspectorConfig.argv = {"hermes-node-inspector"};
inspectorConfig.inspectorBridgeContext = bridgeCtx;

inspectorThread = std::thread([bridgeCtx, inspectorConfig]() {
  runHermesNode(inspectorConfig);
  ...
```

`inspectorConfig` is freshly default-constructed: it does **not** inherit
`inspect`, `inspectBrk`, `disableCompileCache`, or `compileCacheDir` from
the outer config that the user's CLI flags populated. Its own
`createCompileCache(inspectorConfig)` call therefore sees `inspect=false`,
`disableCompileCache=false`, `compileCacheDir=""`, falls through to
`compileCacheDefaultRoot()`, and calls `cache->enable()` on the real
`$XDG_CACHE_HOME`/`~/.cache/hermes-node` path -- regardless of what the
user passed on the command line (`--no-compile-cache`,
`--compile-cache=<dir>`, or the mere presence of `--inspect` itself, which
is documented as disabling the cache).

In practice this only creates the empty directory skeleton
(`v1/<generation>/`), not populated cache files: `inspector-server` is
loaded in a way that never reaches either of the two consult points
(`compileFunctionForCJSLoaderCb`, `compileAndRunCallback`), confirmed by
`HERMES_NODE_DEBUG_NATIVE=COMPILE_CACHE` producing zero hit/miss lines for
the inspector thread. So no user or `node_modules` bytecode leaks into the
default cache this way -- but the flags are still silently ignored, and a
stray directory is still written under the user's real `$HOME` even when
they explicitly asked for no caching.

Under the test suite this only surfaces because `test/lit.cfg`'s suite-
wide `HERMES_NODE_DISABLE_COMPILE_CACHE=1` (an environment variable, which
*is* inherited by the child thread since env vars are process-global, not
per-`HermesNodeConfig`) is the one thing that normally prevents it -- and
`test/compile-cache-inspect.js` is the only test in the suite that
deliberately clears that variable (via the `%hermes-node-cc`
substitution) while also passing `--inspect`, in order to test the
documented "cache is disabled under --inspect" behavior. At the time,
that test passed anyway, because it only asserted its own `%t.cache`
stayed absent, which it did; it did not check the real default location,
so it did not catch this (this is exactly what `180b670` strengthened it
to do). Confirmed via `strace -f -e trace=mkdir,mkdirat` that the mkdir
sequence terminates at exactly `~/.cache/hermes-node/compile-cache/v1/
<generation>`, and bisected to this one test file by running each
`compile-cache-*.js` test individually against a freshly wiped
`~/.cache/hermes-node`.

Task 9 itself is documentation-only per the plan's Global Constraints
("Task 9 ... changes no production code"), so the fix was not made as
part of this task: the diagnostic `fprintf` used to confirm the root
cause was reverted before the Task 9 commit, leaving
`lib/runtime/hermes_node_runtime.cpp` unchanged in that commit. It was
flagged as a concern for a follow-up fix, and `180b670` is that follow-up
(see above).

Full raw command transcripts are in
`.superpowers/sdd/2026-08-12-bytecode-compile-cache-plan/task-9-report.md`.
