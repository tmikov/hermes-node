# Progress: Baking compiled WebAssembly into a bundle

Tracks `docs/superpowers/plans/2026-09-09-wasm-bundle-bake.md`
(spec: `docs/superpowers/specs/2026-09-09-wasm-bundle-bake-design.md`).

## Status

Complete. All eight tasks landed as their own commits on `work-wasm-cache`:

| Commit | What |
| --- | --- |
| `Add the Wasm record file format: BundleWasmRecord, WasmRecordWriter/Reader` | Task 1: the `--record-wasm` file format |
| `Add a Wasm table to the bundle container format (v6)` | Task 2: `BundleWriter::addWasm`, `BundleReader::wasmFor` |
| `Fix BundleWriter::addWasm digest length hazard from review` | a fix folded into Task 2, see below |
| `Add a WASM section to --dump and dumpWasmRecord() for --record-wasm files` | Task 3: the tooling half |
| `Bake --record-wasm entries into a container's Wasm table at build time` | Task 4: the producer |
| `Consult a container's baked Wasm at run time, and record what compiled` | Task 5: the run-time tier and the recorder |
| `Wire the CLI flags for baked Wasm: --record-wasm, --bake-wasm, --dump-wasm` | Task 6: the CLI surface |
| `Add end-to-end lit tests for the baked Wasm compile cache` | Task 7: the lit suite |
| `Fix review findings in the Wasm bake lit tests` | a fix folded into Task 7, see below |
| (this commit) | Task 8: documentation |

No Hermes change, as the design required: the `hermes_set_wasm_cache` hooks
the disk-cache work added are the entire mechanism this reuses.

## Follow-on work, within the plan

Two of the nine commits are review fixes rather than new tasks, and both are
worth recording for what they say about the design rather than just as bug
reports.

- **`BundleWriter::addWasm` took a `std::string_view` of arbitrary length**,
  but `serialize()` unconditionally `memcpy`'d a fixed `kNativeDigestBytes`
  (32) out of it -- a caller passing a shorter digest would read past the
  end of the view. Unreachable at the commit that introduced it (every
  caller passed exactly 32 bytes, from `BundleFormatTest`'s own fixtures),
  but a real gap the moment Task 5 wired a real digest through, and the
  *read* side (`BundleReader::wasmFor`) already guarded against exactly
  this length mismatch. Fixed by changing the parameter to a bare
  `const uint8_t *rawDigest`, matching `WasmRecordWriter::record()`'s
  signature rather than `addNative()`'s `std::string_view` -- deliberately,
  because a Wasm digest is always exactly `kNativeDigestBytes` with no
  length carried beside it, where a native's digest goes into the
  length-checked string table. A type that cannot express the wrong length
  is stronger than a runtime check of it.
- **Three issues surfaced reviewing the lit suite itself**, none of them in
  the feature: a missing duplicate-digest bake case (two `--bake-wasm`
  files sharing a digest -- first file wins, `--verbose` says so, and both
  digests must still resolve at run time, which is what actually catches a
  dropped `continue` after the dedupe branch: without it, two records under
  one digest land in a table a lookup binary-searches, a silent
  format-invariant break rather than a visible one); a test header comment
  that named the wrong regression (`test-wasm-bake-build-exe.js` pins that
  the container tier answers before a live cold disk cache would, not that
  the cache is disabled -- that case belongs to `test-wasm-bake.js`); and a
  GNU-only `dd ... status=none` plus an unquoted `$(find ...)` replaced with
  the project's existing `for f in $(find ...)` house style. Also: naming a
  check prefix `DUPRUN` broke the test, because lit's RUN-line scanner
  matches the substring `RUN:` anywhere with no word boundary -- renamed to
  `DUP2`.

## Measurements

**`cmake-build-release`** (`CMAKE_BUILD_TYPE=Release`, Clang), Linux x86_64,
on the machine this session ran on. `cmake-build-asan` is a debug ASAN
build; its absolute timings are not representative and are not used here.
Each figure is the median of three runs that agreed to within 0.02 s.

### examples/hermes-parser-ast-wasm

One Wasm module (`HermesParserWASM.js`, a 2,054,308-byte bytecode entry in
this build -- close to, but not identical to, the 2,054,332 bytes the disk
cache's own progress file measured on a different build).

| Run | Container | Disk cache | seconds |
| --- | --- | --- | --- |
| cold | unbaked | disabled (`--no-compile-cache`) | 9.24 |
| warm | unbaked | warm (fresh `--compile-cache=` dir, primed once) | 0.13 |
| baked | baked (`--bake-wasm`) | disabled (`--no-compile-cache`) | 0.13 |

Baked and warm-disk-cache land at the same 0.13 s, which is the property
this feature exists to prove: a container that carries its own compiled
Wasm needs no disk cache at all to avoid the 9 s compile. Confirmed from
`HERMES_NODE_DEBUG_NATIVE=COMPILE_CACHE` tracing rather than assumed from
the timing alone -- the baked run's only Wasm-related line is
`wasm container hit`, with no `wasm store` and no `w*` file appearing under
the (disabled) disk cache directory.

The cold figure here is roughly 2x the 5.03 s the disk-cache design doc
measured on `examples/hermes-parser-ast-wasm` on a different machine; the
two are not directly comparable (different hardware, and this run's
`--no-compile-cache` also skips the JavaScript compile cache, where the
original cold measurement may not have). What both agree on is the shape:
compiling the one Wasm module is nearly the entire cold cost, and removing
that compile is nearly the entire saving, whichever tier removes it.

Also measured through a produced executable (`--build-exe`, no new flag
needed) with `HERMES_NODE_DISABLE_COMPILE_CACHE=1`: 0.12 s, with the same
single `wasm container hit` trace line and correct AST output (49 node
`"type"` fields matched, matching the unbundled run) -- confirming the
container tier reaches `--build-exe` exactly as the design says, with
nothing written to the disabled disk cache.

### Container size delta

| File | Bytes |
| --- | --- |
| unbaked container | 1,858,520 |
| baked container | 3,912,872 |
| delta | 2,054,352 |

The delta is the 2,054,308-byte bytecode payload plus 44 bytes: one
`BundleWasmRecord` (32-byte digest + two `uint32_t`s = 40 bytes) plus
alignment padding, confirming the Wasm table adds exactly one entry's worth
of container.

## What the design did not anticipate

- **The struct-size fix above.** See "Follow-on work" -- a length-agnostic
  parameter type over a real digest at Task 2's commit, only made unsafe by
  a later task, and caught by review before that task landed.
- **`--build-exe` flags have to precede the container argument, not follow
  it**, which this session re-learned by hand while measuring rather than
  from the design doc: `hermes-node --build-exe=<out> <bundle.hbb> --kit=...`
  treats `--kit=...` as an argument to the program the executable-to-be
  represents and refuses it by name, per the existing "an argument
  beginning with `-` after the container is refused" rule from the
  Single-File Executables section. `--kit=`, `--verbose` and `--cc=` all
  have to come before the `.hbb` positional. Not a defect -- documented
  behavior -- but a rough edge worth naming here since it cost a few failed
  invocations during measurement.
- **The lit-file naming hazard** (`DUPRUN` matching lit's `RUN:` scanner)
  recorded above is a small, reusable lesson: any check-prefix chosen for a
  new lit case should be checked against the literal substring `RUN` before
  it is used, not just against uniqueness within the file.
