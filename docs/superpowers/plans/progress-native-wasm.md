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
| cc -O0 (as `build-native` would run it; see below) | 2:56.35 (176 s), 1,448,580 KiB (1.38 GiB), object 25,068,984 bytes |
| cc -O3 | 2:56.19 (176 s), 1,448,044 KiB (1.38 GiB), object 25,068,984 bytes |
| cc true -O0 (manifest `-O3` removed) | 0:18.07 (18 s), 1,170,200 KiB (1.12 GiB), object 25,606,184 bytes |

The "-O0" and "-O3" rows are the same compile. The Release kit's manifest
records `driverflag: -O3`, and `buildCompileCommand()` emits the per-build
optimization flag *before* forwarding the whole driver-flag list, so the
manifest's `-O3` comes later on the command line and wins: the two objects
are byte-for-byte the same size. The last row is what a real `-O0` costs once
that `-O3` is filtered out. This is an observation about the existing
`build-native` compile step (its `-O0`/`-O1`/`-O2`/`-Os` flags are overridden
by a Release kit's `-O3`), not something this task changes; it matters to the
design only because a user cannot currently make a Wasm unit compile faster
by asking for `-O0`.

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

## Status

- [x] Task 1: measured (see above)
- [ ] Task 2: H2, codegen configuration on Wasm unit registrations
- [ ] Task 3: H1, lookup_native slot
- [ ] Task 4: H3, native tier in createModuleFromBytes
- [ ] Task 5: H4, accepted notification
- [ ] Task 6: Hermes NAPI harness
- [ ] Task 7: record format v2
- [ ] Task 8: commit-boundary recording
- [ ] Task 9: run-time native tier
- [ ] Task 10: build-native toolchain pieces
- [ ] Task 11: build-native compiles recorded modules natively
- [ ] Task 12: end-to-end coverage
- [ ] Task 13: example, measurements, docs
