# Design: pre-compiling the built-in JavaScript to native code

**Status:** Draft, revised after external review, 2026-09-14.

`hermes-node build-native` (design
`docs/superpowers/specs/2026-09-13-native-compilation-design.md`) compiles a
program's whole `require()` graph to Static Hermes compilation units and
links a standalone executable. It deliberately stopped at the program's own
modules: the **built-in** JavaScript -- `libjs/`, `libjs-node/`, the shims
under `libjs/shims/` and the vendored packages -- stays embedded Hermes
bytecode and stays interpreted. That was phase 1's largest listed gap.

This closes it. The kit gains a second, pre-compiled copy of every built-in
module as native code, and a `build-native` artifact links that copy instead
of the bytecode registry.

## What "fully native" does and does not mean here

The goal is a binary that interprets none of the JavaScript it was built
from. That qualification is load-bearing in two directions, and an external
review found the first draft's unqualified claim false in both: `eval` and
`new Function` compile at run time, so a program that calls them still
executes interpreted code no packaging decision can reach, and Hermes runs
some JavaScript of its own that is not the program's at all.

Four bodies of JavaScript can execute in a `build-native` artifact today:

| | Size | After this design |
|---|---|---|
| The program's own modules | varies | native (phase 1) |
| hermes-node's built-ins, 187 modules | 2,111,177 B of bytecode | **native (this design)** |
| Hermes's `InternalJavaScript` | one bytecode unit | **native (this design)** |
| Hermes's JSI `ExtensionsBytecode` | 1,680 B of bytecode | **still interpreted** |

`ExtensionsBytecode.hbc` installs `TextEncoder` and friends. It is reached
because `lib/runtime/hermes_node_runtime.cpp:813` builds the runtime through
`facebook::hermes::makeHermesRuntime`, which calls
`loadAndInstallExtensions` (`hermes/API/hermes/hermes.cpp:1557`) under
`HERMES_ENABLE_CORE_EXTENSIONS`. Turning that option off would remove the
interpretation and the functionality together, which is not a trade this
design makes. Native-compiling that unit is a Hermes change of the same
shape as `hermesInternalUnit`, and is filed in the tracker rather than done
here.

So the honest claim, and the one the tests assert, is: **every line of
statically packaged JavaScript -- the program's own modules and hermes-node's
built-ins -- is native, and the only interpreted JavaScript bytecode shipped
in the artifact is Hermes's own 1,680-byte extensions unit, run once at
runtime creation.** JavaScript bytecode specifically: a container built with
`--bake-wasm` also ships Hermes bytecode, compiled from WebAssembly rather
than from JavaScript, which is why the magic-count test below uses a fixture
that bakes none. Code the program manufactures at run time through `eval` or
`new Function` is outside that claim and always will be.

This is measurable rather than rhetorical. Counting the Hermes bytecode magic
(`0x1F1903C103BC1FC6`) in the current `hermes-node` gives **189** = 187
embedded modules + `InternalJavaScript` + `ExtensionsBytecode`. With
`HERMESVM_INTERNAL_JAVASCRIPT_NATIVE=ON` it is **188**, confirming there is
no stray copy of the constant in read-only data to confuse the count. A
fully native artifact with no baked Wasm should therefore contain **exactly
one per architecture slice**, and that is what the test asserts -- an exact
number, not a bound. Per slice, because a universal Mach-O carries a complete
linked image for each architecture and therefore one `ExtensionsBytecode`
blob each; release CI builds `x86_64;arm64`
(`.github/workflows/release.yml:316`), and `test/build-exe.js:101` already
counts architectures for exactly this reason.

## Feasibility, measured before the design was written

Everything below rests on measurements taken on macOS arm64 against
`cmake-build-release`, not on reasoning about what ought to work.

**All 187 built-in modules compile.** 180 CommonJS-wrapped modules (161 from
the manifest plus 19 vendored files) and 7 unwrapped ones (the six
`@bootstrap` modules plus the generated `vendored-packages`) went through
`shermes -emit-c` and `cc` -- every one of the 187, individually, not a
sample -- with **zero failures**. This was the feature's main risk: these are
Node's own `lib/*.js` files, and a single module the Static Hermes frontend
rejected would have forced a mixed native/bytecode registry and a much
larger design.

**The compile costs 5.4 s wall at `-j16`** (47 s CPU) for the 180 wrapped
modules. `cc` dominates; `shermes` itself is 0.02-0.25 s per module.

**Two probe links suggest the units cost somewhere between 9.35 MB and
12.03 MB, and neither is the answer.** They bias in opposite directions. Against a baseline probe referencing one of the 187
(`path`), adding the rest gives 9,353,200 bytes -- but that is **186** units,
not 187, and the baseline has already paid for whatever runtime `path` pulls
in. Against a baseline referencing a trivial unit outside the set, adding all
187 gives 12,031,744 bytes -- now all 187, but the figure also absorbs the
runtime the built-ins pull that a one-line script does not.

The number that actually matters is a real artifact's growth when its
built-ins switch from bytecode to native, where the runtime archive already
participates in the link either way. Nothing here *bounds* that number -- dead stripping and
which Static Hermes runtime helpers a program has already pulled both vary by
program -- so the range above is an estimate to expect, not an interval the
result is known to fall in. It **cannot be measured before the archive
exists**, so measuring it is an explicit step of the implementation rather
than a figure asserted here. Against the **2,111,177 bytes** of embedded
bytecode it replaces (the sum of the `.hbc` file sizes; an earlier draft said
2.4 MB, which was `du` block-rounding), that puts the expected net around
**+7.2 MB to +9.9 MB**.
`HERMESVM_INTERNAL_JAVASCRIPT_NATIVE=ON` adds a further **+338,960 bytes**,
measured exactly on `hermes-node` itself. Relative growth shrinks on real
programs -- ditz2's 137 modules and tetris's 22 already carry more.

**A bootstrap module can be served from a native unit during early boot.**
This was the one thing the design could not argue its way to, because every
native unit that exists today initializes on first `require()`, long after
the runtime is up, while `primordials` is nearly the first JavaScript to
run. Proven by spike: `libjs/primordials.js` was compiled to a unit, linked
into `hermes-node`, and `runEmbeddedModule` was temporarily patched to serve
it through `hermes_init_sh_unit`. The bootstrap came up, `globalThis.
primordials` was populated, and a sample of the suite -- `test-path.js`,
`test-buffer.js`, `test-events.js` and twelve node-ported child-process tests
-- passed. (The single apparent failure was the spike's own `fprintf` to
stderr polluting a test that compares a child's captured stderr exactly.) The
spike was reverted.

**`HERMESVM_INTERNAL_JAVASCRIPT_NATIVE=ON` works and is nearly free.**
Flipped in `cmake-build-release` and rebuilt: `hermes-node` runs, sample
tests pass, the magic count drops by exactly one, and the binary grows
338,960 bytes. `hermesInternalUnit` is **already built** in every non-MSVC
configuration -- Hermes builds it unconditionally "to prevent bitrot"
(`hermes/lib/InternalJavaScript/CMakeLists.txt:55-59`) and only the alias
selection at :98-103 is gated -- so this is an option flip over an archive
that already exists, not new compilation work. The flip was reverted.

## The seam: one struct field

`embedded_modules_registry.cpp.o` exports exactly one global symbol,
`findEmbeddedModule`, and references exactly one external symbol, `strcmp`.
Nothing else in the merged kit archive references anything defined in it;
only `embedded_modules.cpp.o` calls `findEmbeddedModule`. That is the whole
coupling between the registry and everything above it, and it is why this
design is small. (Independently confirmed with `nm -gU` during review.)

`EmbeddedModule` (`include/hermes/node-compat/embedded-modules/
embedded_modules.h`) grows one field:

```cpp
struct EmbeddedModule {
  const char *id;
  const uint8_t *data;   ///< Bytecode registry only; null in a native one.
  size_t size;
  SHUnitCreator creator; ///< Native registry only; null in a bytecode one.
  bool isBootstrap;
};
```

`embedded_modules.cpp` -- one dispatch file, in the merged kit archive,
identical in both configurations -- branches on `creator`:

```cpp
if (mod->creator)
  return hermes_init_sh_unit(env, mod->creator, result);
// ... existing hermes_run_bytecode path, unchanged
```

**The two calls return the same thing, which is what makes this a one-line
change rather than a new loading path.** For a CommonJS module wrapped in
`(function (exports, require, module, __filename, __dirname) { ... });`, the
unit's top-level completion value *is* the wrapper closure -- the same value
`hermes_run_bytecode` returns for the same wrapped source. For a bootstrap
module it is the file's completion value, again the same. So `libjs/
loader.js` is untouched, `runEmbeddedModule` keeps its contract, and
`loadBytecodeModuleCallback` keeps returning `undefined` for an unknown id.

One subtlety is already handled upstream and is worth recording because it
is not obvious: `libjs/loader.js:379` calls `loadBytecodeModule(request)` as
an **existence probe**, which means a module can be loaded twice.
`_sh_unit_init` handles a repeat call by discarding the duplicate `SHUnit`
and re-running the existing one's top level (`hermes/lib/VM/
StaticHUnit.cpp:140-144`), which is exactly what re-running bytecode does.
The probe therefore behaves identically. This is the same property that makes
`delete require.cache[...]` work for a natively compiled user module.

## Two registries, chosen by the linker

The native registry defines **the same symbol**, `findEmbeddedModule`, in its
own archive. `build-native` places that archive ahead of the merged kit
archive on the link line. The linker resolves the symbol from the native
archive, and `embedded_modules_registry.cpp.o` is then never pulled from the
merged archive -- nothing else references anything in it.

That last point is the payoff, and it is why the design does not simply add
a second table consulted first: **the 2.1 MB of built-in bytecode does not
enter the artifact at all.** A design that kept both would carry dead
bytecode in every native binary, with no way for the linker to know it was
dead.

### The failure mode this creates, and how it is closed

Relying on archive order alone would be silent. Get the order wrong and the
link succeeds, the program works, and it is interpreted -- a regression
nothing reports. This codebase has spent several rounds removing exactly
that shape (see the VM-options and uncaught-exception sections of
`CLAUDE.md`), so it is not acceptable here either.

So the native archive additionally defines a strong global
`hermesNodeNativeBuiltinsMarker`, in the **same archive member** as
`findEmbeddedModule`, and declared `extern "C"`. The linkage is not a detail:
the registry is generated as C++ inside `namespace hermes::node_compat`
(`tools/gen-embedded-registry.py`), so without `extern "C"` the definition is
mangled while `-u` asks for the unmangled name, and **every** native-builtins
link fails. `build-native` passes
`-Wl,-u,<prefix>hermesNodeNativeBuiltinsMarker` **before** the native
archive on the link line.

`-u` is the right instrument and it does three jobs at once: it makes the
symbol an undefined root, so the archive member is extracted; it makes that
member a garbage-collection root, so `-dead_strip` / `--gc-sections` cannot
discard it; and it fails the link by name if the archive is absent. Placing
it *before* the archive matters -- GNU `ld`'s archive scan is order-sensitive
-- and a wrong order fails loudly rather than silently. An earlier draft
instead emitted a reference "somewhere in the generated assembly", which
review correctly rejected: a reference in a discardable atom can be stripped,
re-opening the silent path.

If both archives were somehow pulled -- the merged one first, satisfying
`findEmbeddedModule`, then the native one pulled by `-u` --
`findEmbeddedModule` is a duplicate-symbol error. Loud in every direction.

**The one remaining silent case is `-u` not being emitted at all** while the
archive ordering happens to be right. Nothing in the link can catch that, so
it is caught earlier: a unit test asserts the generated link argv contains
the `-u` flag when native built-ins are selected and omits it under
`--bytecode-builtins`.

The platform symbol prefix currently exists only as a local inside
`payloadAssembly()` (`lib/build-exe/build_exe.cpp:478`); it becomes a small
shared helper keyed on `hostObjectFormat()`, so the `-u` flag and the
generated assembly cannot disagree about it.

### What this means for the bytecode path

`--build-exe` links the same merged archive the same way and selects the
bytecode registry exactly as it does today. No surgery on
`utils/make-kit.py`'s merge, and no new way for the merge to go wrong.

## The kit

A new CMake target, `hermesNodeBuiltinsNative`, builds the archive:

- For each of the 187 manifest entries, run `shermes -emit-c` over the
  **already-wrapped** file the bytecode pipeline produces in
  `lib/embedded-modules/wrapped/`, listing that file in `DEPENDS` so ninja
  sees one producer and two consumers. Reusing it rather than re-wrapping is
  deliberate: line and column parity with the bytecode build becomes
  structural instead of something a test has to assert. (`wrap-cjs.py`'s
  prefix is character-for-character `kCJSWrapperPrefix`, and it puts the
  wrapper header on the same line as source line 1 so line numbers match the
  original file.) Bootstrap modules are compiled unwrapped, as they are
  today.
- Name each unit from the module's safe id (`internal/errors` ->
  `internal__errors`), the same transform `gen-embedded-registry.py` already
  applies -- but **validate rather than assume**. The generator checks each
  derived name against `^[A-Za-z0-9_]+$`, which is exactly shermes's own rule
  (`hermes/include/hermes/Utils/Options.h:34`), and asserts global
  uniqueness, failing the build with the offending id. Both checks are load
  bearing: the transform replaces only `/` and `-`, so a vendored path
  containing `.` or `@` would be rejected by shermes, and `a-b` and `a_b`
  both map to `a_b`. Today's 187 ids are safe; nothing currently stops the
  188th from not being.
- Pass `-g2`. shermes defaults to `-g0`
  (`hermes/tools/shermes/shermes.cpp:136`), which emits no source locations
  at all, so reusing the identical wrapped text buys line and column parity
  only if the debug level asks for the table that carries it. Phase 1 passes
  `-g2` for user modules (`lib/build-native/native_compile.cpp:66`) and this
  matches it; the bytecode pipeline's equivalent is the `-g` already in
  `JS_COMPILER_FLAGS`. Getting this wrong would silently turn every built-in
  frame in a stack trace into `(native)` with no location.
- Pass `-source-name=<module id>`. For a wrapped module the trailing
  `//# sourceURL=<id>` that `wrap-cjs.py` appends remains authoritative
  (Hermes applies the magic comment during parsing), so `-source-name` is
  only a fallback there -- but it carries the same id, and for the seven
  unwrapped bootstrap inputs, which have no such comment, it is the only
  thing that names them. It also names parse-time diagnostics, which are
  emitted before a trailing comment has been seen.
- Compile the generated `.c` as sources of a `STATIC` CMake target rather
  than through hand-rolled `cc` invocations, with `-fno-strict-aliasing`,
  `-fno-strict-overflow`, `-w` and `gnu11` set explicitly. Those first two
  are required, not tuning: Static Hermes's generated C reads and writes C++
  objects through mirroring C structs, and Hermes appends them to
  `CMAKE_C_FLAGS` only inside its own subdirectory scope
  (`hermes/CMakeLists.txt:609-617`), which a top-level hermes-node target
  does not inherit. `gnu11` rather than `c11` because `static_h.h` uses a
  zero-length array extension.

  Being a CMake target in this build is also **safer than make-kit.py's
  route for the `_SH_MODEL()` suffix trap** documented at
  `utils/make-kit.py:430-447`: the generated config picks
  `_sh_model_..._dbg` or `_rel` from whether `NDEBUG` was defined, and a
  target compiled in the same build as `hermesvm_a` gets `NDEBUG` from the
  same per-config rule, so the two agree by construction instead of via a
  `--ndebug` flag transported by hand.
- Generate `embedded_modules_registry_native.cpp` -- the `extern "C"`
  declarations, the sorted table, `findEmbeddedModule`, and the marker --
  from the same combined manifest `gen-embedded-registry.py` reads, so the
  two registries cannot disagree about which ids exist.

The target is `EXCLUDE_FROM_ALL`. A plain `hermes-node` build therefore pays
**nothing**; `check-hermes-node-js`, which already `DEPENDS` on the kit, pays
about 5 seconds.

**The archive has exactly one writer into the kit.** A tracked
`add_custom_command(OUTPUT <kit>/libhermes-node-builtins-native.a ...)` plus
an `add_custom_target` that `hermes-node-kit` depends on, modelled exactly on
`hermes-node-kit-entry` (`tools/hermes-node/CMakeLists.txt:164-188`) and for
the reason recorded there: the kit's real outputs are invisible to the build
system, so an `add_dependencies` order-only edge would build the archive but
leave a stale copy in the kit. `make-kit.py` does **not** copy it -- it is
given only the file name, and its sole job is to write

```
nativebuiltins: {kit}/libhermes-node-builtins-native.a
```

Two writers to one destination would race, which is why the copy and the
manifest line are split this way rather than both going through make-kit.

`readKitManifest` (`lib/build-exe/kit_manifest.cpp`) gains the key. Its
existing rule that an unknown key is an error means an older `hermes-node`
meeting a newer kit fails by name rather than silently dropping the archive.
Note the order: `readKitManifest` rejects the unknown key while parsing
(`lib/build-exe/kit_manifest.cpp:139`), *before* `buildExecutable` ever
compares versions (`lib/build-exe/build_exe.cpp:674`), so the message names
`nativebuiltins` rather than the version mismatch. An earlier draft had this
backwards.

### The Hermes option

`HERMESVM_INTERNAL_JAVASCRIPT_NATIVE` is set **`FORCE`d** to `ON` in
hermes-node's top-level `CMakeLists.txt` before `add_subdirectory(hermes)`.
`FORCE` rather than the non-forced form `HERMES_ENABLE_WASM` uses, and the
distinction is not stylistic: `CLAUDE.md` already records that a non-`FORCE`
`set(... CACHE ...)` leaves an existing cache entry alone, so every build
directory configured before this change -- all four in this tree -- would
silently keep `OFF` and report the feature as not working. The debugger's
option is `FORCE`d for the same reason. Forcing adds no compilation
work: `hermesInternalUnit` is built in every non-MSVC configuration
regardless, so the only cost is the size noted below.

This changes `hermes-node` and `--build-exe` artifacts too, by 338,960 bytes
in the measured macOS arm64 build -- a universal binary pays it per slice,
and another platform's linker may differ. That asymmetry with our own built-ins is deliberate: Hermes's `InternalJavaScript`
is JSLib internals, not Node lib code, so nobody steps into it with
`--inspect`, and without the flip the headline claim is simply false.

## The CLI

`build-native` links the native built-ins **by default**: a fully native
binary is the point of the feature. `--bytecode-builtins` declines them,
which costs almost nothing to offer because both archives ship in the kit
either way, and earns its place twice: a size-sensitive build can decline
the growth, and when a test fails only under native built-ins it is the
lever that separates a fault in a built-in from a fault in a user module.

The choice is a link-time fact, not a container property, so it is **not**
recorded in the container and `--dump` gains nothing. `--build-exe` gains no
flag at all -- it links the merged archive and gets bytecode, as it must,
since it has no native units to pair them with.

## Verification

The risk this feature carries is not "does it link" but "does
`internal/streams/readable.js` still behave the same". A divergence between
what `hermesc` and `shermes` make of one branch in one Node lib file would be
miserable to find, so the verification is built around re-running real tests.

**123 tests are syntactic candidates; the executed corpus is smaller, and
how much smaller is not knowable until it runs.** The selection criterion is
"the test's SOLE `RUN:` line matches", not "a `RUN:` line matches". The
distinction is not pedantry: `ShTest` executes every `RUN:` line, so a file
with a second one runs a command the wrapper cannot reproduce.
`test/test-process-version.js` is exactly that case -- its first line is the
wrappable shape and its second passes `--node-version v18.12.0`, which a
produced executable cannot accept, since every argument belongs to the
program. Counting by "contains" gives 124 and includes it; counting by "sole"
gives 123 and does not. It is recorded in `excluded.txt` with that reason,
under the wider rule defined below.

So: **77** of the 172 top-level `.js` tests have
`// RUN: %hermes-node %s | %FileCheck %s` as their only `RUN:` line, plus
`test/test-typescript.ts` -- lit recognises `.ts` too (`test/lit.cfg:7`), and
that file is worth having, since a TypeScript entry point exercises a path
nothing else here does. **45 of 45** node-ported tests have
`// RUN: TEST_THREAD_ID=$$ %hermes-node %s` as their only line. That is 123
candidates, from which `excluded.txt` subtracts. **Six are already known to
go**: `test-process-exit-code.js`, `test-process-exit-event.js`,
`test-uncaught-exception-async.js`, `test-uncaught-exception-io.js`,
`node-tests/parallel/test-child-process-exit-code.js` and
`node-tests/parallel/test-child-process-kill.js` all re-spawn through
`process.execPath`, which under the wrapper is the artifact rather than
`hermes-node`. So the executed corpus is at most 117, and the final figure is
whatever survives the fixtures work below -- it is reported by the suite, not
predicted here.

Every candidate's command line can be mechanically rewritten to build the
test with `build-native` and run the artifact. The node-ported 45 matter most: they are
the tests that exercise Node's own lib code directly.

A measured `build-native` round trip for a one-module program is **0.5 s** end
to end, so the corpus costs roughly 10-20 s in a Release build at 16-way. It
runs under its own target, `check-hermes-node-native`, which is **not** part
of `check-hermes-node` -- the same arrangement, and for the same reason, as
`check-hermes-node-examples`. The ASAN kit is ~755 MB and links far slower
than the Release one, so folding up to 123 links into the primary development suite
would make the configuration this project is developed in the slowest one to
test.

### How the suite is actually built

A lit config cannot rewrite a `RUN:` line, so the corpus is a **second lit
suite over the same source files**. Four things about it were wrong in the
first draft and are specified here because each would have cost an
implementation round.

1. **It must inherit, not start fresh.** `test/native/lit.cfg` explicitly
   loads `test/lit.cfg` (`lit_config.load_config`), which is where
   `%FileCheck`, `%not`, `%hermes-node`, the environment settings and the
   feature detection all come from.
2. **It must replace the `%hermes-node` substitution, not append one.**
   Substitutions are an ordered list; a second entry for the same pattern
   never fires. The replacement is a wrapper, `test/native/run-native.py`,
   which builds the named script with `hermes-node build-native` and runs
   the artifact, forwarding stdout, stderr and the exit status.
3. **It must gate on `shermes-available` *and* `linker-available`, suite
   wide.** The reused source tests carry neither `REQUIRES:` directive of
   their own, and `test/lit.cfg` deliberately keeps the two features apart.
   The suite additionally checks that the kit actually holds
   `libhermes-node-builtins-native.a`, so an incomplete kit reports
   UNSUPPORTED instead of a suite-wide failure.
4. **Selection needs a custom test format, not `config.excludes`.** A
   manifest alone does not restrict discovery: the suite's source root is
   `test/`, so lit would walk the whole tree, and pointing lit at the listed
   files directly makes each one find its *nearest* suite -- `test/lit.cfg`
   -- and lose the native substitution entirely. So `config.test_format` is
   a small `ShTest` subclass that yields only the manifest's paths.
   `config.excludes` is additionally unusable because it is silent (lit just
   omits the name) and because `test/node-tests/lit.local.cfg` **assigns**
   `config.excludes`, discarding anything inherited.

### The corpus is two committed lists, reconciled

`test/native/corpus.txt` lists the tests that run. `test/native/excluded.txt`
lists every test that **contains** a wrappable `RUN:` line and is
nevertheless not run, each with a one-line reason. "Contains" rather than "is
a candidate" is the deliberately wider net, and it has to be: it holds both
`test-process-version.js`, which fails the sole-`RUN:` rule, and the six
`process.execPath` tests, which pass that rule and fail at run time. One list
with one rule, rather than two lists that would each need explaining. A checker reconciles both against the tree: any test matching one of
the two exact `RUN:` shapes that appears in neither file is an error, as is a
stale entry, an entry in both, or an exclusion with no reason.

So the corpus cannot shrink silently when someone edits a `RUN:` line, and it
cannot grow silently either. Neither an `XFAIL` nor a quiet skip would do
this: an `XFAIL`ed test stops reporting the day it becomes a real failure,
which is the objection `CLAUDE.md` already records against quarantining the
two known flaky tests.

### Tests that cannot pass, and one that can be made to

Some of the 123 candidates will fail under the wrapper without anything being wrong with
native built-ins.

**Data files are the large class, and most of it is fixable.** A bundled
module's `__dirname` roots at the executable's directory
(`rootDirectoryFor()`, `lib/bundle/bundle_run.cpp`), and the producer
packages code, not assets. So the wrapper builds into
`<test_exec_root>/native/<testname>/` and symlinks the right fixtures
directory beside the artifact -- `test/fixtures` for a top-level test, and
`test/node-tests/fixtures` for a node-ported one, because
`test/node-tests/common/fixtures.js` resolves `common/../fixtures` against
the bundle root, which for those tests is `test/node-tests/`. Verified during
review that this fixes `test-read-package-json.js` and
`test-legacy-main-resolve.js`, which reach only into `fixtures/`. This is the
same "ship data files beside the artifact" rule `CLAUDE.md` already documents
for blessed's terminfo under gtop.

**Observing the running binary is the unfixable class.** `process.execPath`
is the artifact, not `hermes-node`, so a test that re-spawns itself through
it runs a different program; `process.argv[0]` and `argv[1]` both hold the
artifact path (`bundle_main.cpp`). These go in `excluded.txt`.

### The two assertions that pin the claim

The corpus proves behaviour, but it would prove it just as happily against
bytecode built-ins if the default ever flipped. Two targeted tests close
that:

- **An exact magic count.** Build one fixture (with no `--bake-wasm`, since a
  baked Wasm entry is Hermes bytecode inside the container) twice. The
  default artifact must contain **exactly one occurrence per architecture
  slice** of `0x1F1903C103BC1FC6` -- `ExtensionsBytecode` and nothing else --
  and the `--bytecode-builtins` one must contain at least 180 per slice. An
  exact number rather than a bound, so that an accidentally-bytecoded
  `InternalJavaScript` unit fails the test instead of hiding under a
  threshold; and per slice rather than per file, because a universal build
  would otherwise multiply both figures and fail a correct artifact.
  `test/build-exe.js:101` is the existing precedent for counting slices.
- **The link argv carries `-u`.** A unit test over the generated link command
  asserts the marker root is present by default and absent under
  `--bytecode-builtins`. This is the one misconfiguration the linker itself
  cannot catch.

## Costs and accepted risks

**An expected +7.2 MB to +9.9 MB per artifact** -- an estimate from the
feasibility section's two probes, not a bound, replaced by a real figure
during implementation -- plus 338,960 bytes in the measured macOS arm64 build
from the Hermes option. Both are accepted as the price of the feature, with
`--bytecode-builtins` as the escape hatch for the larger half.

**About 27 MB of generated `.c` in the build directory** (measured: 27,961,467
bytes over 187 files). This paragraph predicted ~140 MB, from a ~700 KB
average per module; the measured average is ~150 KB, so the prediction was
about 5x too pessimistic. The `acorn` figure was right -- it is 2.8 MB, and it
is the outlier that the guessed average was drawn from. Declaring those as
CMake sources keeps them on disk, which is the cost of letting CMake own the
compile flags. Judged worth it: the alternative is hand-rolled `cc` commands
that re-derive target selection, which is the failure the kit manifest was
built to prevent.

**The JS language flags now have a third consumer.** `kJSLanguageFlags`
(`include/hermes/node-compat/bundle/cjs_wrapper.h`) drives the scanner and
the `shermes` argv for user modules; `JS_COMPILER_FLAGS` in
`lib/embedded-modules/CMakeLists.txt` drives the built-in bytecode compile;
this adds the built-in native compile. A new CMake variable holds **only the
two genuinely shared language flags** (`-Xes6-block-scoping`,
`-Xasync-generators`) and each pipeline keeps its own output, debug and
warning flags -- sharing `JS_COMPILER_FLAGS` wholesale would hand `shermes`
the bytecode-only `-emit-binary`.

That still leaves the CMake variable and the C++ header as two copies nothing
mechanically forces to agree. This is a real hazard and the codebase has
already lost time to it: block scoping off makes every `let`-in-loop closure
capture the wrong binding, with no diagnostic at build time or run time. What
is different after this change is the forcing function -- the whole corpus runs
through natively compiled built-ins, and a block-scoping divergence inside
Node's lib code would fail a great many of them. Generating the header from
CMake was considered and rejected: `cjs_wrapper.h` is a hand-written header
whose prose carries most of its value.

**Debugging into built-in JavaScript is lost in a native artifact.** A
natively compiled `fs.js` has no bytecode debug info, so a debugger cannot
break inside it. This costs nothing new: `--inspect` is already refused with
`--bundle`, and a produced executable parses no flags at all. It is the
reason the answer to "which binaries get native built-ins" is *only*
`build-native` artifacts -- `hermes-node` itself is a development tool and
keeps bytecode built-ins, where stepping into Node's lib code still works.

**Full-GC pause grows with unit count**, at roughly 0.8 ms per thousand
realistically-sized units (measured in the phase 1 design). 187 more units is
about 0.15 ms. Noted rather than mitigated.

**`hermes-node` and `--build-exe` are behaviourally unchanged, but their
bytes change.** The shared `EmbeddedModule` struct grows a field and the
Hermes option changes what `hermesvm_a` carries. An earlier draft claimed
"untouched, byte for byte", which was wrong.

## Deliberately not in scope

**Native-compiling Hermes's `ExtensionsBytecode` unit.** It is the last
interpreted JavaScript in a native artifact, at 1,680 bytes run once. Fixing
it is a Hermes change shaped exactly like `hermesInternalUnit`, and belongs
upstream; filed in the tracker.

**Dropping the compiler and interpreter from a native artifact.** Even with
every module native, a `build-native` binary still links the full Hermes
compiler and interpreter, because `eval` and `new Function` compile at run
time. Removing them is a much larger size win than this feature costs, and a
much larger change; it also changes what a program can do.

**Native built-ins for `hermes-node` itself or for `--build-exe`.** Decided
against for the debugging reason above, and because it would make `shermes` a
hard build dependency of every `hermes-node` build. Nothing here forecloses a
CMake option later.

**Recording the built-in choice in the container.** It is a link-time fact
about the executable, not a property of the container.

## Interactions checked and found clear

`--record-wasm` is already refused with `build-native`
(`tools/hermes-node/hermes-node.cpp:1033`). Baked Wasm entries stay Hermes
bytecode inside the container and are orthogonal to registry selection --
they matter only to the magic-count fixture, which therefore uses no
`--bake-wasm`. Preload modules are ordinary phase-1 native units. The compile
cache never covered embedded built-ins, so nothing about it changes.

## Testing summary

| What | Where |
|---|---|
| The wrapped corpus (at most 117 of 123 candidates), run natively | `test/native/`, custom format, `check-hermes-node-native` |
| Corpus and exclusion lists reconcile with the tree | checker run by the same target |
| Exactly one bytecode magic per architecture slice in a default artifact, >= 180 per slice with `--bytecode-builtins` | new lit test |
| The link argv carries `-u` by default and not otherwise | `BuildNativeTest` |
| Unit names are valid and unique | build-time assertion in the generator |
| Both registries agree on the id set | generated from one combined manifest |
| `--bytecode-builtins` still links and runs | new lit test |
| Manifest key round-trips | `kit.manifest` cases in `BuildExeTest` |
| Real programs | existing `examples/tetris` and `examples/ditz2` native arms |
