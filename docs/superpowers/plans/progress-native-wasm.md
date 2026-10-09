# Implementation Progress

Tracks `docs/superpowers/plans/2026-10-08-native-wasm.md` (implementation
plan) and its companion design doc
`docs/superpowers/specs/2026-10-08-native-wasm-design.md`.

## Measurements

### cc on hermes-parser's module (Task 1; Release, x86_64 Intel Xeon E-2288G @ 3.70GHz, 16 cores, 2026-10-08)

| what | value |
| --- | --- |
| module (.wasm) | 768,303 bytes |
| generated C | 32,697,631 bytes |
| shermes -emit-c -O | 4.66 s, 222,568 KiB (217 MiB) |
| cc "-O0" as `build-native -O0` ran it when measured -- really the kit manifest's `-O3` compile (see below) | 2:56.35 (176 s), 1,448,580 KiB (1.38 GiB), object 25,068,984 bytes |
| cc -O3 | 2:56.19 (176 s), 1,448,044 KiB (1.38 GiB), object 25,068,984 bytes |
| cc true -O0 (manifest `-O3` removed) | 0:18.07 (18 s), 1,170,200 KiB (1.12 GiB), object 25,606,184 bytes |

The "-O0" and "-O3" rows are the same compile: the two objects are
byte-for-byte the same size. The Release kit's manifest records
`driverflag: -O3`, and when this was measured `buildCompileCommand()` put the
per-build optimization flag *before* the forwarded driver-flag list, so the
manifest's `-O3` came later on the command line and won. The last row is what
a real `-O0` costs. `buildCompileCommand()` now emits the per-build level
after both forwarded lists, so it is the last `-O` on the line whatever the
kit records, and `build-native -O0` with a Release kit compiles as the last
row does; `BuildNativeTest.CompileOptLevelWinsOverTheKitsOwnOptFlags` pins
it. (dz `01a11df9-3c0f`, closed.) The first row was not re-measured after
that change.

`cc` compiles the 32.7 MB of C for hermes-parser's module in about three
minutes and 1.4 GiB at the kit's `-O3`, well under the gate (8 GiB / 15
minutes) and under the flag-but-continue thresholds (4 GiB / 5 minutes). The
module is practical to compile natively. Three minutes is a per-build cost
(there is no object cache) that lands on a single `cc` process, so with
`--jobs` it overlaps other modules but the build's wall time is bounded below
by it. Peak memory is modest next to the 3.66 GB the plan's earlier
measurement saw on a 1,500-module JavaScript build. `-DNDEBUG`, `-ccflag`
lines and `-I{kit}/include` came from the kit manifest as it stands; no extra
include path was needed. The Release tree was rebuilt first (`shermes`,
`hermes-node`, `hermes-node-kit`, 2m19s) and the rebuilt `shermes` accepts
`.wasm` input (`SHWasmUnitReg s_wasm_reg` count in the empty-module probe: 1).
The module bytes were captured from `examples/hermes-parser-ast-wasm`
(`ast.js sample.js`) under Node and are exactly 768,303 bytes.

### Task 13: artifact and run time (Release, x86_64 Intel Xeon E-2288G @ 3.70GHz, 16 cores, 2026-10-09)

`examples/hermes-parser-ast-wasm`, `cmake-build-release` rebuilt at `c25ef65`
plus the dirty submodule (`hermes-node 0.0.2-36-gc25ef65-dirty`), in a scratch
directory. One fresh recording, both artifacts built from it, one after the
other on an otherwise idle machine:

```bash
hermes-node --no-compile-cache --record-wasm=ast.wasmrec ast.js sample.js
/usr/bin/time -f '%e s %M KiB' hermes-node build-native ast.js -o ast-native \
  --bake-wasm=ast.wasmrec --kit=cmake-build-release/kit
/usr/bin/time -f '%e s %M KiB' hermes-node build-native ast.js -o ast-bc \
  --bake-wasm=ast.wasmrec --bytecode-wasm --kit=cmake-build-release/kit
```

The recording is 2,819,128 bytes, format v2: one bytecode entry of 2,050,672
bytes and one source entry of 768,303 bytes. Both builds report 33 JavaScript
modules compiled natively; the default one also prints
`wasm: native 5a05980febf879f2 (768303 bytes)`.

| | native Wasm (default) | `--bytecode-wasm` | difference |
| --- | --- | --- | --- |
| artifact size | 45,678,736 bytes | 29,723,064 bytes | +15,955,672 bytes (1.54x) |
| build wall time | 177.87 s | 6.24 s | +171.63 s |
| build peak RSS | 1,448,124 KiB | 185,176 KiB | +1,262,948 KiB |
| cold run over `sample.js`, `/usr/bin/time -f %e`, best of 5 | 0.05 s | 0.05 s | below the timer's resolution |
| the same, `time.perf_counter()` around the process, best of 5 | 55.4 ms | 57.4 ms | -2.0 ms |
| cold run over a 316,194-byte input, best of 5 | 719.0 ms | 944.6 ms | -225.6 ms (-23.9%) |
| run peak RSS over `sample.js` | about 36,000 KiB | about 30,000 KiB | about +6,000 KiB |

Every run had `HERMES_NODE_DISABLE_COMPILE_CACHE=1`. Both artifacts print
output byte-identical to the unbundled run; under
`HERMES_NODE_DEBUG_NATIVE=COMPILE_CACHE` the default one traces exactly
`wasm native hit 5a05980f...` and the opt-out exactly
`wasm container hit 14f4b4a0...` -- one module, two digests, because the
native table keys on the bytes alone and the container on bytes plus
configuration. The two `/usr/bin/time -f %e` columns are five runs of 0.05 s
each; the `perf_counter` row is the same comparison from a second batch of
five (native 55.4-56.2 ms, bytecode 57.4-58.5 ms). The larger input is the
first 20 files of `libjs-node/`, each wrapped in a function expression; it
prints 31,705,430 bytes of JSON, identical from both artifacts, so a good part
of either figure is `JSON.stringify` rather than the parse.

What it shows. Build time is the one `cc` compile Task 1 measured (176 s at
`-O3`, `build-native`'s default) and nothing else: the
`--bytecode-wasm` build of the same 33 modules takes 6.24 s. The artifact grows
by 16 MB, less than the 25 MB object: the native build carries no
2,050,672-byte bytecode entry, and an object file also holds relocation and
symbol data a linked binary does not keep. How the rest splits between those
was not measured. At
run time the native unit is faster, by 2 ms on the tiny sample -- where
process startup dominates both -- and by about a quarter on an input big
enough for the parse to matter. The speed comparison was to be recorded, not
required; this is what it shows.

Larger inputs could not be compared: at 120 of `libjs-node/`'s files
(1,639,255 bytes) the parser fails with `memory.copy: out of bounds memory
access`, and at all 349 with `unreachable executed`, identically from the
plain unbundled `hermes-node ast.js` run, the `--bytecode-wasm` artifact and
the native one, while Node parses the same file. A pre-existing defect in
running this module under Hermes, not something this plan changes.

## Status

- [x] Task 1: measured (see above)
- [x] Task 2: H2, codegen configuration on Wasm unit registrations
- [x] Task 3: H1, lookup_native slot
- [x] Task 4: H3, native tier in createModuleFromBytes
- [x] Task 5: H4, accepted notification
- [x] Task 6: Hermes NAPI harness
- [x] Task 7: record format v2
- [x] Task 8: commit-boundary recording
- [x] Task 9: run-time native tier
- [x] Task 10: build-native toolchain pieces
- [x] Task 11: build-native compiles recorded modules natively
- [x] Task 12: end-to-end coverage
- [x] Task 13: example, measurements, docs

## State at the end

- The Hermes submodule is on the local branch `native-wasm-lookup`, six
  commits over `80ea8dc99`: `93bd8b6c0` (Task 2, registration carries the
  codegen configuration), `60958e56d` (Task 3, `lookup_native` slot),
  `cc315beeb` (Task 4, native tier in `createModuleFromBytes`), `549b80650`
  (Task 5, `accepted`), and `c19d53e72` plus its review fix `c1c1a98c2`
  (Task 6, the NAPI harness test).
- The gitlink is dirty (`M hermes`) and deliberately unstaged. Those commits
  change public ABI (`hermes_wasm_cache_callbacks`, `SHWasmUnitReg`, the
  unit-ABI model symbol) and must land upstream on `hermes-node` before a
  gitlink commit can pin them.
- hermes-node commits: `5055524` (Task 7), `169159b` (Task 8), `b1df1d1`
  (Task 9), `46f277c` (Task 10), `afa5e02` (Task 11), `c25ef65` (Task 12), the
  dz issues `9ec7dec`, and the Task 13 documentation commit.
- Filed during this work: dz `01a11df9-3c0f` (a Release kit's `-O3`
  overrode `build-native -O<n>`; fixed and closed), `01a11df9-3c69` (`StaticHUnitTest` under
  native InternalJavaScript), `01a11df9-3cc6` (`check-hermes` in this build
  tree).
