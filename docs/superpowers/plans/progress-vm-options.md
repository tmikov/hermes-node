# Implementation Progress

Tracks progress on `docs/superpowers/plans/2026-09-02-vm-options-plan.md`
(implementation plan) and its companion design doc
`docs/superpowers/specs/2026-09-02-vm-options-design.md`.

## Status

| Step | Description | Status |
|------|-------------|--------|
| Task 1 | Flag classifier: which Hermes VM flags hermes-node honours | done |
| Task 2 | Parse `--vm=` strings into a `vm::RuntimeConfig` | done |
| Task 3 | `--vm=` / `--vm-help` on the command line, for a plain script | done |
| Task 4 | Format v5: VM-options section and override bit | done |
| Task 5 | `--build-bundle --vm=` records; `--dump` shows | done |
| Task 6 | A `--bundle=` run applies the container's options | done |
| Task 7 | A produced executable inherits and locks | done |
| Task 8 | Document in CLAUDE.md | done |

All eight tasks are complete, plus a whole-branch review and its fix round.
Every task was reviewed individually as well.

## Context notes

### Three flags whose effect cannot be observed the obvious way

Each of these cost a task real time, and each is a property of Hermes or of
this runtime's bootstrap rather than of the code under test.

**`-Xes6-proxy=false` can never be applied to a hermes-node runtime.**
Hermes gates the global `Reflect` object on the same `hasES6Proxy()` check
as `Proxy` itself (`hermes/lib/VM/JSLib/GlobalObject.cpp`, both under `if
(runtime.hasES6Proxy())`), and `libjs/primordials.js` unconditionally
destructures `Reflect` during bootstrap, before any user script runs. So a
container that genuinely applies that flag dies in `primordials.js` for
every program, bundled or not. It fails loudly -- a clear message and exit
1, which is the acceptable half -- but any test that has to *run* something
must use a different flag. `-enable-eval=false` is the replacement used
throughout: honoured, defaulting to true, and observable as `new Function`
or `eval()` throwing, with no coupling to the bootstrap. `-Xes6-proxy` is
still fine for the cases that only *record* and `--dump`, which is what
`test/bundle-vm-options.js` uses it for.

**`-Xasync-generators` reaches only `eval` and `new Function`.** A top-level
async generator in a script file is compiled through the NAPI compile path
(`hermes_napi_compile.cpp` / `hermes_napi.cpp`), which hardcodes
`compileFlags.enableAsyncGenerators = true` regardless of `RuntimeConfig`.
So a test that checks the flag through a script file sees no difference and
concludes, wrongly, that the flag is inert. `JSLib/eval.cpp` is different:
it sets the compile flag from `runtime.hasAsyncGenerators()`, the live
`RuntimeConfig` field, which is where `test/vm-options.js` asserts it.

**`-gc-print-stats` and `-track-io` set a real bit and report nothing.**
Both were honoured until the final review. Each configures a genuine field
-- `GCConfig::shouldRecordStats`, `RuntimeConfig::trackIO` -- so both parse
cleanly and cost real work: `GCBase` starts recording statistics, `Runtime`
attaches a page-access tracker. But the only readers are
`Runtime::printHeapStats` and `Runtime::getIOTrackingInfoJSON`, reached from
`ConsoleHost` and from `jsi::Instrumentation`, neither of which this runtime
uses. They are now refused alongside `-sample-profiling`, which the design
already refused for exactly this reason.

### The invariant, and the four divergences that produced it

The mapping applies a field **if and only if the caller named that flag**
(`getNumOccurrences()`), never unconditionally and never by comparing the
flag's value against an expected default. The first implementation did the
latter -- audit which of Hermes's `cl::init(...)` values already agree with
`RuntimeConfig`'s and `GCConfig`'s, and apply the rest unconditionally --
and four fields diverged:

- **`GCSanitizeRate` and `GCSanitizeRandomSeed`.** A handle-sanitizer build
  defaults the rate to 0.01, where `GCSanitizeConfig`'s own bare default is
  0.0. That silently turned on 1% random handle sanitization for every plain
  run under ASAN, and broke two unrelated tests that assert exact process
  output.
- **`AsyncBreakCheckInEval`.** `cl::init` false against `RuntimeConfig`'s
  true, so it flipped in every build.
- **`VerifyEvalIR`.** `cl::init` true under `HERMES_SLOW_DEBUG` against
  `RuntimeConfig`'s false -- the opposite mismatch, still a mismatch.

`-gc-init-heap` and `-gc-max-heap` need the gating for a second, independent
reason: their `MemorySize` type has no `OptionValue` specialization, so
`Option::setDefault()` is a silent no-op for them and the gating is the
*only* thing standing between an earlier call's parsed value and this call's
output.

`VmOptionsTest.EmptyOptionsMatchHardcodedBuilderFieldByField` pins the whole
property; all four would have failed it. What the rule really guards against
is a future Hermes `cl::init` change silently adding a fifth, with no edit
to this code at all.

## What the whole-branch review caught that no task review could

**An unintended GC behaviour change, arriving inside a bug fix.** The
mapping had grown an unconditional
`gcBuilder.withShouldReleaseUnused(kReleaseUnusedNone)`, copied from
`hermes.cpp:93`. No `--vm=` flag names that field, so nothing gated it, and
`GCConfig::Builder`'s own default is `kReleaseUnusedOld` -- what the
hardcoded three-call builder this feature replaced produced, since it never
touched `GCConfig` at all. Every plain `hermes-node app.js` therefore
stopped returning old-generation memory to the OS.

It is worth recording *how* it got in, because the reasoning was locally
sound. It arrived framed as a mapping-fidelity fix ("hermes.cpp is the
stated authority"), and that framing answered the wrong question: which
source function to copy, rather than whether hermes-node's behaviour should
change. The task review saw a field brought into agreement with the stated
reference. Only a pass over the whole branch, holding the "empty options are
identical to the hardcoded builder" invariant in view, saw that the
agreement was itself the regression -- and that the unit test had been
edited to *document* the divergence rather than to fail on it.

The fix deletes the call. The mapping's job is to honour flags the caller
passed, not to import every constant the `hermes` binary picks for itself,
and the invariant is now true with no exception.

**A silent no-op on a VM setting, in the one place the branch was not
looking.** `HERMES_NODE_VM_OPTIONS` was read only inside the `--bundle`
branch, so `HERMES_NODE_VM_OPTIONS=-gc-max-heap=4g hermes-node app.js` did
nothing and said nothing. The argument for the restriction explained why the
variable is redundant for a plain script (which already has `--vm=`), not
why setting it should be inert -- and this branch's own comments invoke that
exact failure shape three times over as the reason for refusing rather than
ignoring. It is now honoured in every mode that runs a program, with the
environment applied before `--vm=` so an explicit flag still wins.
`--build-bundle` stays out, and for a different reason: there `--vm=` is
recorded into the container, so folding the environment in would bake a
build machine's ambient variable into a shipped artifact.

**`--dump` could not audit the most open container there is.** The
`VM_OPTIONS` section printed only when the container recorded options, so an
artifact built with `--allow-vm-options-override` and no `--vm=` -- one that
honours `HERMES_NODE_VM_OPTIONS` unconditionally, `-enable-eval=true` and
`-Xhermes-internal-test-methods=true` included -- dumped with nothing said
about VM configuration at all. It now prints on options **or** the override
bit.

**Nothing pinned the flag tables against Hermes.** The feature's stated
value is that spellings come from Hermes and cannot drift, and
`HelpTextNamesEveryHonouredFlag` passed for a name Hermes does not register:
`vmOptionsHelpText()` prints `"  -" << name` unconditionally and only the
*description* comes from the registry. Both lists are now resolved against
`llvh::cl::getRegisteredOptions()`. Two refused names are exempt and
documented as such: `Xperf-prof` and `Xperf-prof-dir` sit inside `#ifdef
HERMES_ENABLE_PERF_PROF` (`RuntimeFlags.h:261`), which no configuration this
repo builds defines.

**A compile-define mismatch that is benign only by accident.**
`vm_options.cpp` includes `hermes/VM/RuntimeFlags.h`, which drags in
`ConsoleHost.h` and through it `Runtime.h`, whose member layout branches on
`HERMES_CHECK_NATIVE_STACK` and `HERMES_MEMORY_INSTRUMENTATION`. Hermes sets
both with a directory-scoped `add_definitions()` that does not reach
`lib/vm-options/`, so this TU saw a different `vm::Runtime` than every
Hermes TU -- confirmed in `build.ninja`, where `vm_options.cpp.o` carried no
`DEFINES` line at all. Nothing in the TU odr-uses a layout-dependent entity
today, which is a property nothing enforces: the next edit would break it
with wrong field offsets rather than a diagnostic. That is precisely the
hazard the design cites as its reason for refusing `ConsoleHost` flags in
the first place. `lib/vm-options/CMakeLists.txt` now propagates both from
the same CMake variables Hermes tests.
