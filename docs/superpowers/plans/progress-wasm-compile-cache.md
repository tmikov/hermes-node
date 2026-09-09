# Progress: Wasm compile cache

Tracks `docs/superpowers/plans/2026-09-07-wasm-compile-cache.md`
(spec: `docs/superpowers/specs/2026-09-07-wasm-compile-cache-design.md`).

## Status

All nine tasks complete.

| Task | What | Where |
| --- | --- | --- |
| 1 | Serialize a Wasm-compiled module | hermes, `b89d5dc2e` (+ `01bceee39`) |
| 2 | Embedder cache hooks and the NAPI entry point | hermes, `2f81dc915` (+ `683eb9bb6`) |
| 3 | Verify the Hermes half end to end | no commits |
| 4 | Cache directory configuration | `3e91d54`, `884f6d1` |
| 5 | Content-keyed Wasm entries | `a44d24f` |
| 6 | Bounding Wasm entries | `90d1fb5`, `7a46d3e` |
| 7 | Install the hooks | `d1522e5`, `91a1d23` |
| 8 | End-to-end tests | `875012c`, `9d6011a`, `b1cd40f` |
| 9 | Documentation and measurement | this file, plus `CLAUDE.md` |

The four `hermes` commits are on branch `wasm-compile-cache`; the gitlink in
hermes-node-compat is deliberately unstaged, awaiting the user's direction.

## Measurements

`cmake-build-release` (`CMAKE_BUILD_TYPE=Release`), Linux x86_64. Each figure is the
median of two or three runs that agreed to within 0.05 s.

### examples/hermes-parser-ast-wasm

One Wasm module (`HermesParserWASM.js`, a 2,054,332-byte bytecode entry).

| | seconds |
| --- | --- |
| cold (empty cache) | 5.03 |
| warm | **0.07** |
| JavaScript cached, Wasm entry deleted | 5.21 |

Cache: 3,908,234 bytes over 34 entries plus `config` -- 33 JavaScript
entries and one Wasm entry that is 53% of the bytes.

### examples/flow-bundler, FLOW_BUNDLER_PARSER=wasm

The same parser module against a far larger JavaScript graph.

| | seconds |
| --- | --- |
| cold (empty cache) | 12.65 |
| warm | **2.33** |
| JavaScript cached, Wasm entry deleted | 7.25 |

Cache: 18 MB.

**The third row is the measurement that matters** and it was not in the
plan. Cold-versus-warm conflates this feature with the JavaScript compile
cache that already existed, so both were also measured with every JavaScript
entry left in place and only the single `w` entry removed. On
hermes-parser-ast-wasm that run costs 5.21 s against a 5.03 s cold run:
compiling that one module is essentially the entire cold cost, and caching
it is essentially the entire speedup -- 74x. On flow-bundler it isolates
~4.9 s of the 10.3 s saving as the Wasm module's alone.

## What the design did not anticipate

- **`hermesBackend` never declared its dependency on `hermesInst`.** Latent
  in Hermes, exposed the moment `WasmCompile.cpp` called
  `serializeBytecodeModule`, which pulls `LiteralBufferBuilder.cpp.o` out of
  the archive; it calls `hermes::inst::getInstSize`. Confirmed pre-existing
  by stashing every other edit and rebuilding. Fixed in its own commit
  ahead of the feature commit.
- **The `config` file made two existing tests unable to fail.** It sits at
  the cache root, above the versioned tree, so generation pruning cannot
  delete it -- but that also means it is written on every run whether or not
  a single entry is. `test/compile-cache-cjs.js` asserted a cold run had
  populated the cache with `find %t.cache -type f | wc -l` against
  `POPULATED-NOT: {{^0$}}`, which the config file alone satisfies forever;
  reproduced by deleting every real entry after a run and watching the test
  still pass. `test/compile-cache-corrupt.js` was separately zero-filling 64
  bytes inside the config file's text. Both now spell `-not -name config`.
  The general lesson outlived the fix: any new file placed in the cache tree
  needs this audit.
- **The hit buffer's lifetime is not the one the adapter first claimed.**
  `hermes_run_bytecode` transfers a mapping to the bytecode provider and
  frees it when that dies; the Wasm hit path in `WebAssembly.cpp` instead
  copies the bytes with `getMemBufferCopy` and calls the finalizer
  immediately afterwards. The code was right and the comment was wrong,
  which is the more dangerous of the two orders -- corrected in `91a1d23`.
- **`--inspect-brk` cannot be used in a test.** It pauses before user code
  and never resumes without a debugger client, so the obvious way to assert
  that inspection disables the cache hangs forever (measured: exit 124 under
  a 5 s timeout). `--inspect` reaches the identical
  `config.inspect || config.inspectBrk` guard without pausing.
- **A produced executable is not launched through `%hermes-node-cc`**, so it
  inherits the suite-wide `HERMES_NODE_DISABLE_COMPILE_CACHE=1` and its test
  silently asserted nothing until the RUN lines gained `env -u`.
- **`strtoull` accepts a leading sign.** `max_wasm_bytes: -1` parsed
  cleanly, set `errno` to 0, consumed the whole string, and yielded
  2^64-1 -- a nonsense eviction budget arriving through the path that is
  supposed to fall back to the default. Signs are now rejected before the
  parse.
- **Five of the nine task briefs contained a defect** -- a header that does
  not exist (`BCProviderFromBuffer.h`), an API spelled with the wrong prefix
  (`picohash_*` for `ph_*`), an include that resolves nowhere, an expected
  output that was simply wrong (`WebAssembly.validate` on a header-only
  module returns `true`, as Node does), and a `struct_size` check written
  after the dereferences it guards. Each was caught by the implementer or
  the reviewer rather than by the plan; the plan text was corrected where it
  would mislead a later reader.
