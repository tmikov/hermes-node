# Native WebAssembly in `build-native` artifacts

Status: design, reviewed externally (two rounds, see "Review history").

## Goal

A default `hermes-node build-native` artifact should compile no
WebAssembly to Hermes bytecode at run time, for the same reason it no
longer interprets any built-in JavaScript: an artifact whose code is
native should not interpret anything. The criterion is **completeness**,
not speed. A speedup is welcome and will be measured, but it is not what
decides whether this is done.

This became possible with the Hermes submodule update to `80ea8dc99`,
which brought `90125ceb3` ("Wasm: compile WebAssembly natively via
shermes"). `shermes --exported-unit=NAME -c foo.wasm` produces an object
that, linked into an application, yields an ordinary
`WebAssembly.Module` through `WebAssembly.Module.fromNativeUnit(NAME)`.
Today nothing in hermes-node uses it: `fromNativeUnit` exists in our
binary and finds nothing, because nothing links a unit in.

## Decisions taken in brainstorming

1. **Where the bytes come from: the recording run.** `--record-wasm`
   already watches every module the program hands to `WebAssembly`, so it
   is the one place that sees bytes embedded in JavaScript (the common
   npm shape: hermes-parser holds its module as a string literal) as
   well as bytes read from a `.wasm` file. The record file grows to carry
   each module's source bytes. An explicit `--native-wasm=<file.wasm>`
   was rejected: it misses every embedded module.
2. **How a plain `new WebAssembly.Module(bytes)` reaches the unit: a
   Hermes hook.** The embedder cache callbacks gain an optional, content-
   keyed lookup that returns a unit creator. This covers every entry point
   (`new Module`, `compile`, `instantiate`) with no JavaScript change.
   Rejected: wrapping the `WebAssembly` globals in `libjs` (they would stop
   being the engine's own functions, and each new entry point would need
   wrapping by hand), and letting Hermes resolve units by a key of its own
   (duplicates the content digest hermes-node already owns).
3. **A module with no usable native unit: fall back, and warn on
   stderr.** The program still works, getting that module through the
   bytecode tiers exactly as today, but the artifact says so on stderr,
   once per module per process.
4. **Native replaces bytecode, with `--bytecode-wasm` as the opt-out**,
   mirroring `--bytecode-builtins`.

## Overview

```
step 1   hermes-node --record-wasm=app.wasmrec app.js ...
           record file v2: per module, the bytecode (as today) AND the
           source bytes, committed only once Hermes accepted the module

step 2   hermes-node build-native app.js -o app --bake-wasm=app.wasmrec
           each recorded module -> <tmp>/w<sha256>.wasm
                                -> shermes -emit-c --exported-unit=w<sha256>
                                -> cc -x c -c
                                -> linked beside the JavaScript units
           payload assembly gains a table {sha256(bytes) -> sh_export_w<...>}
           the container gets NO bytecode entry for those modules

run      new WebAssembly.Module(bytes)
           Hermes -> lookup_native(bytes)                 [new hook]
             hermes-node: sha256(bytes), binary-search the table
           Hermes: find the unit's static registration, compare its
                   codegen config with this runtime's
             match    -> run the unit's top level, build the module
             no match -> existing tiers: container bytecode, disk cache,
                         compile; stderr warning in a native-Wasm artifact
```

## 1. Command-line surface and workflow

**Recording.** `--record-wasm=<file>` keeps its flag and its behaviour --
written once at runtime creation, rewritten atomically after every new
commit, so a program that ends in `process.exit()` still leaves a
complete file -- but writes **record format v2**, which adds each
module's source bytes (section 4), and commits an entry only once Hermes
has accepted the module (section 2, H4). A v2 file is roughly v1 plus the
raw modules: about +768 KB for `examples/hermes-parser-ast-wasm`.

**Building.** In `build-native`, `--bake-wasm=<rec>` (repeatable, as
today) changes meaning by default:

- every recorded module is compiled natively (section 5) and linked;
- it gets **no** bytecode entry in the container -- a native hit never
  reads one;
- the producer prints `wasm: native <first 16 hex digits> (<n> bytes)`
  per module, so what was compiled natively is visible at build time.

`--bytecode-wasm` (a `build-native` option; that subcommand parses its
own argv, so no other mode can be handed it) restores today's behaviour:
bytecode entries are baked, nothing is compiled natively, and the
run-time warning is off. It is independent of `--bytecode-builtins`, and
it is the lever that separates a fault in native Wasm codegen from
anything else.

**Refusals**, each naming the problem and the way out:

- a **v1** record file in native mode, before anything is compiled -- it
  has no source bytes: re-record, or pass `--bytecode-wasm`;
- an invalid baked `--vm=` option, before anything is compiled (section
  3 explains why this now happens at build time);
- a `shermes` built without Wasm support. There is no probe: `shermes`
  refuses `.wasm` input itself (`tools/shermes/shermes.cpp`, the
  `HERMES_ENABLE_WASM`-off branch), so the first Wasm job fails, and the
  producer reports that failure as a build error naming
  `--bytecode-wasm`. A refusal in effect, though it happens during
  compilation rather than before it.

The record file's version check (the recorded
`HERMES_NODE_VERSION_STRING` must equal the producer's) is unchanged.

**Compatibility of record files.** Updated record readers accept v1 and
v2. A reader from before this change rejects v2 by its format version,
which is the safe outcome. `--build-bundle --bake-wasm` reads only the
bytecode table, so it bakes from either version. `--build-exe` never
reads a record file -- it consumes a container -- and is unaffected.

**Unit naming.** A unit is named `w` followed by the 64 lowercase hex
digits of **SHA-256 over the module bytes alone**, without the codegen
configuration. That is a valid `--exported-unit` (alphanumerics only),
and deliberately a different key from the disk cache and container
digest, which hash the configuration too. For a native unit the
configuration is checked another way -- against metadata the unit itself
carries (H2) -- so the key only has to identify the module.

## 2. Hermes changes

Four, each its own commit in the submodule, none mentioning hermes-node.

**H1. `lookup_native` on `hermes_wasm_cache_callbacks`.** Appended after
`discard`:

```c
/// Return the creator of a natively compiled unit for these bytes, or NULL.
/// Consulted before `lookup`. Returning a creator does not commit Hermes to
/// using it: the unit must be registered and built under this runtime's
/// codegen configuration (see below), and a creator that is not is followed
/// by the ordinary `lookup`, exactly as if this had returned NULL.
/// Optional: NULL here means "no native tier".
SHUnitCreator (*lookup_native)(
    void *ctx, const uint8_t *wasm, size_t wasm_size);
```

The existing setter rejects any `struct_size` below the **current**
`sizeof` (`hermes_napi_wasm_cache.cpp:23`), so appending a field alone
would lock out every caller compiled against the old header. The minimum
becomes `offsetof(hermes_wasm_cache_callbacks, lookup_native)`, and the
new field is read only when `struct_size >= offsetof(lookup_native) +
sizeof(callbacks->lookup_native)`; it is copied into `WasmCacheHooks`
like the others. The opaque `SHUnit` / `SHUnitCreator` typedefs move above
the struct (they are below it today, `hermes_napi.h:342`). No store token
is involved: nothing is stored on this path, and keeping it out of the
token protocol leaves "exactly one of `store` or `discard`" untouched.

**H2. A Wasm unit's static registration carries its codegen
configuration.** `shermes` emits `{name, creator, codegen_config, NULL}`
instead of `{name, creator, NULL}` into `SHWasmUnitReg` (SH.cpp, the
`options.wasmUnit` block). The string is the same `wasmCodegenConfig()`
form the cache keys on (`hermes-wasm;bc=<n>;cg=<n>;t262=<0|1>`), built
from the **emitting compiler's** own version constants and effective
settings -- the factoring that lets one function serve the runtime and
the backend is the plan's to choose. Static data, not an `SHUnit` field:
calling a creator `calloc`s a fresh `UnitData` (SH.cpp:3224), and
`_sh_unit_init` calls the creator again itself (StaticHUnit.cpp:134), so
a configuration read from a created unit would leave that first
allocation unowned on every hit and every refusal. `SHWasmUnitReg`'s
layout changes, so the unit-ABI component of the model symbol is bumped
as `90125ceb3` did; that protects generated objects in the normal
static-link workflow, and is not a version check on a hand-written
registration, since `_sh_wasm_register_unit` receives neither size nor
version.

`_sh_wasm_register_unit` already aborts on a duplicate **name**; it now
also aborts on a second registration of the same **creator** with
different metadata, so "the registration for this creator" is always
unambiguous. Two generated objects cannot collide that way -- each
defines its own `sh_export_<name>` -- so this guards hand-written
registrations only.

**H3. `createModuleFromBytes` consults the native tier first.** In the
`.wasm` branch, before `lookup`:

1. call `lookup_native` if present; NULL -> continue as today;
2. find the registration whose `creator` equals the returned pointer, a
   new helper beside `_sh_wasm_find_unit` that walks the same list under
   the same mutex and releases it before anything runs. **No
   registration, a NULL configuration, or a configuration different from
   `wasmCodegenConfig(runtime)` -> a miss: continue as today.** Nothing
   has been allocated. A missing registration is never remembered as
   negative, since a unit's static constructor may simply not have run
   yet;
3. match -> `buildModuleDataFromArtifact(runtime, nullptr, creator,
   ...)`, the path `fromNativeUnit` already takes, which calls the
   creator exactly once. A failure of the unit's top level is the
   module's error, raised as `fromNativeUnit` raises it, **not** a
   fallback: the unit is the artifact, and a broken artifact is not a
   slow one (the rule `bundleFatalWasmRefused()` already applies to a
   refused baked entry). Allocation failure inside unit creation keeps
   whatever behaviour every native unit already has; this design does not
   change it.

`fromNativeUnit` gains the same configuration check, so "a mismatched
unit never executes" holds for both entry points. It has no bytes to fall
back to, so there a mismatch is a `TypeError` naming the unit, its
configuration and the runtime's.

Validation is skipped on a native hit, exactly as on a bytecode cache
hit; the key is a SHA-256 of the bytes `shermes` validated at build time.

`verifyIR` is deliberately not part of the configuration: it adds checks
and can reject a compile, but it does not change the code generated from
valid IR (`WasmCompile.cpp:151`), so it does not affect whether a unit is
**compatible** with a runtime. It does mean the configuration is not a
predictor of whether a compile succeeds, which H4 depends on.

**H4. An optional `accepted` notification.** Appended after
`lookup_native`, gated by `struct_size` the same way:

```c
/// Called when Hermes has accepted the bytecode a `lookup` returned,
/// immediately before the `discard` for that token. Optional.
void (*accepted)(void *ctx, void *store_token);
```

The recorder needs to know when a module is good, and today it cannot
tell. An accepted hit and a rejected hit followed by a failed recompile
both end in `discard` (`WebAssembly.cpp:806` and `:827`), and the second
is reachable without OOM in two ways: `verifyIR` is outside the cache key
and can reject a recompile, and a cache entry proves nothing about the
bytes -- Hermes's own test serves valid bytecode for invalid Wasm and
succeeds (`NapiWasmCacheTest.cpp:177`). Rather than reconstruct the
outcome, Hermes reports it. The "exactly one `store` or `discard`"
contract is unchanged.

## 3. hermes-node run time

**The table.** The generated payload assembly gains, beside
`hermesNodeNativeUnits`:

```
hermesNodeNativeWasmUnits:      // sorted by digest, for binary search
    .byte <32 digest bytes>
    .quad sh_export_w<hex>
    ...
hermesNodeNativeWasmUnitCount:  .quad <n>
hermesNodeNativeWasmPolicy:     .quad <0|1>   // 1 = warn on a fallback
```

All three are emitted **unconditionally**, empty for a bytecode
`--build-exe`, for the reason the comment in `payloadAssembly()` already
gives: one `bundle_main.cpp` links against a single definition either
way. Entries are 40 bytes, so the table is `.p2align 3` in
`.data.rel.ro` on ELF, as `hermesNodeNativeUnits` is. The objects are on
the link line directly, never in an archive, and the table references
each `sh_export_w<hex>`, so every unit is linked and its registration
constructor runs. `bundle_main.cpp` hands the three to the runtime
alongside the existing two.

**The hook.** `lib/compile-cache/wasm_cache_hooks.cpp` gains
`cacheLookupNative`: SHA-256 the bytes (picohash, already a dependency),
binary-search the table with `memcmp`, return the creator or NULL. It is
installed only when the table is non-empty or the policy is 1, so every
other run -- `hermes-node app.js`, `--bundle`, a `--build-exe` artifact --
pays nothing new.

**Order** becomes native, then container bytecode, then disk, then
compile. A native miss falls through to exactly the path the run would
have taken without this feature.

**The warning.** When the policy is 1 and `lookup` (the bytecode tier) is
reached, the native tier did not answer. The decision is made at the top
of `lookup`, **before** the store-token allocation, so a token
allocation failure cannot suppress it. `cacheLookupNative` records, per
digest, whether it returned a creator, which tells the two cases apart:

```
warning: WebAssembly module <16 hex> was not compiled natively (it is not in the recording); falling back to bytecode
warning: WebAssembly module <16 hex>: its native unit is unavailable or incompatible with this run (e.g. -test262); falling back to bytecode
```

"Falling back to bytecode", not "compiling": the bytecode may come from
the container or the disk cache. The second message cannot be more
specific, because from the hook's side an absent registration, NULL
metadata and a configuration mismatch look the same; an explicit refusal
reason from Hermes was considered and is not worth another ABI field for
a message whose remedy is the same in every case. Once per digest per
process, unconditional rather than behind `HERMES_NODE_DEBUG_NATIVE`.

**Tracing**, under `HERMES_NODE_DEBUG_NATIVE=COMPILE_CACHE`, adds `wasm
native hit` and `wasm native miss`. Tests assert from tracing, never from
timing.

**`-test262`.** A unit's configuration is fixed at build time, the
runtime's is not: an artifact built with `--allow-vm-options-override`
can run under `HERMES_NODE_VM_OPTIONS`. The `hermes-node build-native`
CLI therefore derives the **effective** baked value by running the baked
`--vm=` list through `buildVmRuntimeConfig()` -- the same dedupe and
last-occurrence-wins path a run uses (`vm_options.cpp:233`) -- and
reading `getTest262()`. It passes a plain bool into the producer, so the
producer library stays VM-free; the environment variable is excluded, as
`--build-bundle` already excludes it. Each unit is compiled with
`-test262` exactly when that bool is set, so the artifact's default
configuration hits; an override that changes it produces the second
warning and a correct fallback.

Two consequences. Parsing the whole list moves the rejection of an
unrelated invalid baked option from the artifact's startup to build time,
where today the producer only serializes the strings
(`bundle_build.cpp:1648`) -- a behaviour change, and a better place for
the error. And because `llvh::cl`'s registry is process-global and the
parse resets it, it happens before any parallel compilation starts.

The bytecode and Wasm codegen versions match by default, because the
kit's `shermes` and its runtime come from one build. `--shermes=<path>`
can break that by naming a different `shermes`; H3's check catches it,
as the second warning at run time, which is why the check sits in Hermes
rather than relying on the kit.

## 4. Record file format v2

`BundleWasmRecord` is shared verbatim between the record file and the
container's v6 Wasm table, and the container does not change. So v2 adds
a **second, separate table** to the record file rather than widening that
struct:

```
WasmRecordHeader  (v2 appends sourceTableOffset, sourceCount)
BundleWasmRecord[recordCount]     -- unchanged: config digest -> bytecode
WasmSourceRecord[sourceCount]     -- new: sha256(bytes) -> source bytes
  { uint8_t digest[32]; uint32_t offset; uint32_t size; }
```

**Parsing is version-dependent.** The reader checks `size >=` the v1
header, then the magic and version -- rejecting an unknown version
before interpreting anything past the common prefix -- and, for v2 only,
`size >=` the v2 header. No accessor reads a v2-only field from a v1
header. Both tables are sorted by digest and validated structurally,
like everything else in the format (magic, version, every offset,
length, alignment and range). No payload checksum, for the reason the
v6 container has none. Two recordings of one module under different
configurations share one source entry.

**Commit boundary.** The recorder copies the bytes into the store token
at `lookup` -- they are valid only for that call -- as a **pending**
entry, keyed by SHA-256 over the bytes alone. It is committed, together
with its bytecode entry, only when the module is known good: at `store`
(a compile succeeded) or at `accepted` (H4: Hermes took a hit). A miss
whose compile fails, and a rejected hit whose recompile fails, both end
in a bare `discard` and commit nothing. This applies to the bytecode
side as well, which today records a container or disk hit at `lookup`,
before knowing whether Hermes accepts it -- the same gap, closed by the
same notification.

`--dump-wasm` prints the source table too.

## 5. The producer

**Placement.** The bake step lives in `buildBundleImpl`
(`bundle_build.cpp`, the loop over `bakeWasmPaths`), which adds bytecode
entries, serializes and writes the container before returning. Doing the
native work afterwards would be too late to omit those entries. So that
step takes a mode. In native mode it performs the same file checks --
format version, recorded build version, duplicate diagnostics -- but
adds **no** bytecode entry; instead it returns owned copies of the
de-duplicated source entries in `BuildProducts` (a new field), before
serialization. Native mode requires v2.

Then `bundle_build_native.cpp`:

1. writes each source entry to `<tempDir>/w<hex>.wasm`;
2. adds each as a job in the existing pool: `shermes -emit-c
   --exported-unit=w<hex> [-test262]`, then `cc -x c -c`, under the same
   `-O` level and flags as a JavaScript unit -- `--jobs` covers both
   kinds;
3. passes the objects to `buildNativeLinkCommand()` with the JavaScript
   ones, and the `(digest, symbol)` pairs to `payloadAssembly()`.

**Failure policy: any failure compiling a Wasm unit is a hard build
error.** The two-tier policy for JavaScript modules does not transfer.
There, a `shermes` source rejection becomes a throwing stub because the
static walk reaches code the program may never load. A recorded Wasm
module, under section 4's commit boundary, is one Hermes accepted during
the recording run, so `shermes` rejecting it is a defect in one of the
two, not a property of the program. The message names the digest, the
failing stage, and `--bytecode-wasm` as the way to ship anyway.

**The open risk is `cc`.** hermes-parser's module is 768,303 bytes of
Wasm. The C that `shermes` generates from it has not been measured, and
the native-compilation work already recorded `cc` reaching 3.66 GB peak
RSS on one large generated JavaScript file at `-O3`. The first task of
the plan measures it: generated C size, `cc` time and peak RSS at `-O0`
and `-O3`, artifact size against the `--bytecode-wasm` build. If one
module turns out impractical to compile, that is a finding to bring back
before more is built, not something to work around inside this design.

## 6. Testing

Hermes (submodule):

- `NapiWasmCacheTest`: an **actually old-sized** callbacks struct still
  installs and completes a miss/store round trip and an accepted-hit
  round trip, with the new slots never read; `lookup_native` returning
  NULL, a registered matching creator, a creator with no registration, a
  registration with a different configuration (each of the last two
  falling through to `lookup`); `accepted` called before `discard` on an
  accepted hit and never on a rejected one; a rejected hit followed by a
  failed recompile ending in a bare `discard`.
- Registry: a second registration of one creator with different metadata
  aborts; a matching duplicate does not.
- A `test/wasm` lit harness in the existing native-Wasm style whose
  driver installs `lookup_native` through `hermes_set_wasm_cache` and
  calls `new WebAssembly.Module(bytes)`, so H3 is traversed rather than
  bypassed as `fromNativeUnit` bypasses it. It needs a `napi_env` on the
  same runtime that runs the driver; an addon loaded through
  `loadNativeModule()` provides one (`ConsoleHost.cpp:242`), and the test
  is gated on NAPI support as well as `wasm` and `shermes`. Cases: units
  compiled with and without `-test262` and only the matching runtime
  accepting each; a unit whose top level throws, registered by a test
  shim with **matching** metadata so the throw is reached rather than a
  refusal, being the module's error and not a fallback.
- `fromNativeUnit` on a mismatched unit: the `TypeError`.

hermes-node:

- `WasmRecordTest`: v2 round-trip; v1 still read for bytecode; an empty
  v1 file; source entries de-duplicated across configurations; v2-only
  fields never read from a v1 header; malformed source tables rejected;
  the commit boundary (pending entries discarded on a bare `discard`).
- `VmOptionsTest` or `BuildNativeTest`: the effective baked `-test262`
  for absent, `-test262`, `-test262=false` and a repeated flag.
- `BuildNativeTest`: the payload assembly's Wasm table (sorted, 40-byte
  entries, the three symbols present even when empty, ELF section and
  alignment), and the `shermes` argv for a Wasm unit with and without
  `-test262`.
- Lit, `REQUIRES: wasm, linker-available, shermes-available`, modules
  from `test/fixtures/wasm/modules.js`:
  - record, build, run with the source tree moved away: `wasm native
    hit`, no `wasm store`, correct output;
  - the native build's intermediate container, kept with `--keep-temp`
    and opened with `--bundle=<it> --dump`, dumps successfully and has
    no `WASM` section; the same check under `--bytecode-wasm` shows the
    entries present. (A trace showing a native hit cannot prove the
    entries absent, since the native tier masks them.)
  - a module the recording never saw: the first warning, once, with a
    cold compile and again with a warm disk cache;
  - `--bytecode-wasm`: `wasm container hit`, no warning;
  - a v1 record file: refused before compiling, naming both ways out;
  - `-test262` baked, unlocked, run with
    `HERMES_NODE_VM_OPTIONS=-test262=false`, and the inverse (nothing
    baked, `-test262` from the environment): the second warning, and an
    out-of-bounds store whose result differs with and without `test262`
    (dropped versus trapped), caught and reported inside the fixture so
    output, trace and exit status are asserted together;
  - recording a program that catches an invalid-module error, then
    building natively from that recording, succeeds -- the failed module
    was never committed;
  - v2 record -> `--build-bundle --bake-wasm` -> `--build-exe` still
    works.
- `test/build-native-wasm.js` keeps its current cases under
  `--bytecode-wasm`, since what it pins -- caching and baking in a native
  artifact -- is the opt-out's behaviour.
- `examples/hermes-parser-ast-wasm` gains a `build-native` arm: native
  Wasm, output byte-identical to the unbundled run, no warning.

## Out of scope

- **The source bytes still ship.** The program builds its `Uint8Array`
  before `WebAssembly.Module` sees it, so the bytes inside the
  JavaScript stay; native code replaces the compile, not the data.
  Eliminating that copy needs a token where the bytes were, as for the
  container's double copy.
- **`.wasm` files read with `fs`** still have to ship beside the artifact.
- `--build-exe` and `--bundle` get nothing: a bytecode container cannot
  carry native code.
- MSVC: Wasm unit self-registration is unimplemented there
  (`dz/issues/01a0d7fa` in Hermes), and this design now depends on
  registration, so native Wasm is unavailable on Windows until that is.
- A speed comparison is measured and recorded, not required.

## Documentation

`CLAUDE.md` gains a "Native Wasm" subsection under Native Compilation;
the "Baked Wasm entries" subsection says what `--bake-wasm` means in
`build-native`; the record file's version bump and commit boundary are
noted where v1 is described.

## Review history

External review (Codex), round 1: nine findings, all verified against
the code. The design changed in response to five. The struct-size gate
would have locked out existing callers (H1). Reading the configuration
from a created unit leaked an allocation, so it moved to the static
registration (H2). The producer did not, in fact, already parse baked VM
options (section 3). Recording at `lookup` committed modules that then
failed to compile (H4, section 4). And the native work could not run
after `buildBundleImpl` had already serialized bytecode entries (section
5). Round 2 accepted eight resolutions and rejected the ninth -- an
argument that a rejected hit followed by a failed recompile was
unreachable short of OOM, which `verifyIR` and Hermes's own cache test
both disprove; H4 replaced it.
