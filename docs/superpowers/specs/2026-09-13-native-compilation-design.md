# Design: compiling a program to native code with shermes

**Status:** Draft for review, 2026-09-13.

`--build-bundle` compiles a program's `require()` graph to Hermes bytecode
and `--build-exe` links that container into a standalone executable, where it
is interpreted. This compiles the same graph to **native code** instead:
every JavaScript module becomes a Static Hermes compilation unit, compiled
through C to an object file, and linked into the executable alongside the
runtime.

The shape is deliberately the same as the bundle/`--build-exe` pair, and
almost all of the machinery is the same code. What changes is the payload:
where a container holds bytecode, a native build holds nothing, and the code
lives in linked units.

This is phase 1. It compiles the program's own modules and its
`node_modules` tree. It does **not** compile the built-in JavaScript
(`libjs/`, `libjs-node/`, the shims), which stays embedded bytecode and stays
interpreted; that is the obvious follow-up, and see "Deliberately not in
scope". WebAssembly is not native-compiled either -- it keeps compiling to
Hermes bytecode at run time, exactly as it does under `--build-exe`, with the
disk cache and baked container entries working unchanged. Nothing about Wasm
is taken away here.

## What already exists, and is reused unchanged

Everything about a bundle except the payload:

- the `require()` scanner (`lib/bundle/require_scanner.cpp`), which
  identifies `require` by binding rather than by name and wraps each source
  in the CommonJS wrapper before parsing;
- the one resolver with two backends (`lib/bundle/bundle_resolve.cpp`),
  `--include` and `--preload`;
- the container format, the edge table, the `kRequirable` flag, resolve-only
  `package.json` records, the preload table, the natives table and its
  sidecar rules, baked `--vm=` options and the override bit;
- the closed world, `libjs/bundle-loader.js`, and both escape closures
  (`__closeDiskModuleLoading`, the `Module._resolveFilename` wrap);
- `--build-exe`'s kit, `kit.manifest`, `resolveDriver()`,
  `driverCandidates()`, `versionOutputIsClang()`, `formatCommandLine()` and
  `runCommand()`;
- `tools/hermes-node/bundle_main.cpp`, unmodified in its argv handling.

A native build is an AOT bundle whose `kJavaScript` payloads are empty. That
is the whole idea, and it is what keeps this design small.

## The decision: one unit per module, initialized lazily

`shermes -exported-unit=<name>` emits an object file defining
`SHUnit *sh_export_<name>(void)` and no `main`. `_sh_unit_init(shr,
creator)` registers the unit with the runtime, runs its `unit_main`, and
returns the completion value of the file. A module staged as

```js
(function (exports, require, module, __filename, __dirname) {
  ...original source...
});
```

therefore has, as its unit's completion value, exactly the CommonJS wrapper
function that `__bundleLoad(identity)` already promises to return. The
contract lines up with no adaptation: the loader asks for a wrapper, the unit
produces one, and it does so **on demand**, at the first `require()`, which
is when a bytecode module is run today.

The alternative -- concatenating every module into a single unit, which
`shermes` supports directly, since it splices multiple input files' statements
into one program while keeping each file's own source buffer -- was
considered and rejected. It gives one string table and the possibility of
cross-module optimization, but it serializes the C compile (the dominant
cost, see "Measurements") into one process over one enormous file, and it
makes the per-module failure policy below impossible to express. One unit per
module keeps the producer's structure identical to the bytecode producer's:
each module is compiled independently, and a module that fails is handled by
itself.

Measured per-unit fixed overhead, on two toy modules: 4,600 bytes of object
as one unit against 3,528 + 3,640 as two, so roughly 2.5 KB per unit of
string pool, symbol array, function-info table, source-location table and
`UnitData`, plus duplicated common identifiers. At a thousand modules that is
a few MB of object and about 1 MB of runtime `SymbolID` arrays. Accepted.

Two run-time costs of that shape are worth naming, because neither is
bounded by anything this design controls. **Every GC visits every initialized
unit**, and a long-lived collection scans each unit's whole symbol and
property-cache arrays, so full-GC pause time grows with the total number of
units rather than with live data. Identifier *strings* are deduplicated by
content in the `IdentifierTable`, so duplicated names across units do not
multiply entries there -- but each unit still keeps its own `SymbolID` array
and its own static string pool, and the GC still walks the duplicates. And
each unit's `calloc`'d `UnitData` and its `SHUnitExt` live until runtime
teardown: bounded, not leaked, but resident.

Related and worth fixing while we are here:
`sh_unit_additional_memory_size()` undercounts badly -- it uses
`sizeof(unit->runtime_ext)`, the pointer, and omits the generated `UnitData`
allocation entirely. At eight units nobody noticed; at 1,500 the heap
accounting is simply wrong. Accurate accounting needs an emitted
allocation-size field on the unit.

Startup and full-GC pause time on a realistic ~1,500-module graph must be
measured during implementation. Nothing here suggests a correctness limit
after the array change, but the pause-time curve is unmeasured and is the
most likely unpleasant surprise.

## The container: one new flag bit, no version bump

Format v6 as it stands, with every `kJavaScript` record's payload zero
length. No new field is needed for the module data itself: the module kinds,
the `kRequirable` flag, the edge table, the JSON payloads, the preload and
natives tables and the VM options all mean exactly what they mean today, and
"is this module native?" is answered by the unit table having a non-null
entry for its index, which is where the answer has to live anyway.

One bit is needed, though, and the reason is the temp file below. The
container **is** written to a file: `.incbin` takes a path, so the producer
serializes the container into the build's temp directory alongside the
`.c`/`.o`/`.s`, through the existing `atomic_write`, subject to the same
quote/backslash/CR/LF path rejection `payloadAssembly()` already applies. It
is removed on success unless `--keep-temp` -- and `--keep-temp` is exactly
how a container with empty payloads ends up on disk where someone can hand
it to `--bundle=`, which would reach `fatalBadPayload()` and report a
damaged artifact when it is merely the wrong kind.

So: `kBundleFlagNativeUnits` in the header's container-flags word, the
second bit there after `kBundleFlagAllowVmOptionsOverride`. Where it is
enforced matters, because `openBundle()` (a container on disk) and
`openEmbeddedBundle()` (the copy linked into an executable) both go through
the same `BundleReader::open()` -- rejecting there would reject every native
executable, which is the one thing that must work:

- `BundleReader` recognizes the bit and exposes it; `open()` does not judge it.
- `openBundle()` refuses a native container by name, saying it holds no
  bytecode and must be run as the executable it was linked into.
- `openEmbeddedBundle()` accepts it, and requires a unit table whose count
  matches the container's module count -- a native container with no unit
  table, or a unit table of the wrong size, is a producer bug and is fatal.
- `openForInspection()` accepts it, so a future `--dump` can describe one.

The implementation must confirm that `BundleWriter` and `BundleReader` accept
a zero-length payload; the format permits it, but nothing has exercised it.

## Three Hermes changes

### `SHUnit *units[8]` becomes growable

`hermes/include/hermes/VM/sh_runtime.h:77` caps a process at eight units:

```c
/// The active SHUnits in this runtime.
SHUnit *units[8];
```

and `_sh_unit_init` aborts past it. The cap is process-wide, not per
runtime: the index counter is a function-local `static uint32_t nextIndex`
and each unit's index lives in a `static` in its own object file. Index 0 is
reserved for "unassigned", so it is really seven.

The archaeology is worth recording, because the commit does not. `23f102e3b`
"Store units by index in the runtime" (Neil Dhar, 2024-06-22, D58649567)
replaced a `std::vector<SHUnit *> shUnits` on `Runtime` with this array. Its
entire stated rationale is *"Use the assigned indices to store an SHUnit in
the runtime. This will allow a unit to efficiently access them based on the
known index."* That justifies **the array** -- generated code does
`shr->units[unit_index]` as a single load off the runtime struct, which a
`std::vector` could not give -- and says nothing about **8**. It reads as
"enough for the handful of units a shermes program links."

The change:

```c
-  SHUnit *units[8];
+  SHUnit **units;
+  uint32_t units_size;
```

Generated code is unchanged as text and gains one dependent load in the
prologue of each function that touches the unit. Reallocation is safe: a
frame holds the `SHUnit *` itself, never a pointer into the array, and the
`SHUnit` objects do not move.

Outside generated code there are **eight** uses: five in `Runtime.cpp` (the
`std::fill` at construction plus four range-`for` loops -- root marking, weak
roots, `mallocSize()`, teardown) and three in `StaticHUnit.cpp` (the capacity
check, the lookup, and the store).

Four details are load-bearing, and three of them are easy to miss:

- **The capacity check must move out of `if (!*unit->index)` and become an
  unconditional ensure-capacity on *this* runtime**, before both the lookup
  and the store. Today it sits inside the index-assignment branch, which is
  correct only because every runtime's array is the same fixed size. The
  moment arrays are per-runtime and grown on demand, a unit that was assigned
  index 900 by runtime A and is then initialized in runtime B reads
  `B.units[900]` out of bounds -- the index counter is process-wide (a
  function-local `static uint32_t nextIndex`, with each unit's index in a
  `static` in its own object file) while the array is not.
- **Newly added slots must be null-initialized.** The lookup treats a null
  entry as "not registered in this runtime", which is what the `std::fill` at
  construction establishes today; `realloc` does not.
- **A failed or overflowing growth must not overwrite the old pointer.** Size
  into a temporary, check, then commit.
- **`mallocSize()` must account for the backing store**, `units_size *
  sizeof(SHUnit *)`, on top of the per-unit sizes it already sums; and the
  array is freed in `~Runtime`.

Growth is unsynchronized beyond what exists: a Hermes runtime is
single-threaded, so the array is grown, read and marked on its owning thread.
The existing mutex in `_sh_unit_init` continues to guard only the
process-wide index counter, which is genuinely shared.

Tests: more than eight units in one runtime; two sequential runtimes; two
simultaneously live runtimes with units registered in both; and specifically
a runtime B that initializes a **high-index unit first**, which is the
out-of-bounds case above and passes trivially if the test happens to
register units in ascending order.

A larger fixed array was considered. It keeps the single load, but it is
still an arbitrary cap that aborts at a size no build can predict, and
placing thousands of pointers at the head of `SHRuntime` pushes `stackPointer`
and `currentFrame` out of small immediate offsets.

### `hermes_init_sh_unit`

```c
typedef struct SHUnit SHUnit;
typedef SHUnit *(*SHUnitCreator)(void);

NAPI_EXTERN napi_status NAPI_CDECL hermes_init_sh_unit(
    napi_env env,
    SHUnitCreator creator,
    napi_value *result);
```

in `hermes/API/napi/`, alongside `hermes_set_wasm_cache`, and following
`hermes_run_bytecode`'s conventions. The body is short but every line of it
is a decision:

- `NAPI_PREAMBLE(env)`, then validate `creator` and `result`.
- Call **`_sh_unit_init_guarded`**, not `_sh_unit_init`. The guarded form
  supplies the `GCScope` and catches Static Hermes's longjmp-based exception
  unwind; letting a `_sh_throw` cross the NAPI boundary is not something the
  boundary is built for.
- On failure, **do not** call `captureRuntimeException()`. `_sh_catch()` has
  already extracted and cleared the runtime's thrown value, so asking the
  runtime for it again finds nothing. Assign
  `HermesValue::fromRaw(value.raw)` to `env->pendingException` directly, set
  the pending flag, and return `napi_pending_exception` -- the same status
  `hermes_run_bytecode` returns for a module whose top level threw, which is
  what makes a throwing native module propagate through `require()` exactly
  as a throwing bytecode one does.
- On success, `env->addToCurrentScope(HermesValue::fromRaw(value.raw))`
  before returning it. The raw reinterpretation is sound *inside* Hermes --
  `SHLegacyValue` and `HermesValue` share a representation and Static Hermes
  already converts between them -- but handing the raw value out as a
  `napi_value` without rooting it in the current handle scope is not.

`HermesRuntime::getSHRuntime()` is already public, so hermes-node could reach
the `SHRuntime *` itself -- but the conversion and rooting above cannot be
done outside Hermes, and doing the whole thing there keeps hermes-node free
of `static_h.h`, whose inline functions depend on the VM layout defines this
codebase deliberately never includes (see the note beside
`hermes_napi_create_env` in `lib/runtime/hermes_node_runtime.cpp`). The only
type hermes-node needs is the opaque function pointer, declared in
`hermes_napi.h`.

### `shermes -source-name=<name>`

Overrides the source buffer's identifier, which is what
`SourceErrorManager::getSourceUrl()` returns and therefore what lands in the
unit's `SHSrcLocationTable` -- so it reaches stack traces, which is the whole
point. Roughly ten lines.

It exists to delete a problem rather than to add a feature. Without it, the
only way to control the name in the table is the path handed on the command
line, which means staging each module in a tree mirroring its identity and
running `shermes` with its working directory set to the staging root. That
in turn means the producer cannot use `posix_spawnp()` -- there is no
portable `chdir` file action, since glibc gained
`posix_spawn_file_actions_addchdir_np` only in 2.29 and the Linux release
image (AlmaLinux 8) has 2.28 -- and would have to `fork()`, `chdir()` and
`exec()` from inside a `--jobs` worker pool, where only async-signal-safe
calls are permitted between the fork and the exec. With the flag: no cwd
change, `posix_spawnp()` unchanged, and the staging tree becomes **flat**
(`<tmp>/0017.js`) because its filenames no longer mean anything.

Define it as single-input-only, and rejected with more than one input file,
rather than inventing a mapping. An explicit `//# sourceURL=` in the source
should still win over it, matching what the bytecode parser does with the
same comment.

## The producer: `hermes-node build-native <entry.js> -o <out>`

A **subcommand**, dispatched from `main()` before the ordinary parse loop,
like `cache`. The reason is the parse loop's invariant: it stops at the
first positional and everything after it belongs to the program being run.
Every tool verb so far is a flag bolted onto that run-oriented line, which
is why `checkToolOptions()` has grown into a conflict matrix and why
`--build-exe` needed a special refusal of `-`-prefixed arguments after its
container -- that slot is the one place the invariant becomes observable.
This verb wants more flags than any of them, and a subcommand parses its own
argv with flags in any position, joining no matrix.

`build-native` in `argv[1]` is always the subcommand, whether or not a file of
that name exists; `./build-native` runs a script so named. Same rule as
`cache`, and for the same reason: a grammar whose meaning changed with the
contents of the working directory is a worse surprise than the shadowing it
avoids.

If `--build-bundle` and `--build-exe` ever move under subcommands too, the
flag/subcommand asymmetry this introduces disappears rather than becoming
permanent. Nothing here depends on that happening.

### Flags

| flag | meaning |
| --- | --- |
| `-o <file>` | output executable; required |
| `--include=<spec>` | as `--build-bundle`, repeatable |
| `--preload=<spec>` | as `--build-bundle`, repeatable |
| `--vm=<flag>` | baked into the container, as `--build-bundle` |
| `--allow-vm-options-override` | as `--build-bundle` |
| `--jobs=<n>` | parallelism; default `std::thread::hardware_concurrency()` |
| `-O0` `-O1` `-O2` `-O3` `-Os` | one knob driving both the shermes IR level and the cc level; `-O3` default |
| `--kit=<dir>` | as `--build-exe` |
| `--cc=<path>` | as `--build-exe`; replaces the candidate list rather than heading it |
| `--shermes=<path>` | overrides the kit's copy |
| `--keep-temp` | keep the whole temp directory: staged sources, `.c`, `.o`, the generated `.s` and the container |
| `--verbose` | narrate to stderr |

`--bake-wasm=<file>` is accepted and behaves exactly as it does for
`--build-bundle` (see the Wasm note under "Deliberately not in scope").
`--record-wasm` is refused by name, because this verb never runs the program.
`--jobs=0` is refused rather than silently meaning "unlimited".

The optimization knob maps to both tools, so one number means one thing:

| flag | shermes | cc |
| --- | --- | --- |
| `-O0` | `-O0` | `-O0` |
| `-O1` | `-Og` | `-O1` |
| `-O2` | `-O` | `-O2` |
| `-O3` (default) | `-O` | `-O3` |
| `-Os` | `-Os` | `-Os` |

`-O2` and `-O3` differ only on the cc side because shermes offers four
levels (`-O0`, `-Og`, `-Os`, `-O`) and `-O` is its highest; there is nothing
above it to ask for.

### Pipeline

Discovery, resolution, and container assembly are the bytecode producer's,
unchanged, up to the point where it would compile. Then, per JavaScript
module, in a pool of `--jobs` workers:

1. **Stage.** Write the CommonJS-wrapped source into a **flat** temp file,
   `<tmp>/<index>.js`. Flat, not a tree mirroring the identity, because
   `-source-name=` carries the name the source-location table should hold, so
   the staged filename means nothing. The wrapper prefix is
   `kCJSWrapperPrefix` from
   `include/hermes/node-compat/bundle/cjs_wrapper.h`, the same single source
   the scanner and the bytecode compile step already share, so the scan sees
   exactly the text that gets compiled.

2. **shermes.**

   ```
   shermes -emit-c -g2 -O<n> -w -sm-comment=off \
       -Xes6-block-scoping -Xasync-generators [-transform-ts] \
       -source-name=<identity> -exported-unit=hn_m<index> \
       -o <tmp>/<index>.c <tmp>/<index>.js
   ```

   spawned with `posix_spawnp()`, the existing `runCommand()` path, with no
   working-directory change anywhere.

   **The language flags are not optional and their absence is silent.** The
   bytecode path sets `enableES6BlockScoping`, `enableAsyncGenerators` and
   `enableGenerator` in `hermes_napi_compile.cpp`, and `enable_ts` by
   extension in `bundle_build.cpp:1441`. `shermes` defaults the first two
   **off**. Measured, with `shermes -exec`:

   ```js
   var fs = [];
   for (let i = 0; i < 3; i++) fs.push(function () { return i; });
   print(fs.map(function (f) { return f(); }).join(','));
   ```

   | invocation | output |
   | --- | --- |
   | `shermes` default | `3,3,3` |
   | `shermes -Xes6-block-scoping` | `0,1,2` |

   Every `let`-in-loop closure changes meaning, with no diagnostic at build
   time and none at run time. That makes flag parity the most dangerous thing
   in this design: the driver mismatch below fails loudly, and this does not
   fail at all.

   The parity table, one row per field the bytecode producer sets, is
   therefore part of the design rather than an implementation detail:

   | bytecode path | value | shermes |
   | --- | --- | --- |
   | `compileFlags.enableES6BlockScoping` | true | `-Xes6-block-scoping` |
   | `compileFlags.enableAsyncGenerators` | true | `-Xasync-generators` |
   | `compileFlags.enableGenerator` | true | default on; no option |
   | `compileFlags.enableTS` | per `.ts` extension | `-transform-ts` |
   | `compileFlags.format` | `EmitBundle` | implied by `-exported-unit` |
   | `compileFlags.includeLibHermes` | false | default; no option |
   | `compileFlags.strict` | unset by the producer | default |
   | `compileFlags.emitAsyncBreakCheck` | unset by the producer | default |
   | `flags.optimize` | true | the `-O` knob |
   | `sourceMap` argument | `""` | `-sm-comment=off` |

   **The scanner is a third party to this parity, and it currently agrees
   with neither.** `require_scanner.cpp:424` builds a bare
   `hermes::Context`, configuring only `setParseTS`, and `Context`'s defaults
   are `enableAsyncGenerators_{false}` and `enableES6BlockScoping_{false}`.
   Async generators are a *parse* error when that flag is off, so the scanner
   rejects any module containing `async function*` and the producer stubs it
   -- today, in the shipped bytecode path, for a file the compiler behind it
   would have accepted. Reproduced:

   ```
   $ hermes-node app.js
   PASS 1
   $ hermes-node --build-bundle=app.hbb app.js
   warning: cannot parse gen.js (1:60: async generators are unsupported);
     packaged as a module that throws when required
   $ hermes-node --bundle=app.hbb
   SyntaxError: gen.js: 1:60: async generators are unsupported
   ```

   The comment at `require_scanner.cpp:437-442` asserts the invariant this
   breaks -- "a failure here means the compiler will reject the same wrapped
   text for the same reason" -- and the scanner is in fact strictly stricter
   than the compiler it speaks for. Filed as `01a09e14-0a0e`.

   This is a **prerequisite**, not a side note. The failure policy above puts
   the whole tolerance decision in the scanner; a scanner that rejects valid
   modules therefore stubs them on both paths, and under a native build it
   would do so while the parity table above promises the opposite. So the
   parity table has a third column in practice: the scanner's `Context` must
   be configured from the same flags, and the three settings must live in one
   place so they cannot drift again.

   **Only the async-generator half of that actually bites the scanner**, and
   the distinction was established by experiment during implementation rather
   than by reading. `Context::getEnableES6BlockScoping()` is consulted in
   exactly one place in Hermes -- `lib/IRGen/ESTreeIRGen-stmt.cpp`, for
   loop-capture codegen -- and nowhere under `lib/Sema/`, `lib/AST/` or
   `lib/Parser/`. `SemanticResolver` pushes a scope for every block
   unconditionally. The scanner runs the parser and `sema::resolveAST` and
   never reaches IRGen, so the block-scoping flag is inert there: removing
   the setter and rebuilding changes nothing it computes. It is set anyway,
   to track the compiler's configuration and because the same struct drives
   the `shermes` command line, where the flag *is* load-bearing -- that is
   where the 3,3,3-against-0,1,2 measurement comes from. An earlier draft of
   this section claimed block scoping changed what the scanner resolves; it
   does not.

   `-sm-comment=off` is a parity setting, not merely a safe one: `shermes`
   defaults to `-sm-comment=file`, so a `//# sourceMappingURL=` comment --
   which plenty of published packages ship -- would make it load a source map
   and rewrite the names in the location table, while the bytecode path
   passes `sourceMap=""` and ignores the comment entirely.

   `-w` because a CommonJS module reports `primordials`, `internalBinding`
   and `process` as undeclared globals -- three warnings in the first hundred
   lines of `libjs-node/net.js` alone -- and these are warnings the bytecode
   path does not print either.

   The unit name is `hn_m` followed by the container module index, zero-padded
   to six digits (`hn_m000017`). The index, not the identity: `isValidSHUnitName`
   permits alphanumerics and underscore only, so an identity would have to be
   mangled, and a mangling is a second thing that can collide. The index is
   already unique, already the table's key, and already in the container.

3. **cc.** Compile the C with the kit's driver, through the existing
   `resolveDriver()` / `kit.manifest` / `--cc` / Clang-detection path, so a
   failure prints the same shell-quoted, pasteable command the link step
   already prints.

   ```
   <driver> -x c -std=gnu11 -c <tmp>/<index>.c -o <tmp>/<index>.o \
       -O<n> <manifest ccflags>
   ```

   **`-x c`, before the input, is mandatory.** `kit.manifest` records
   `cc: /usr/bin/clang++` -- it is the *link* driver, and `CMAKE_CXX_COMPILER`
   is what `make-kit.py` captures -- and a C++ driver compiles a `.c` file as
   C++. Measured on the generated C for `libjs-node/net.js`: five hard errors
   (`definition of variable with array type needs an explicit size`, two
   redefinitions, `cannot initialize a variable of type 'struct UnitData *'
   with an rvalue of type 'void *'`), and clean with `-x c`. A separate C
   driver is deliberately **not** recorded: the manifest's sysroot, `-arch`
   and sanitizer flags were captured from this driver, and a second binary
   reintroduces exactly the driver-versus-kit mismatch `driverCandidates()`
   exists to prevent. The `.s` assemble step is unaffected, assembly being
   language-neutral.

   `-std=gnu11` rather than the driver's default, so the dialect cannot drift
   with the compiler, and **gnu** rather than strict `c11` because the SH
   headers use GNU zero-length arrays (`SHLocals::locals[0]`). It belongs in
   the object cache key when that lands.

`shermes` is used purely as a JavaScript-to-C compiler and never invokes a C
compiler itself. That is not a preference: `SHERMES_CC`,
`SHERMES_CC_SYSCFLAGS` and `SHERMES_CC_INCLUDE_PATH` are baked in at CMake
time, and the last is an absolute path into the build tree and the source
tree, neither of which exists in a release. `CC` and `CFLAGS` are
environment-overridable and the include path is not, except by overriding
`CFLAGS` wholesale, which also discards `SHERMES_CC_SYSCFLAGS`. Running the
compiler ourselves avoids all of that and reuses machinery that already
exists for the payload `.s`.

### The generated assembly

One `.s`, an extension of `payloadAssembly()` in `lib/build-exe/build_exe.cpp`
with its existing `ObjectFormat` parameter: the container `.incbin` exactly as
today, in `.rodata` as today, plus the unit table -- which does **not** go in
`.rodata`.

A table of function pointers needs a relocation per entry, and in a
position-independent executable those are dynamic relocations applied at load
time. Putting them in a read-only section is the classic text-relocation
problem. The table therefore goes in `.section .data.rel.ro` on ELF, which
RELRO makes read-only after relocation, and `.section __DATA,__const` on
Mach-O. The payload itself stays where it is: it needs no relocations.

```
    .section .data.rel.ro
    .p2align 3
_hermes_node_native_units:
    .quad _sh_export_hn_m0000
    .quad 0                        ; a JSON module: no unit
    .quad _sh_export_hn_m0002
    ...
_hermes_node_native_unit_count:
    .quad 1483
```

(The leading underscore is Mach-O's; the ELF spelling omits it, and the ELF
file still ends with `.section .note.GNU-stack,"",@progbits` for the reason
already recorded in the single-executable design.)

Indexed by **container module index**, with a null for every record that is
not a compiled JavaScript module -- JSON, a native addon, a resolve-only
`package.json`. The bytecode `--build-exe` path emits the same two symbols
with a count of zero, which is what lets **one** `bundle_main.cpp` serve both
configurations with no weak symbols and no second entry object in the kit.

The final link is the existing one plus the N module objects, passed through
a linker response file (`@file`), since N is routinely four figures.

## Run time

`bundleLoadCallback` in `lib/bundle/bundle_run.cpp` gains one branch, ahead
of the bytecode path:

```
index = indexOf(identity)
  ... isRequirable / kNative guards, unchanged ...
  if kind == kJSON: return payload text            // unchanged
  if unit table has an entry for index:
      return hermes_init_sh_unit(env, table[index])
  ... bytecode path, unchanged ...
```

Nothing in `libjs/bundle-loader.js` changes. `Module._cache` remains the
loader's only cache, so a module reached both by an edge and by a computed
specifier is instantiated once and `delete require.cache[...]` still forces a
reload -- which for a native module means re-running `_sh_unit_init`, whose
second call on an already-registered unit re-runs `unit_main` and returns a
fresh wrapper function, exactly the semantics the bytecode path has.

A unit whose top level throws propagates the exception through `require()`
like any other module. There is no analogue of `fatalBadPayload()`: a linked
unit cannot be a corrupt payload, since a damaged object file fails at link
time or does not load at all.

Everything else about a produced executable is unchanged: the root is the
executable's own directory realpath'd, addon sidecars and the
`<root>/<identity>` escape hatch sit beside it, preloads run before the entry
in table order, `process.argv` is `[exe, exe, ...userArgs]`, and the closed
world is closed.

## The kit

Three additions, cut by the existing `hermes-node-kit` target, which
`check-hermes-node-js` already `DEPENDS` on:

- **`kit/shermes`** -- 2,778,232 bytes on macOS arm64 Release, linked against
  `libSystem`, `CoreFoundation` and `libc++` only.
- **`kit/include/`** -- the headers the generated C needs, which are few.
  `clang -MM` over a generated file names eleven files from the Hermes source
  tree -- ten headers and one `.def` -- plus the generated config header:

  ```
  hermes/Support/sh_tryfast_fp_cvt.h   hermes/VM/sh_mirror.h
  hermes/VM/SHRuntimeHermesValueFields.def  hermes/VM/sh_runtime.h
  hermes/VM/sh_config.h                hermes/VM/sh_segment_info.h
  hermes/VM/sh_legacy_value.h          hermes/VM/sh_small_hermes_value.h
  hermes/VM/sh_stack_frame.h           hermes/VM/static_h.h
  hermes/VMLayouts/sh_stack_frame_layout.h
  libhermesvm-config.h
  ```

  copied under their `hermes/...` paths, with `libhermesvm-config.h` at the
  root of `kit/include` so a single `-I{kit}/include` serves both. That
  config header is what carries `HERMESVM_COMPRESSED_POINTERS`,
  `HERMESVM_BOXED_DOUBLES`, `HERMESVM_LOG_HEAP_SEGMENT_SIZE`, `HERMESVM_GCKIND`
  and `HERMESVM_MODEL`; compiling the generated C against a mismatched copy
  would miscompute `SHRuntime`'s layout. `_SH_MODEL()` in the generated C
  turns that into a link error rather than silent corruption.
- **`kit.manifest` gains `ccflag:` lines** for the compile step, mirroring
  the existing `driverflag:` and `linkarg:` and taking the same `{kit}`
  substitution. In a normal build that is
  `-DNDEBUG -fno-strict-aliasing -fno-strict-overflow -I{kit}/include`;
  `SHERMES_CC_SYSCFLAGS` is empty by default and picks up
  `-fsanitize=address -fno-omit-frame-pointer` (or `-fsanitize=undefined`) in
  a sanitizer build, which the kit records for that configuration. A
  universal kit records its `-arch` pair here as well as in `driverflag:`.

  `-fno-strict-aliasing` and `-fno-strict-overflow` are not optional: the
  generated C reads and writes C++ objects through mirroring C structs, so
  unrelated types alias.

`version:` and the generation-tag check are unchanged, so a kit cut from a
different build is refused as it is today, and the commit-granularity caveat
recorded for `--build-exe` applies identically.

macOS releases ship no kit, so `build-native` is unavailable there for
exactly the reason `--build-exe` is -- not a new limitation, and it resolves
when that one does.

## Failure policy: two stub sites on the bytecode path, one classification on the native one

The native backend has no language restrictions; it compiles what the
bytecode backend compiles. The first tolerance is decided in code both paths
already share, **before `shermes` is ever spawned**: the scanner parses and
sema-resolves every module inside the CommonJS wrapper, and a module it
rejects is packaged as a throwing stub, with the existing wording and the
existing `reporter.stubbed()` counter:

```
warning: cannot parse %s (%s); packaged as a module that throws when required
```

The entry and any preload stay a hard error there, as today, both being
certain to run.

**This section originally claimed that scanner tolerance was the whole
story, and that claim was measured wrong (Task 17).** The reasoning was:
the one case that mattered, `import()` inside a `.cjs`, is a *parse* error
the scanner already catches, so both producers stub it from the same line of
code and a `shermes` failure past that point can only be a compiler defect.
Measured on `import(f)` as a bare statement, parse and compile agreed
exactly:

```
$ shermes -emit-c dyn.cjs
dyn.cjs:1:11: error: Invalid expression encountered
$ hermesc -emit-binary dyn.cjs
dyn.cjs:1:11: error: Invalid expression encountered
```

That measurement was real but incomplete. `@babel/core`'s actual file
(`lib/config/files/import.cjs`) does not use `import()` as a statement; it
*returns* the value, `return import(filepath);`, from inside an ordinary
function -- and that form **parses**. The scanner accepts it (parsing and
sema both succeed), the module is discovered and queued for compilation like
any other, and it is IRGen -- which the scanner never runs -- that rejects
`import()` as an expression. The bytecode producer already has a second
stub site for exactly this shape, at the point `hermes_compile_to_bytecode`
reports the failure:

```
warning: cannot compile %s (%s); packaged as a module that throws when required
```

The native path had no equivalent, so a `shermes` failure on a discovered
but never-executed `import.cjs` was a hard build error -- which is what made
`build-native` unable to compile most real Babel-based programs (every
`@babel/core`-based plugin graph reaches this file; see the measurement
below and dz `01a0a0d6-03d4`, filed when this was found in Task 16).

**The fix classifies the subprocess result instead of treating every
`shermes`/`cc` failure alike**, using pieces that already existed for a
different reason. Task 5's `CommandResult` already distinguishes a
subprocess that ran and exited (`Exited`, with a status) from one that was
killed (`Signalled`), one that never started (`SpawnFailed`) and one
`waitpid` itself failed on (`WaitFailed`) -- precisely the "crashed
compiler, not a source problem" cases this section worried about being
unable to separate from a source rejection. A **source rejection** --
`Exited`, non-zero, with diagnostic text captured -- on a module that is
neither the entry nor a preload (both still get the hard error
unconditionally, being certain to run) is recompiled in place as a throwing
stub carrying the diagnostic, under the identical warning wording above, and
counted in the same `stubbedModules`. Everything else -- `Signalled`,
`SpawnFailed`, `WaitFailed`, an exit with no diagnostic text at all, or a
tool that exited 0 and wrote nothing -- says something about the toolchain
or the machine rather than the source, and stays a hard build error exactly
as before: turning a crashed compiler or a full disk into a module that
throws `SyntaxError` at run time would be strictly worse than stopping. The
classification lives in `lib/bundle/bundle_build_native.cpp`, the one place
that already decides entry-vs-preload for the scanner-failure case above;
`lib/build-native/job_pool.cpp` (the job pool Task 10 added) only reports
what happened, through the same `CommandResult` fields, and does not itself
decide what a failure means. It applies identically to the `cc` step, which
can reject generated C for reasons of its own (a header the kit did not
ship, for instance) that have nothing to do with the module's source.

Mechanically, this needed no change to `runCommand()`'s own capture, which
Task 5 already built as a structured result -- exited with status, killed by
signal, or failed to exec -- with the child's stdout and stderr **drained
while it runs**, not after `waitpid`, so a compiler printing more than a
pipe buffer's worth of diagnostics does not deadlock. What was missing was
reading that structure at the one call site that decides a build's fate
(`bundle_build_native.cpp`'s step 7) rather than only formatting it for a
hard-error message, and reusing `makeThrowingStub` (moved to external
linkage for this) rather than writing a second copy of the throwing-stub
source and its warning wording.

One change applies to **both** producers rather than to native alone: the
stub count is printed in the end-of-build summary unconditionally, not only
under `--verbose`. A build that quietly turned three modules into throwing
stubs should say so in its last line.

## Stack traces: the string is right, structured CallSites are not

Both halves measured with `shermes -exec` on a three-frame throw, rather than
assumed.

The plain `Error.stack` string is correct, and `-g2` is what makes it so:

| | `e.stack` frame |
| --- | --- |
| `-g0` | `at inner (native)` |
| `-g1` | `at inner (st.js:1:35)` |
| `-g2` | `at inner (st.js:1:35)` |

With `-source-name=<identity>` supplying the name, a native build's traces
read `node_modules/foo/index.js:12:3`. That differs from a bytecode bundle,
which passes `sourceUrl` at load time and so roots traces at the container's
directory; a native build bakes the name in and has no run-time root
available. Relative identities are the better of the two answers: they are
location-independent and they are what the container calls the module.

**Structured CallSites lose their locations, at every `-g` level.** With
`Error.prepareStackTrace` installed, a native frame gives:

```
getFileName()=null getLineNumber()=null getColumnNumber()=null
getFunctionName()=inner
```

This is inherent in the current VM: `JSError::constructCallSitesArray` says
in a comment that CallSites are supported only for regular stack traces and
not native ones, and `JSCallSite::getStackTraceInfo` returns a
`BytecodeStackTraceInfo *`, which a native frame has none of. Plain trace
formatting supports native frames by a separate path, which is why the two
halves disagree.

Phase 1 **scopes this out explicitly** rather than fixing it. It is a real
compatibility gap -- source-map-support, caller-location helpers, some error
reporters and test frameworks read CallSites rather than the string -- but it
is far less universal than `error.stack`, and closing it means adding native
frame support to `JSCallSite`, which is a Hermes feature in its own right and
not a prerequisite for compiling modules.

The test **asserts the current behaviour** (`getFileName() === null` on a
native frame) rather than skipping it, so that fixing it later fails the test
and is noticed. A tracker issue records the gap. Testing only the `e.stack`
string, which is what this design originally proposed, would have passed
while missing the regression entirely.

## Measurements

`libjs-node/net.js`, 69,770 bytes of JavaScript, wrapped in the CommonJS
wrapper. macOS arm64, Release `shermes`, Apple clang.

| form | bytes | time |
| --- | --- | --- |
| bytecode, `hermesc -O` | 43,475 | -- |
| generated C | 734,772 | 43 ms (shermes) |
| object, cc `-O3` | 322,368 | 1.04 s |
| object, cc `-O1` | 335,608 | 0.90 s |
| object, cc `-O0` | 439,672 | 0.27 s |

Three things follow. The native object is about **7.4x** the bytecode, so a
native artifact is substantially larger than the equivalent
`--build-exe` one. The C compile **dominates**, at roughly 24x the shermes
step, which is why the job pool covers both stages and why the follow-up work
is an object cache rather than anything to do with shermes. And `-O3` buys
about 4% of size over `-O1` for 15% more time on this file -- a single data
point, not a corpus, which is why `-O3` stays the default and the knob exists.

### End-to-end, small scale: tetris

Same machine as above (macOS arm64, Release `hermes-node`/`shermes`, Apple
clang), `examples/tetris/play.js`: 31 modules (22 JS, 9 JSON, 0 native).

| artifact | build wall clock | size |
| --- | --- | --- |
| native, `-O3` (default) | ~4.3 s | 13,738,152 bytes |
| native, `-O0` | ~3.9 s | 13,735,160 bytes |
| bytecode `--build-exe` | 0.34 s | 12,568,712 bytes |

The whole-artifact ratio is **~1.09x**, far below the 7.4x single-file
object ratio above -- exactly as expected, since the linked runtime is the
same ~12.5 MB in both and dominates a 22-module program. `-O0` was
predicted "roughly 4x faster" than `-O3`; measured, it is **not**: about
10% faster, not 4x. The reason is visible in the per-module compile log --
of the 4.3 s total, one file (`lodash.js`, 546 KB source) costs 4.05-4.19 s
of it, and for a file that large the C **frontend** work (parsing 3.1 MB of
generated C, IR construction) dominates over backend optimization passes,
so `-O0` mainly saves optimizer time it was never spending much of. The
smaller modules in the same build show no consistent -O0 speedup at all --
several are slightly *slower* at `-O0` -- because their compile time is
dominated by fixed per-invocation `shermes`/`cc` process overhead
(~150-200 ms each), not by anything an optimization level touches. The 4x
prediction holds for optimizer-bound code; it does not hold for a graph
whose cost is either one huge file's frontend time or many small files'
process-spawn overhead, which in practice is most graphs.

Startup (median of 11 runs, a fixture requiring the same ~21-module
subgraph and exiting -- see below): bytecode-exe 15.81 ms, native 16.07 ms.
No measurable difference at this scale.

### End-to-end, ~1,500 modules

`examples/flow-bundler`'s own entry (`bundler/buildBundleCLI.js`) **cannot
be used for this measurement at all**, confirmed rather than assumed: it is
Flow-typed ESM, which fails to parse under `build-native` with the exact
same error the example's own README documents for `--build-bundle`
(`'from' expected`), because both paths share one scanner and one Hermes
parser. Substituted a throwaway fixture in the same directory, requiring
flow-bundler's own real `node_modules` dependencies directly (`@babel/core`,
`@babel/preset-env`, `@babel/generator`, three `@babel/plugin-transform-*`
packages, `@babel/register`, `@babel/types`, `prettier`, `string-width`,
`babel-plugin-syntax-hermes-parser`, `hermes-estree`, `hermes-transform`) --
a real, not synthetic, dependency graph, just entered from a different file
since the bundler's actual entry point cannot be statically compiled.

That graph also could not be built as first assembled: `@babel/core`
ships `lib/config/files/import.cjs`, `return import(filepath)` inside a
`.cjs` file, which **parses** fine but is rejected in IRGen (not a parse
error, despite what this doc claimed at the time -- see the "Failure
policy" section above for the correction). `--build-bundle` tolerates it
(not the entry, so it is packaged as a module that throws if ever
required); at the time of this measurement `build-native` did not -- a
`shermes` failure on ANY discovered module was an unconditional hard build
error, and `import.cjs` is discovered by static scan whether or not the
program's execution ever reaches it. Since virtually every
`@babel/core`-based plugin graph reaches this exact file, this meant
`build-native` could not compile most real-world Babel-based programs
without a workaround. For this measurement the one file was replaced with
an equivalent that throws synchronously instead of using `import()`
(outside `node_modules`, gitignored, not committed); see dz `01a0a0d6-03d4`,
filed for this and fixed by Task 17 -- the classification in "Failure
policy" above packages this exact file as a throwing stub and lets the
build proceed, so the workaround this measurement needed is no longer
necessary.

1,745 modules total, 1,541 compiled by `shermes` (the rest JSON). macOS
arm64, Release, `--jobs=16` (this machine's core count):

| artifact | build wall clock | peak RSS during build | size | startup (median of 11) |
| --- | --- | --- | --- | --- |
| native | 460 s (7.7 min) | 3.66 GB | 94,103,352 bytes | 133.82 ms |
| bytecode `--build-exe` | 0.4 s (from an already-built bundle) | -- | 30,668,088 bytes | 136.47 ms |

The whole-artifact size ratio is **~3.07x** here, well above tetris's
1.09x: at this scale the linked runtime is a shrinking fraction of a much
larger program, so the native/bytecode ratio grows toward the single-file
7.4x figure as the program grows, rather than staying fixed. Startup shows
**no measurable difference** between native and bytecode at this scale
either -- the two medians are within each other's run-to-run noise. The
peak RSS figure is the `build-native` process's own peak (`getrusage`
`ru_maxrss`), which parses and resolves the whole graph in-process before
spawning any `shermes`/`cc` child; it is not the sum across the 1,541
spawned compiler subprocesses.

### Full-GC pause versus unit count

Cannot be measured from a produced executable at all: `-gc-print-stats` is
one of the flags this runtime refuses by name (see "Hermes VM Options" in
`CLAUDE.md`), there is no `global.gc()`, and timing the whole process
measures process startup, not a collection. Measured one level down
instead, with a `DISABLED_` GTest
(`StaticHUnitTest.DISABLED_FullGCPauseVersusUnitCount`, run by hand with
`--gtest_also_run_disabled_tests`) that hand-registers 1, 100 and 1,000
units, each with a real 200-entry symbol table (all units share one ASCII
pool and one strings table -- only each unit's own symbols array, property
caches and index variable are distinct, since that is what the GC actually
walks), then times one `Runtime::collect("benchmark")`. macOS arm64:

| units | Release | ASAN+handle-sanitizer (`cmake-build-asan`) |
| --- | --- | --- |
| 1 | 0.148 ms | 1.62 ms |
| 100 | 0.217 ms | 2.47 ms |
| 1,000 | 0.925 ms | 11.4 ms |

Release, five repeated runs, was stable to within a few percent. The curve
is linear once a small fixed cost is subtracted: fitting `base + n *
per_unit` against the Release numbers gives roughly a 0.14 ms fixed
component and about 0.00078 ms (0.78 µs) per additional unit at 200
symbols each -- consistent between the 1->100 and 100->1,000 steps. This
confirms the design's worry directionally (pause time does grow with unit
count, not just with live data) but the absolute cost is small at this
symbol-table size: roughly 0.8 ms of extra full-GC pause per 1,000
initialized units. It would matter more for units with much larger symbol
tables, or for a program that forces many full collections, neither of
which this benchmark's fixed 200-symbol units represent.

## Diagnostics

`--verbose` narrates to stderr: the kit, the resolved `shermes` and driver
(and why that driver was chosen, including when the recorded one was tried
and rejected), then per module the source bytes, C bytes, object bytes and
timing plus both commands, then the link command, then a summary -- modules
compiled, stubs, total object bytes, total wall clock, final binary size.

Without `--verbose`: the shared producer's discovery warnings (computed
`require`, escaped `require`, unpackageable files), the stub warnings, and
the summary line.

A failing `shermes`, `cc` or link prints the whole command shell-quoted
through `formatCommandLine()`, for the reason that function exists:
reproducing the failure by hand is how anyone diagnoses a toolchain problem.

`--keep-temp` retains the whole temp directory -- staged sources, the `.c`
files, the objects, the generated `.s` and the serialized container. The C is
the artifact you actually debug when codegen is suspect, and it is deleted by
default because it is ten times the size of the source. Retaining the
container is why `kBundleFlagNativeUnits` exists: that is the one way a
payload-less container reaches a filesystem, and `openBundle()` refuses it by
name rather than calling it damaged.

## Implementation layout

- `lib/build-native/` -- `staging.cpp` (wrapped-source staging and
  staging-path derivation), `native_compile.cpp` (the shermes and cc
  invocations), `job_pool.cpp`, `unit_table.cpp` (the assembly the unit table
  is emitted as). Links `hermesNodeBundle` and `hermesNodeBuildExe`, both
  VM-free, and stays VM-free itself, which is what lets `BuildNativeTest` run
  with no runtime and no Hermes compiler.
- `include/hermes/node-compat/build-native/build_native.h`.
- `lib/bundle/bundle_build.cpp` gains a native mode rather than getting a
  parallel copy. Discovery, resolution, classification and container
  assembly must be **one** implementation: two builds of the same entry that
  packaged different graphs would be the same class of defect as a specifier
  resolving differently at build and run time, which is why there is one
  resolver with two backends rather than two resolvers. Only the payload step
  branches -- bytecode through `hermes_compile_to_bytecode`, or an object file
  through `lib/build-native/`.

  That split has a consequence worth stating: the shared half's `napi_env`
  parameter becomes optional and is **null** in native mode, because the only
  things that use it are `hermes_compile_to_bytecode` and
  `takeCompileErrorText`, both on the bytecode payload path. Parse failures
  come from the scanner, which needs no env. So `build-native` creates no
  runtime, no event loop and no `napi_env` -- consistent with the rule that a
  verb needing no runtime runs before one exists -- even though it does link
  the Hermes parser and sema through `hermesNodeBundleBuild`, and therefore
  `hermesvm_a`. The implementation must confirm that nothing else in the
  shared half reaches for the env.
- `tools/hermes-node/hermes-node.cpp` -- subcommand dispatch in `main()`
  before the parse loop, beside `cache`. Unlike `--build-bundle`, which runs
  inside `runHermesNode` because it needs a runtime to compile bytecode, this
  one does not.
- `lib/build-exe/build_exe.cpp` -- `payloadAssembly()` grows the unit table;
  the compile-a-C-file step and `ccflag:` parsing are added beside the
  existing assemble step.
- `lib/bundle/bundle_run.cpp` -- one branch in `bundleLoadCallback`, and the
  unit table plumbed in from `bundle_main.cpp`.
- `utils/make-kit.py` -- copy `shermes` and the headers, write `ccflag:`.
- Hermes, three changes: `sh_runtime.h` / `StaticHUnit.cpp` / `Runtime.cpp`
  for the growable units array; a new
  `hermes/API/napi/hermes_napi_sh_unit.cpp` with its declaration in
  `hermes_napi.h`; and `-source-name=` in `tools/shermes/` (the flag itself
  in `shermes.cpp`, applied where `memoryBufferFromFile` result is handed to
  `addNewSourceBuffer`).

## Testing

A new lit feature `shermes-available` (`test/lit.cfg`): true when the kit
directory holds a runnable `shermes`. Required together with
`linker-available`.

- `test/build-native.js` (`REQUIRES: linker-available, shermes-available`) --
  hello world; requires across directories; a JSON require; a module packaged
  as a throwing stub; `process.argv`; `process.exitCode`; an `e.stack` string
  naming the module by its identity with a line and column; and, asserting
  the known gap rather than skipping it, `getFileName() === null` on a native
  frame under `Error.prepareStackTrace`.
- `test/build-native-parity.js` -- the forcing function for the parity table,
  in the spirit of `bundle-builtins.js`: one fixture with a row per compile
  flag (a `let`-in-loop closure for block scoping, an async generator, a
  plain generator, a `.ts` module), run plain and run natively, outputs
  diffed, so a divergence fails here instead of in somebody's program. Output
  comparison cannot prove the optimization level reached the child, so the
  unit test below asserts the constructed argv for `-O` separately.
- `test/build-native-wasm.js` (`REQUIRES: wasm, linker-available,
  shermes-available`) -- a native executable instantiating a module, covering
  both a `--bake-wasm` hit and an unbaked disk-cache hit, asserted from
  `HERMES_NODE_DEBUG_NATIVE=COMPILE_CACHE` tracing and never from timing.
- `test/build-native-errors.js` -- deliberately **ungated**, like
  `build-exe-errors.js`, and says so in its header: every case is refused
  before the toolchain is reached (missing `-o`, missing entry, `--jobs=0`,
  `--record-wasm`, an entry that does not parse, an
  `--include` that does not resolve), so this is the coverage that survives a
  kitless checkout.
- `test/build-native-escapes.js` -- the closed world is still closed, mirroring
  `bundle-escapes.js` and `build-exe-escapes.js`.
- `test/build-native-natives.js` -- an addon sidecar beside the produced
  executable, and a recorded addon whose sidecar is missing still throwing
  `MODULE_NOT_FOUND`.
- `unittests/BuildNativeTest.cpp` -- unit naming; unit-table assembly for
  both object formats, including the all-null bytecode case and the section
  directive each format uses; the constructed `shermes` argv, asserting every
  parity-table row and the `-O` mapping is present; the constructed cc argv,
  asserting `-x c` precedes the input; the linker response file; and the
  subprocess result classification, including that a captured stream larger
  than a pipe buffer does not deadlock.
- `examples/tetris` gains a native arm in its `run.sh`, checked with
  `node_modules` moved aside, as that example already does for `--build-exe`.
  tetris is the right first case for the same reason it is the right
  `--build-exe` case: entirely CommonJS, no computed requires, no addons.

Hermes-side: the growable units array needs the four cases listed with that
change, the high-index-first-in-runtime-B one especially; `-source-name=`
needs a lit test asserting the name reaches a stack trace, and one asserting
it is refused with more than one input file. The existing `shermes` lit tests
cover the single-unit path.

## Deliberately not in scope

- **Built-in JavaScript stays embedded bytecode and interpreted.** The
  executable still carries the interpreter and the embedded bytecode for
  `libjs/`, `libjs-node/` and the shims. Compiling those to units at
  hermes-node build time is the obvious next phase and is independent of
  everything here.
- **WebAssembly caching is not degraded, and this design originally claimed
  it was.** A native executable installs the Wasm cache hooks and uses the
  disk cache exactly as a `--build-exe` one does -- that is what
  `test/test-wasm-cache-build-exe.js` pins -- and baked entries live in the
  v6 container this design already reuses, which
  `test/test-wasm-bake-build-exe.js` pins. So `--bake-wasm` is **accepted**,
  not refused: refusing it would throw away a capability the artifact already
  has, for no technical reason. Only `--record-wasm` is refused, because
  `build-native` never runs the program, which is the same reason
  `--build-bundle` refuses it. What remains genuinely untested is native code
  and Wasm in one process, so a native Wasm test is part of this work,
  covering both a baked hit and an unbaked disk-cache hit.
- **No object cache.** Every build recompiles every module. The key would be
  a digest over the wrapped source, the shermes flags, the cc flags and the
  kit version, stored under the existing compile-cache tree beside the
  JavaScript and Wasm tiers; the follow-up is mechanical and changes no
  interface here. Given the measurements, it is the single largest usability
  win available after this lands.
- **No cross-module optimization.** One unit per module forecloses inlining
  across module boundaries. That is the price of the parallelism above and of
  per-module incrementality later. Chunking several modules into one unit is
  now an option rather than an impossibility, since the eight-unit cap is
  gone.
- **No runnable or inspectable native container.** A native container is a
  build intermediate, not an artifact: it is serialized to the temp directory
  so `.incbin` has a path, and it is deleted unless `--keep-temp` retains it.
  It cannot be run -- `openBundle()` refuses it by name -- and no tool verb
  reads it, so there is no `--dump` for one, even though
  `openForInspection()` is left able to accept it so that a later verb can be
  added without a format change. To see what a native build will package,
  build a bytecode bundle of the same entry -- the same producer, the same
  discovery, the same answers -- and `--dump` that.
- **Windows, cross-compilation, universal macOS binaries.** Inherited from
  `--build-exe` unchanged, including the warning `make-kit.py` prints when
  handed a multi-architecture link line.
