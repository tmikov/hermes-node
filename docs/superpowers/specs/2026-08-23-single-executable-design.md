# Design: single-file executables

**Status:** Draft for review, 2026-08-23.

Follows `docs/superpowers/specs/2026-08-15-aot-bundle-design.md` and the rounds after
it, which turned a `require()` graph into one container that runs with no
source tree. This settles how that container becomes an executable.

Spike results this design rests on:
`docs/notes/2026-08-23-macos-partial-link-spike.md` (handoff) and
`docs/notes/2026-08-23-macos-partial-link-results.md` (measurements).
Both were produced on a Mac, on branch `work-mac`, and carried here.

## The problem

`--bundle=app.hbb` still needs two files that both have to arrive: a
`hermes-node` the user installed, and the container. The unit of distribution
is "a runtime, plus a file" -- which is a worse story than the one bundling
was built to tell.

Everyone who ships single-file executables solves this the same way, and all
of them pay the same tax:

- **Node.js SEA** builds a preparation blob (`--experimental-sea-config`) and
  an *external npm tool*, `postject`, injects it into a copy of the `node`
  binary: an ELF note, a Mach-O section in segment `NODE_SEA`, or a PE
  `RT_RCDATA` resource. Presence is signalled by a **sentinel fuse** -- a
  `volatile` string `POSTJECT_SENTINEL_...:0` whose last byte is flipped to
  `1`, so `postject_has_resource()` costs one byte load
  (`deps/postject/postject-api.h:42`). The docs instruct the user to
  `codesign --remove-signature` first and re-sign after. The payload is one
  CommonJS script; its `require()` "can only be used to load built-in
  modules".
- **Deno `compile`** injects into `denort` (a stripped runtime) using
  `libsui` (`denoland/sui`). ELF gets a `.note.sui` section placed inside a
  `PT_LOAD`. Mach-O **arm64** gets a real `LC_SEGMENT_64` inserted before
  `__LINKEDIT`, every downstream offset shifted, and then an ad-hoc signature
  **hand-written in Rust**: SHA-256 page hashes and a CodeDirectory with
  `flags=0x20002` (adhoc | linkerSigned). Mach-O **x86_64** takes a cheaper
  route -- strip the signature, append, patch `__LINKEDIT` sizes, shell out
  to `codesign -s -`.
- **Bun `--compile`** appends a serialized module graph plus an `Offsets`
  struct and the trailer `"\n---- Bun! ----\n"`, in a real per-platform
  section. On macOS it **gives up on preserving the signature** (LIEF-based
  injection was too slow); the user re-signs by hand. On Windows it strips
  Authenticode outright and recomputes the PE checksum.

The tax is macOS. Apple TN2206 states that appending to a Mach-O is expressly
prohibited and verification will fail, and on Apple Silicon a binary whose
signature does not verify is SIGKILLed rather than run.

## The decision

**We link a new executable instead of injecting into a prebuilt one.**

The container becomes an ordinary object file via `.incbin`, and the system
linker produces the executable.

The observation that makes this obviously right: sui's hand-built
CodeDirectory carries the `linkerSigned` flag. **sui is reimplementing what
`ld64` already does.** Use the linker and it is free -- measured on macOS
26.6.1 / arm64 / ld-1266.8:

```
$ codesign -dvvv /tmp/hn2
CodeDirectory v=20400 size=96092 flags=0x20002(adhoc,linker-signed) ...
Signature=adhoc
$ codesign --verify --verbose /tmp/hn2
/tmp/hn2: valid on disk
/tmp/hn2: satisfies its Designated Requirement
```

Identical flags to the binary CMake itself produces. It runs on Apple Silicon
with no `codesign` step. A **payload-carrying** binary -- a real `.hbb`
linked in through `.incbin` -- is likewise ad-hoc linker-signed, verifies, and
runs.

There is no fuse, no sentinel, no self-mmap, no `/proc/self/exe`, no backwards
scan for a magic, and no Mach-O surgery. The payload is a symbol.

### The cost, stated plainly

This needs a **linker on the build machine**, and on macOS the Xcode command
line tools. Bun and Deno specifically avoid that; it is the one thing
injection buys. We accept it deliberately: Static Hermes native compilation
requires a linker anyway, so this is not a new prerequisite -- it is an early
consumer of one we are already committed to.

The related cost is **cross-compilation**. Injection is a pure file rewrite,
which is why Bun offers eight targets from any host. Linking needs a
cross-linker and a sysroot. We do not offer cross-compilation and this design
does not add it.

## What is proven

Linux x86_64 (GNU ld, `cmake-build-release`) and macOS arm64 (ld-1266.8),
both against the full 42-input static closure:

| | Linux | macOS arm64 |
|---|---|---|
| partial link of the closure | 0.3 s, 20.8 MB object | 0.15 s, 14.1 MB object |
| relinked binary vs CMake's | same size | +3.9% (see below) |
| `napi_` / total dynamic exports | 145 / 3717, unchanged | 145 / 2719, unchanged |
| exported symbol *sets* | -- | byte-identical, diff produces nothing |
| `.node` addon `dlopen` | works | works |
| full lit suite vs control | identical failure set | identical failure set |
| ad-hoc signature | n/a | `adhoc,linker-signed`, verifies |
| payload-carrying binary | -- | signed, verifies, runs |

The relinked-binary dependency sets are identical to CMake's on both
platforms.

## The kit

The produced executable is the full runtime plus one bundle. Since the
runtime is **full** (`hermesvm_a`, not `hermesvmlean_a` -- so `eval` and
`new Function` behave exactly as under `--bundle`), the object closure is
**fixed**. Only the payload varies. That is what makes a shippable kit
possible at all.

The kit is three things:

1. `libhermes-node-kit.a` -- every archive in the closure, merged.
2. `libhermesNapi.a` -- kept separate, because it is genuinely `-force_load`ed
   so that `dlopen`ed addons can resolve the NAPI surface.
3. `hermes-node-bundle-main.o` -- the entry object for a bundled app.

plus `kit.manifest`, described below.

### Why merged, and not a directory of 41 archives

Shipping the archives exactly as CMake produces them, with a linker response
file, *is* the original link -- zero risk by construction. It was seriously
considered and rejected for one reason: **a response file bakes in 41 paths
that must all be present and correctly laid out.** That is 41 chances for a
packaging bug whose symptom is undefined symbols at *the user's* link, which
is the worst place to discover it. One archive is one chance, and a kit you
can `ls` and describe is one you can verify shipped correctly.

Secondary: `ld` resolves an archive to a fixpoint *within* that archive, so
merging removes link-order sensitivity entirely rather than preserving it by
construction.

The merge is one command per platform -- `libtool -static` on Apple, `ar -M`
with an MRI script on Linux -- the same shape of conditional as the
`-force_load` / `--whole-archive` split `tools/hermes-node/CMakeLists.txt`
already carries.

Measured on macOS: the merged-archive link produced a binary **8 bytes
smaller** than CMake's own, with identical exports, a working addon, and the
same lit results. (Eight bytes is almost certainly a path-length or
identifier difference; it was not chased and this is not a bit-for-bit
equivalence claim.)

**Two constraints that fall out of merging, to write down rather than
rediscover:**

- **Entry objects stay out of the merge.** If `hermes-node.cpp.o` were inside
  the archive, `main` would be pulled from it and the two link configurations
  below would collapse into one.
- **Nothing may `-force_load` the merged archive.** Merging preserves
  duplicate symbols that the real link never sees, because it never pulls both
  members: `hermes::vm::matchTypeOfIs` appears in two `Operations.cpp.o`
  members, `llvh::DisplayGraph` in two `GraphWriter.cpp.o`. Harmless as an
  ordinary archive; a duplicate-symbol failure the moment something force-loads
  it. This is why `libhermesNapi.a` is item 2 and not part of item 1.

### Why not `ld -r`

A single pre-linked relocatable object was the first candidate: it is smaller
(14.1 MB against the merged archive's 28 MB) and it is one file rather than
three. It is rejected because of what it costs the **produced** binary on
macOS.

`ld -r` drops `MH_SUBSECTIONS_VIA_SYMBOLS` from the object's header. Mach-O
has no per-function sections; that flag is what grants the linker permission
to synthesize atoms from symbols. Without it the final link can neither fold
identical code nor dead-strip:

| link | size | note |
|---|---|---|
| original inputs | 11,915,960 | baseline |
| original inputs `-no_deduplicate` | 12,385,960 | within 2,712 bytes of the `-r` result |
| from the `-r` object | 12,383,248 | +3.9% |
| original inputs `-dead_strip` | 10,890,840 | -8.6%, unreachable via `-r` |
| from the merged archive | 11,915,952 | folding intact |

Against a folded, dead-stripped link the `-r` route costs about 12%.

The loss is **macOS-only**, and the asymmetry is worth understanding rather
than memorizing. ELF's granularity unit is the section, and `ld -r` preserves
sections, so nothing is lost. Measured on Linux:

| Linux link | size |
|---|---|
| original inputs, plain | 13,963,320 |
| original inputs + `--gc-sections` | 12,841,280 |
| `-r` object, plain | 13,963,312 |
| `-r` object + `--gc-sections` | 12,845,800 |

The `-r` object dead-strips to within 4,520 bytes (0.035%) of the original
inputs. `ld -r` would be free on Linux. We take the merged archive on both
platforms anyway: one story is worth more than 14 MB of kit.

### Why not LTO

LTO emits bitcode objects, and linking those requires a matching LLVM linker
plugin. Shipping a kit turns "you need a linker" into "you need a linker whose
LLVM version matches ours" -- a real toolchain-compatibility requirement that
does not otherwise exist here. Explicitly out of scope.

(Dead-stripping is a separate matter, enabled for the ordinary `hermes-node`
link in `551d4c8` independently of this design: `--gc-sections` / `-dead_strip`,
worth 1,131,376 bytes (-8.10%) on Linux. It does not weaken `-rdynamic` --
a symbol in the dynamic symbol table is a GC root, and the two sorted export
name lists diff empty across the change. It applies to a produced executable
too, where it is worth more, since an app links neither the CLI nor the tool
verbs.)

### `kit.manifest`

A small text file beside the kit recording what it is: the hermes-node
version it was cut from, and the exact ordered argument list to place after
the user's objects -- the force-load spelling, the kit archive, and the system
libraries and frameworks for that platform (`-lresolv -lpthread -lm -framework
CoreFoundation` on macOS; `-lpthread -ldl -lrt -lm` plus ICU on Linux).

Its first job is to let `--build-exe` construct a correct link line with no
hardcoded per-platform list. The spike found `-lresolv` present on macOS and
absent from the command I had guessed, and `-ldl` the reverse -- exactly the
class of error a manifest removes, because the launcher that writes it *is*
the thing intercepting CMake's real link line, so it cannot drift from what
actually links.

Its second job is version agreement. **It does not record the bundle
generation tag** -- that tag is computed in C++ from the bytecode version and
build configuration, and is not available to CMake, which writes this file.
The check that matters is done differently and is strictly stronger:
`--build-exe` opens the container through `BundleReader::open()` with its
**own** `bundleGenerationTag()`, so a container built by a different
hermes-node is rejected at build time with a tag mismatch. The manifest's
`version:` line covers the other half of the matrix -- a *kit* from a
different build than the binary reading it -- and is compared against the
running binary's own version string. Between them: a container the produced
executable could not run, and a kit that does not match the producer, are both
build-time errors rather than a binary that refuses its own payload at
startup.

## The payload object

`--build-exe` generates an assembly file and assembles it:

```asm
        .section __DATA,__const        # Mach-O;  .section .rodata on ELF
        .globl _hermesNodeBundleStart  # no leading underscore on ELF
        .p2align 4
_hermesNodeBundleStart:
        .incbin "app.hbb"
        .globl _hermesNodeBundleEnd
_hermesNodeBundleEnd:
```

`.incbin` rather than a generated C array: a multi-megabyte array would
explode compile time for no benefit, and `.incbin` is equally portable across
both assemblers.

Alignment is `.p2align 4` (16 bytes), comfortably above the format's
`kBundlePayloadAlign` of 8
(`include/hermes/node-compat/bundle/bundle_format.h:26`). Payload offsets
inside the container are relative to its start, so aligning the start is
sufficient for bytecode to be executed in place out of the mapping.

The section is read-only and mapped by the loader. `BundleReader` already
maps read-only, so nothing in the reader wants to write into it.

Size is `end - start`, not a stored length: the linker computes it and it
cannot disagree with the payload.

## Two link configurations

No fuse and no weak symbols. The two products link different things:

```
hermes-node   =  hermes-node.cpp.o
                 -force_load libhermesNapi.a  libhermes-node-kit.a  <syslibs>

myapp         =  blob.o  hermes-node-bundle-main.o
                 -force_load libhermesNapi.a  libhermes-node-kit.a  <syslibs>
```

An app therefore carries no CLI argument parser and none of the tool verbs,
which dead-stripping turns into a real size difference rather than a
rounding error. A produced executable's positional arguments belong to the
bundled program, as they already do under `--bundle`.

`-rdynamic` / `-Wl,-export_dynamic` is applied to the app link too, so
`dlopen`ed addons resolve the NAPI surface exactly as they do today.

## Run time

Almost nothing changes, because the reader was already written for this.

`BundleReader::open(const uint8_t *data, size_t size, tag, error)` is already
pointer-and-length (`include/hermes/node-compat/bundle/bundle_reader.h:43`).
The file mapping lives only in `openBundle()`
(`lib/bundle/bundle_run.cpp`). So the new entry point is:

```cpp
bool openEmbeddedBundle(const uint8_t *data, size_t size, std::string *error);
```

which runs the same validation chain, including the generation tag, and fails
the same way.

**The one thing that needs a new answer is the root.** `openBundle()` derives
it as `canonical(bundlePath).parent_path()`. There is no container path here,
so the root is the **directory of the executable itself**, realpath'd for the
same reason -- an executable reached through a symlinked directory resolves
identities against the directory it really lives in.

That keeps every downstream property intact and unchanged: the closed world,
the resolver and its two backends, preloads, and native addon sidecars, which
now sit beside the executable instead of beside the container. Nothing else in
`libjs/bundle-loader.js` or `lib/bundle/` moves.

## Flag surface

```
hermes-node --build-exe=<output> <bundle.hbb>
```

It takes **a container, not an entry script**. This is deliberate and it is
the smaller thing to build: `--build-bundle` already produces containers, and
the container that goes into the executable is inspectable beforehand with the
five verbs that already exist -- `--dump`, `--extract-module`,
`--verify-natives`, `--dump-bytecode`, and `--verbose`. Node's SEA blob has no
equivalent. Accepting a `.js` entry directly is a strictly additive change
later, dispatched on the magic bytes, and nothing here forecloses it.

Additional flags:

- `--kit=<dir>` -- override where the kit is found. Default is derived from
  the running binary's own location.
- `--verbose` -- narrate: kit location and manifest contents, container size,
  the generated assembly, the exact link command, and the resulting binary's
  size. To stderr, as the other verbose paths do.

Conflicts, to be added to `checkToolOptions()` in
`tools/hermes-node/hermes-node.cpp` alongside the existing matrix, each
naming both flags: `--build-exe` with `--bundle`, with `--build-bundle`, with
`-e`/`--eval`, with any of the four read-only verbs, and with
`--inspect`/`--inspect-brk`. An empty `--build-exe=` names the flag rather
than reporting a missing file with no filename in it.

`--build-exe` is dispatched by `runToolVerb()` **before `runHermesNode`**,
alongside the four read-only verbs. It writes files rather than only reading
them, so it is not read-only -- but the criterion for that dispatch point is
not read-only-ness, it is whether the verb needs a runtime, and this one does
not. It reads an already-compiled container, generates assembly, and runs the
toolchain; no parser, compiler, event loop or `napi_env` is involved. A verb
that fails for reasons belonging to a runtime it never needed is exactly what
that dispatch point exists to prevent.

## What does not work

- **Windows.** Deferred, because we cannot test it. Nothing here is
  Windows-hostile -- `.incbin` and a merged `.lib` both exist -- but it is
  unwritten and unclaimed.
- **Universal macOS binaries are unproven end to end.** `ld -r` accepts two
  `-arch` flags on a toy case, but nobody has linked a genuinely two-slice
  hermes-node closure; the tree tested was arm64-only. Release CI ships
  `macos-universal` today, so the kit must eventually be universal too.
- **`ld -r` and `lipo` will silently produce empty slices** for a missing
  architecture: warnings only, exit 0, a 216-byte slice with zero symbols that
  `lipo -info` reports as a real architecture. The failure surfaces as an
  undefined `_main` at the *customer's* final link. Whatever builds the kit
  must assert per-slice symbol counts and never trust exit status. This
  applies to `libtool`/`lipo` as much as to `ld -r`.
- **The Linux executable is not self-contained.** It needs ICU 74,
  `libstdc++.so.6` and `libgcc_s.so.1`. macOS needs only `libresolv.9`,
  `libSystem.B`, `CoreFoundation` and `libc++.1`, all OS-provided -- so a
  macOS single-file executable genuinely is one, and a Linux one is not.
  `HERMES_USE_STATIC_ICU=ON` would fix the ICU half at the cost of requiring
  a static system ICU at build time. Out of scope here, worth its own round.
- **Native addons still ship beside the executable**, exactly as they ship
  beside a container today. `dlopen(3)` takes a path; that has not changed.
- **The CodeDirectory `Identifier` is derived from the output filename.**
  Irrelevant for ad-hoc signing. If a produced executable is ever Developer-ID
  signed and notarized, the identifier becomes meaningful and should be set
  deliberately rather than inherited from whatever the user named the program.

## Testing

- `test/build-exe.js` -- build a container, build an executable from it, run
  it, check output. The end-to-end case.
- `test/build-exe-errors.js` -- a container that does not exist; a corrupt
  container; a generation-tag mismatch against the kit manifest; a missing or
  malformed kit; an unwritable output path; `--build-exe=` empty.
- `test/build-exe-tool-errors.js` -- every conflict row, both orders, each
  asserting that the message names both flags.
- `test/build-exe-natives.js` -- an executable whose bundle records an addon:
  the sidecar beside the executable loads; a missing sidecar throws
  `MODULE_NOT_FOUND` naming the file to ship, not `ERR_DLOPEN_FAILED`.
- `test/build-exe-closed-world.js` -- the escapes pinned by
  `test/bundle-escapes.js` stay shut in an executable: `(0, eval)('require')`,
  `Module.createRequire()`, a computed specifier, a `node_modules` decoy on
  disk beside the binary.
- A unit test that the produced executable's root is its own directory, and
  that a symlinked path resolves to the real one.

The suite must be skippable where no linker is available, the way
`bundle-yargs.js` is gated on `examples-installed`: a `linker-available` lit
feature.

## Not doing

- **Cross-compilation.** See above. It is the one capability injection has
  that linking does not, and it is not worth reversing the decision for.
- **A fuse or self-inspection.** Two link configurations make "am I an app?"
  a compile-time fact rather than a run-time question.
- **The lean runtime** (`hermesvmlean_a`). It would be smaller, but `eval`
  and `new Function` would throw at run time in the customer's hands. If size
  ever justifies it, it becomes an opt-in flag and a build-time warning driven
  by the existing scanner, which already identifies bindings.
- **Teaching the read-only verbs to open an executable.** `--dump` of a
  produced binary is plausible and cheap later; the container it was built
  from is right there and already inspectable, so nothing needs it yet.
- **Compressing the payload.** Bytecode is executed in place out of the
  mapping; compressing it would mean decompressing to anonymous memory at
  startup and paying for it on every launch, in an artifact whose reason for
  existing is startup cost.
