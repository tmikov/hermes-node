# Design: Hermes VM options on the hermes-node command line

**Status:** Draft for review, 2026-09-02.

Adds a way to configure the Hermes VM from the hermes-node command line, the
way the `hermes` binary already allows, and a way for an AOT container to
carry the configuration its program needs.

Touches the AOT bundle format, so it follows
`docs/superpowers/specs/2026-08-19-closed-world-bundle-design.md` (format v2),
`docs/superpowers/specs/2026-08-20-bundle-preload-design.md` (v3) and
`docs/superpowers/specs/2026-08-21-bundle-natives-design.md` (v4). This is v5.

## The problem

hermes-node's VM configuration is a constant. There is exactly one
`RuntimeConfig` in the tree, at `lib/runtime/hermes_node_runtime.cpp:691`:

```cpp
auto rtConfig = hermes::vm::RuntimeConfig::Builder()
                    .withMicrotaskQueue(true)
                    .withEnableAsyncGenerators(true)
                    .withES6BlockScoping(true)
                    .build();
```

Nothing on the command line can reach it. There is no way to raise the heap
limit for a program that needs one, no way to turn the JIT on, no way to
disable `eval` for a program that should not have it, and no way to run
hermes-node with `-Xhermes-internal-test-methods` while debugging the VM
underneath it. The `hermes` binary can do all of these; hermes-node, which
embeds the same VM, cannot.

The second half of the problem is distribution. `--build-bundle` produces a
container and `--build-exe` links it into an executable, and a program's VM
requirements -- a heap ceiling, say -- are a property of that program, not of
whoever launches it. A produced executable in particular has no command line
of its own to put them on: `bundle_main.cpp` pushes `argv[0]` twice and hands
everything else to the program, deliberately, so that `process.argv.slice(2)`
means what it means under `--bundle=<f> arg`.

## The decision

Four decisions, each of which the rest of this document elaborates.

1. **Hermes parses the flags, behind an explicit envelope.** `--vm=<flag>`,
   repeatable. The value is a Hermes flag spelled exactly as the `hermes`
   binary spells it, and `hermes::cli::RuntimeFlags` plus `llvh::cl` do the
   parsing. hermes-node's own flag namespace stays closed.
2. **Only the flags whose effect hermes-node can actually deliver are
   accepted.** Twenty-five are honoured; thirteen whose effect the `hermes`
   binary delivers through `ConsoleHost` are refused by name, because
   accepting them would be a silent no-op.
3. **A container can carry VM options.** Format v5 gains a VM-options section
   and a flag bit. `--build-bundle --vm=...` records them; `--bundle=` and any
   executable linked from that container inherit them; `--dump` prints them.
4. **Baked options are locked by default.** `--build-bundle
   --allow-vm-options-override` opens them to `--vm=` on the command line and
   to `HERMES_NODE_VM_OPTIONS` in the environment.

## Why delegate the parsing

Hermes already has the whole apparatus. `hermes::cli::RuntimeFlags`
(`hermes/include/hermes/VM/RuntimeFlags.h`) is a struct of `llvh::cl::opt`
members that registers every VM flag when instantiated, with Hermes's own
descriptions, defaults and value parsers -- `MemorySizeParser` for `1g` and
`512m`, `RandomSeedParser`, the `on|force|off` enum behind `-Xjit`. Writing
our own subset would mean hand-rolling those parsers and then watching the
spellings drift from the binary they are supposed to match.

Three facts make delegation safe here, and each was checked rather than
assumed.

**The option registry is empty.** `llvh::cl` keeps its registry in process
globals, so a second registration of the same option name aborts. In the
built binary (`nm -C`, ASAN build, 2026-09-02) 161 `cl::opt` symbols are
present and every one belongs to LLVM's own internal options (`-debug`,
`-debug-buffer-size`, `-track-memory`). Neither `hermes::cli::RuntimeFlags`
nor `hermes::CompilerRuntimeFlags` is linked, and neither is the
`hermes::cl::compilerRuntimeFlags` global that
`hermes/lib/CompilerDriver/CompilerDriver.cpp:351` defines -- hermes-node
compiles JavaScript through the Hermes runtime API, not through
`CompilerDriver`. **This becomes a standing constraint: hermes-node must not
link `hermesCompilerDriver`.** If it ever does, both that global and our
`RuntimeFlags` instance will try to register `-Xes6-block-scoping` and the
process will abort at static-initialisation time.

**Parse errors can be caught rather than exited on.**
`cl::ParseCommandLineOptions` "will print the error message to stderr and
exit if `Errs` is not set, or print the error message to `Errs` and return
false if `Errs` is provided"
(`hermes/external/llvh/include/llvh/Support/CommandLine.h:55-61`). Passing an
`Errs` stream is therefore load-bearing: it is what keeps a bad `--vm=` value
from terminating the process out from under our own error reporting.

**A synthesized argv keeps the two parsers apart.** hermes-node's parse loop
(`tools/hermes-node/hermes-node.cpp:575`) is hand-written and stays that way.
It collects `--vm=` values into a vector; nothing else is ever handed to
`llvh::cl`. So an unknown `--buidl-bundle` is still our error with our
message, and a Hermes flag never has to be disambiguated against one of ours.

### Two things llvh::cl does not do, found during implementation

Both were assumed away in the first draft of this document and are recorded
here because the code now works around them.

**A repeated flag is an error, not an override.** A plain `llvh::cl::opt`
defaults to `NumOccurrencesFlag::Optional`, and a second occurrence is
rejected with "may only occur zero or one times!"
(`CommandLine.cpp:1440-1442`). None of the VM options declares
`cl::ZeroOrMore`. So "later wins" is *not* free from the parser, as this
document originally claimed: `buildVmRuntimeConfig` deduplicates the option
list by flag name, keeping each flag's last occurrence, before handing it to
`llvh::cl`. That is merge logic of ours, small and tested. It matters
because the whole override story -- container options, then the
environment, then `--vm=` -- is a chain of repeated flags where the last
must win.

**A per-option error never reaches the `Errs` stream.** Passing `Errs` to
`ParseCommandLineOptions` is still load-bearing, but for one thing only: it
makes the function return `false` instead of calling `exit()`. The stream
itself receives only the errors that function prints itself (unknown
argument, positional-count problems). A bad *value* goes through
`Option::error()`, whose `Errs` parameter defaults to `llvh::errs()` -- real
process stderr -- and every internal call site takes that default
(`CommandLine.h:366`, `CommandLine.cpp:1422-1432`). So the user does see a
precise message, on stderr, written by llvh; we cannot capture it, and
`buildVmRuntimeConfig` falls back to a generic non-empty error string when
the captured text is empty. The consequence worth knowing: for a bad value
the process prints llvh's message *and* ours.

### Spelling

`--vm=<flag>`, repeatable, **one Hermes flag per occurrence, never split on
whitespace**. Splitting would corrupt any value containing a space, and
`-Xperf-prof-dir=<dir>` is proof that such values exist even inside this flag
set.

`HERMES_NODE_VM_OPTIONS` is a single string split on whitespace, because an
environment variable has no other shape. Values containing spaces cannot be
expressed there. That is a documented limitation of the environment variable,
not of the feature: `--vm=` has no such limit.

It is honoured in every mode that *runs* a program -- a plain script, `-e`,
the REPL, `--bundle=` and a linked executable -- and everywhere it is
applied before `--vm=`, so an explicit flag on the command line wins.
`--build-bundle` is the exception, because there `--vm=` is recorded into
the container rather than applied to anything: folding the environment in
would bake a build machine's ambient variable into a shipped artifact.

`--vm-help` prints the honoured flags with Hermes's own `cl::desc` text, read
out of the registry so the help cannot drift from what is accepted.

## Which flags are honoured

The dividing line is not editorial. It is whether the `hermes` binary
implements the flag by putting a value into `RuntimeConfig`, or by calling a
method on a live `vm::Runtime`.

`hermes/tools/hermes/hermes.cpp:110-141` builds the `RuntimeConfig`; the
lines after it fill in an `ExecuteOptions`, which is a `ConsoleHost` concept.
`ConsoleHost` then applies those by reaching into the runtime:

```cpp
runtime->getJITContext().setDumpJITCode(options.dumpJITCode);
runtime->getJITContext().setCrashOnError(options.jitCrashOnError);
runtime->getJITContext().setEmitAsserts(options.jitEmitAsserts);
runtime->getJITContext().setEmitCounters(options.jitEmitCounters);
runtime->getJITContext().setHCIdLimit(options.jitHCIdLimit);
```

(`hermes/lib/ConsoleHost/ConsoleHost.cpp:967-971`)

hermes-node does not use `ConsoleHost`, and more to the point it cannot reach
`vm::Runtime` at all. The comment at `lib/runtime/hermes_node_runtime.cpp:718`
records why: those headers' struct layout depends on private compile defines
(`HERMES_MEMORY_INSTRUMENTATION`, `HERMES_CHECK_NATIVE_STACK`) that Hermes's
CMake leaks only within its own subdirectory scope, so including them here
would silently miscompute field offsets and corrupt unrelated runtime state.
Everything in this codebase goes through JSI or the `IHermes` interfaces
instead. That is a deliberate boundary, and reaching across it to implement a
handful of JIT development flags is not a trade worth making.

**Honoured (25).** Each maps to a `RuntimeConfig` or `GCConfig` field, using
the same mapping `hermes.cpp` uses:

| Flag | Goes to |
| --- | --- |
| `-gc-init-heap`, `-gc-max-heap`, `-occupancy-target` | `GCConfig` sizing |
| `-gc-sanitize-handles`, `-gc-sanitize-handles-random-seed` | `GCSanitizeConfig` |
| `-gc-alloc-young`, `-gc-revert-to-yg-at-tti` | `GCConfig` |
| `-max-register-stack` | `withMaxNumRegisters` |
| `-Xjit[=on\|force\|off]` | `withEnableJIT` + `withForceJIT` |
| `-Xjit-threshold`, `-Xjit-memory-limit` | `withJITThreshold`, `withJITMemoryLimit` |
| `-Xes6-proxy`, `-Xes6-block-scoping`, `-Xasync-generators`, `-Xintl` | the matching `with...` |
| `-Xmicrotask-queue`, `-Xvm-experiment-flags`, `-Xrandomize-memory-layout` | the matching `with...` |
| `-enable-hermes-internal`, `-Xhermes-internal-test-methods` | the matching `with...` |
| `-enable-eval`, `-verify-ir`, `-optimized-eval`, `-emit-async-break-check` | eval-related fields |
| `-test262` | `withTest262` |

**Refused (13).** `-sample-profiling`, `-sample-profiling-freq`,
`-gc-print-stats`, `-track-io`, `-stop-after-module-init`,
`-Xheap-timeline`, `-Xdump-jitcode`, `-Xperf-prof`, `-Xperf-prof-dir`,
`-Xjit-crash-on-error`, `-Xjit-emit-asserts`, `-Xjit-emit-counters`,
`-Xjit-hc-id-limit`.

Three of those deserve their own note, because they are the ones that look
like they should work. Each *does* set a real `RuntimeConfig` or `GCConfig`
bit, so accepting it would parse and configure cleanly -- and each is
refused anyway, because the only reader of what that bit produces lives in
`ConsoleHost`, so the result would be real work done and nothing reported:

- `-sample-profiling` sets `withEnableSampleProfiling`; the half that writes
  the profile out is `ConsoleHost.cpp:1094-1112`, so it would give a
  profiler that runs and reports nowhere.
- `-gc-print-stats` sets `GCConfig::shouldRecordStats`, which makes `GCBase`
  record statistics (`GCBase.cpp:51`) that only `Runtime::printHeapStats`
  prints -- reached from `ConsoleHost.cpp:1137`, from
  `jsi::Instrumentation::getRecordedGCStats` and from Android platform
  logging, none of which this runtime uses. It is the worst of the three to
  accept in silence, because its name promises printed output.
- `-track-io` sets `RuntimeConfig::trackIO`, which makes `Runtime` attach a
  page-access tracker (`Runtime.cpp:1191`) that only
  `Runtime::getIOTrackingInfoJSON` reports -- reached from
  `ConsoleHost.cpp:1140` and from `jsi::Instrumentation`, neither of which
  this runtime uses.

Supporting any of the three means deciding where its output goes, which is a
feature in its own right and out of scope here. Sampling profiler support in
particular is buildable later without reaching into VM internals, since
`facebook::hermes::HermesRuntime` exposes a public API for it.

Refusing rather than ignoring is the whole point. A flag accepted and
silently dropped is the failure mode this codebase has spent several rounds
removing -- the `process.exitCode` that nothing could reach, the uncaught
exception that printed and exited 0. A refusal names the flag and says why:

```
Error: --vm: '-Xdump-jitcode' is not supported by hermes-node.
       It is implemented by Hermes's ConsoleHost, which this runtime
       does not use. Run '--vm-help' for the flags that are supported.
```

The allowlist has a second job: it keeps `--vm=-help` and `--vm=-version`
from reaching `llvh::cl`'s help printer, which would dump the entire
registry, LLVM's internal options included, and exit.

## How the options reach the runtime

### Do not call `hermes::cli::buildRuntimeConfig()`

There is a ready-made `hermes::cli::buildRuntimeConfig(const RuntimeFlags &)`
in `hermes/lib/VM/RuntimeFlags.cpp`, and it is the wrong function to call. It
omits `ES6BlockScoping`, `EnableAsyncGenerators`, `Test262` and every JIT
field. The `hermes` binary does not use it either -- it builds its config
inline at `hermes.cpp:110-141`, which is the complete mapping. Ours mirrors
that inline mapping, and the reference is recorded in a comment so the two
can be diffed when Hermes moves.

### Preserve hermes-node's defaults

This is the sharp edge of the whole feature. Hermes's flag defaults are not
hermes-node's defaults:

```cpp
llvh::cl::opt<bool> ES6BlockScoping{
    "Xes6-block-scoping", llvh::cl::init(false), ...};
llvh::cl::opt<bool> EnableAsyncGenerators{
    "Xasync-generators", llvh::cl::init(false), ...};
```

(`hermes/include/hermes/Utils/CompilerRuntimeFlags.h:46,53`)

Both default to `false`, and hermes-node forces both to `true` today -- async
generators are listed in CLAUDE.md as a supported feature precisely because
that flag is on. Building the config from the parsed flags without care would
turn both off for every user, with no flag passed and no message printed.

So for every field where hermes-node has an opinion, the value is chosen by
whether the user actually said something:

```cpp
bool blockScoping = flags.ES6BlockScoping.getNumOccurrences()
    ? (bool)flags.ES6BlockScoping
    : kHermesNodeDefaultES6BlockScoping;  // true
```

`getNumOccurrences()` is the test rather than assigning defaults into the
option objects before parsing, because it does not depend on whether
`ParseCommandLineOptions` resets option values -- it states the intent
directly, and it reads as what it is.

The three fields with hermes-node defaults are `ES6BlockScoping` (true),
`EnableAsyncGenerators` (true) and `MicrotaskQueue` (true; Hermes's own
default is already true, so this one is belt-and-braces and documents the
requirement). Every other field takes Hermes's default.

### Parse once, share with both runtimes

`llvh::cl`'s registry is global and its parse accumulates, so the parse must
happen exactly once per process. But hermes-node starts *two* runtimes when
`--inspect` is given, and the inspector runtime must be configured
identically -- a debugger attached to a differently-configured VM would be
misleading in a way that is hard to notice.

`HermesNodeProcessConfig` is exactly the right home; its doc comment already
says so ("put a setting here only if every runtime in the process should
share it"), and the compile-cache settings are there for the same reason.
It gains one field:

```cpp
/// Hermes VM options, in the order they take effect: the container's
/// baked options first, then HERMES_NODE_VM_OPTIONS, then --vm= from the
/// command line. Later occurrences of the same flag win, which is
/// last-wins, which buildVmRuntimeConfig implements by deduplicating.
std::vector<std::string> vmOptions;
```

Strings rather than a built `vm::RuntimeConfig`, so the public header stays
free of Hermes VM headers. `lib/runtime` parses them under `std::call_once`
and caches the resulting config; the inspector runtime, receiving a copy of
the same process config, gets the same configuration by construction.

## Format v5: a container carries its VM options

`kBundleFormatVersion` goes 4 -> 5 (`include/hermes/node-compat/bundle/bundle_format.h:22`).

The header gains a VM-options section -- an offset and a count into the
existing string table, the same shape the preload table and the native table
already use -- and one flag bit, `kBundleFlagAllowVmOptionsOverride`.

`--dump` gains a section, so a container's VM configuration can be audited
before it ships, in the same spirit as `--verify-natives`:

```
VM_OPTIONS  (overrides: locked)
  -gc-max-heap=2g
  -Xjit=on
```

The section is printed when the container records options **or** has the
override bit set -- nearly the rule `NATIVES` follows, with the one addition
that has no `NATIVES` equivalent: the bit is VM configuration even when no
options accompany it. A container built with `--allow-vm-options-override`
and no `--vm=` honours `HERMES_NODE_VM_OPTIONS` unconditionally,
`-enable-eval=true` and `-Xhermes-internal-test-methods=true` included, and
that is the most important single fact an audit before shipping can
surface. Section sizes gain a `vm_options` row.

As elsewhere, `open()` keeps its hard version-mismatch error so nothing on
the run path can reach a v4 container, and `openForInspection` keeps
reporting the mismatch rather than enforcing it.

### Reading the options before the runtime exists

The container's options decide how the runtime is built, but today the
container is opened *after* the runtime is created -- `runBundle()` and
`runEmbeddedBundle()` (`lib/runtime/hermes_node_runtime.cpp:656,668`) both
take a `napi_env`.

Rather than restructure the run path, the bundle layer gains two small
functions that open for inspection, read the options and the flag bit, and
close:

```cpp
bool readBundleVmOptions(const std::string &path, BundleVmOptions *out, std::string *error);
bool readEmbeddedBundleVmOptions(const uint8_t *data, size_t size, BundleVmOptions *out, std::string *error);
```

`main()` calls the first when `--bundle=` is given; `bundle_main.cpp` calls
the second. Both run before `runHermesNode`, and both cost one extra
map-and-validate of a file that is about to be mapped again -- cheap, and it
leaves the run layer's structure alone.

Note what this does **not** change: `--build-exe` reads a container and links
it, and the options travel inside the container's bytes. No new symbol in the
generated payload object, no change to `bundle_main.cpp`'s link
configuration, and `--build-exe` gains no flag of its own.

## Override policy

**Locked by default.** A container built with `--vm=` records its options and
refuses to have them changed at run time. `--build-bundle
--allow-vm-options-override` sets the flag bit and opens them.

Locked is the default because the flag set includes `-enable-eval` and
`-Xhermes-internal-test-methods`, which are not tuning knobs. An artifact
that always honours its environment lets whoever controls the environment
turn those back on.

**Unlocked, later wins.** Run-time options are appended after the container's,
and the later occurrence of a repeated flag is the one that takes effect.
Order of application:

1. the container's baked options
2. `HERMES_NODE_VM_OPTIONS`
3. `--vm=` from the command line

so an explicit flag beats the environment.

**Locked meets an override: error.** Both `--vm=` and a **non-empty**
`HERMES_NODE_VM_OPTIONS` are refused, by name. Non-empty rather than merely
set, because `splitVmOptionsEnv` returns an empty list for an empty or
all-whitespace value: there is nothing to apply, so there is nothing to
refuse, and failing on `HERMES_NODE_VM_OPTIONS=""` would report an override
that was never attempted.

```
Error: this bundle's VM options are locked and cannot be overridden.
       HERMES_NODE_VM_OPTIONS is set in the environment.
       Rebuild with: --build-bundle --allow-vm-options-override
```

The alternative -- ignoring the environment variable when locked -- was
considered and rejected. It is quieter for someone who has the variable
exported globally for a development hermes-node and then runs a locked
executable, and that is a real cost. But it is a silent no-op on a VM
setting, which is the exact failure shape this codebase keeps removing, and
the escape hatch is a build-time flag that the person shipping the artifact
controls. Loud and wrong-for-one-workflow beats quiet and wrong-for-everyone.

## Flag conflicts

New rows in `checkToolOptions()` (`tools/hermes-node/hermes-node.cpp:188`),
all of them after the parse loop so that flag order never matters, each
naming both flags:

| Refused | Because |
| --- | --- |
| `--vm` with `--build-exe` | the container carries the options; this is the wrong place to say it |
| `--vm` with `--dump`, `--extract-module`, `--verify-natives`, `--dump-bytecode` | those verbs run before any runtime exists |
| `--allow-vm-options-override` without `--build-bundle` | it records a bit in a container this run is not building |
| an empty `--vm=` | names the flag, rather than reporting a parse failure with nothing in it |

`--vm` with `--build-bundle` is **allowed** and means "record these in the
container". It does not configure the producer's own runtime; the producer
compiles rather than runs, and a build machine's VM tuning is not the
artifact's business. This is stated in the help text because it is the one
place the flag means something other than "configure this run".

`--vm` with `--inspect` is allowed. `-Xjit` and a debugger are a combination
worth being able to try.

## Testing

The property that makes this testable is that several honoured flags are
observable from JavaScript with no timing dependency:

| Flag | Observable as |
| --- | --- |
| `-Xes6-proxy=false` | `typeof Proxy === 'undefined'` |
| `-Xintl=false` | `typeof Intl === 'undefined'` |
| `-enable-eval=false` | `new Function(...)` throws |
| `-Xhermes-internal-test-methods=true` | the test methods appear on `HermesInternal` |
| `-Xasync-generators=false` | an async generator fails to compile |

So the tests assert effect, not merely that a flag parsed.

- `test/vm-options.js` -- honoured flags take effect; hermes-node's defaults
  survive when no flag is passed (the async-generator and block-scoping
  regression this design exists to avoid); `--vm=` ordering, last wins.
- `test/vm-options-errors.js` -- each refused flag by name, an unknown flag,
  an empty `--vm=`, and every row of the conflict matrix. Not gated on
  anything: none of it needs a linker or a kit.
- `test/bundle-vm-options.js` -- build with `--vm=`, `--dump` shows the
  section, running the container applies them; locked refuses `--vm=` and
  refuses `HERMES_NODE_VM_OPTIONS`; `--allow-vm-options-override` accepts
  both, with the run-time value winning.
- `test/build-exe-vm-options.js` -- an executable inherits its container's
  options, and honours or refuses `HERMES_NODE_VM_OPTIONS` according to the
  bit. `REQUIRES: linker-available`, like the other three `build-exe` tests.
- `BundleFormatTest` -- v5 round-trip, with and without options, both states
  of the flag bit; a v4 container is rejected by `open()` and reported by
  `openForInspection()`.
- A GTest for the flag classifier and the parse: every honoured name maps
  where the table says, every refused name is refused, and the
  `getNumOccurrences()` default preservation holds for the three fields that
  have hermes-node defaults.

## What this does not do

- **No sampling profiler.** See above; it needs somewhere for the output to
  go, which is a design question of its own.
- **No JIT development flags.** `-Xdump-jitcode` and the four beside it need
  `vm::Runtime`, which this codebase does not touch. If they are ever wanted,
  the honest route is an ABI-stable side interface on the Hermes side, in the
  shape of `ICancelAsyncTimeout` -- not an `#include` of a VM-internal header.
- **No per-flag validation of combinations.** `-Xmicrotask-queue=false` will
  break promise semantics and hermes-node will let you do it. These are
  development flags and the `hermes` binary does not second-guess them
  either. What hermes-node needs to run correctly is a *default*, not a
  constraint.
- **The compile-cache generation tag does not fold VM options.** Bytecode
  does not depend on them -- the honoured flags configure the VM, and the two
  compile-affecting ones (`-Xes6-block-scoping`, `-Xasync-generators`) reach
  only `eval` and `new Function`, whose results are never cached. A container
  built under one set of options runs correctly under another.
