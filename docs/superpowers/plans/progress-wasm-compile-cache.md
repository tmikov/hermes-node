# Progress: Wasm compile cache

Tracks `docs/superpowers/plans/2026-09-07-wasm-compile-cache.md`
(spec: `docs/superpowers/specs/2026-09-07-wasm-compile-cache-design.md`).

## Status

Complete, plus a round of follow-on work the plan did not contain. The
history has since been squashed twice and the Hermes side rebased, so this
records what the work *is* rather than the commits it passed through: every
SHA the first version of this file named is now unreachable, which is the
reason none appear below.

**hermes-node-compat**, on `work-wasm-cache` above `origin/work`:

| Commit | What |
| --- | --- |
| `Point the hermes submodule at the Wasm cache branch` | gitlink; a stopgap, see below |
| `Cache compiled WebAssembly modules on disk` | the feature, its fixes, its documentation |
| `Add a cache subcommand: info, prune and clean` | `hermes-node cache ...` |
| `dz: close the non-discriminating NAPI test issue` | a closure whose fix is in the submodule |

**hermes**, on `wasm-compile-cache` above `96f7a4103`:

| Commit | What |
| --- | --- |
| `Declare hermesBackend's dependency on hermesInst` | pre-existing build fix, kept separate |
| `Let a Wasm compile hand back serialized bytecode` | the WasmFrontend half |
| `Let an embedder cache compiled Wasm modules` | the hooks, the ABI, the call site |

**Outstanding:** the Hermes commits are to be grafted onto `wasm-new` and
merged into `hermes-node`, after which the gitlink should move to the
resulting public commit. Until then the gitlink pins a local, unpushed
commit, so a fresh clone cannot resolve it and nothing in CI or a release
can use it. The commit that moves it says so, but names a pre-rebase SHA in
its message.

## Follow-on work, after the plan

None of this was in the nine tasks. It came from an external review and from
reading the finished feature again.

- **The codegen configuration became an opaque byte string**, having been a
  `uint32_t` carrying one bit. It is a parameter of the `lookup` callback,
  not a field of the struct, so `struct_size` extensibility does not reach
  it and the type had to be right before anything else consumed the ABI.
  Hermes composes it -- `hermes-wasm;bc=...;cg=...;t262=...` -- so an
  embedder need not know what belongs in it, and its length is hashed before
  it, because a variable-length prefix makes plain concatenation ambiguous.
  `WASM_CODEGEN_VERSION` came with it, which is what makes a Hermes-only
  codegen change invalidate.
- **Config parse failures are reported.** They were silent, and the design
  had promised tracing; unconditional warnings were chosen instead, because
  a bad line in a hand-edited file is wrong on every run until someone
  changes it, and a diagnostic behind a debug flag would never reach whoever
  made the typo.
- **The budget bounds the whole cache.** The sweep walked one generation
  while three retained ones held their own, so real disk use could reach
  four times the configured number.
- **Abandoned temp files are reaped.** An interrupted write left a
  full-size entry that nothing collected and the budget could not see.
- **`hermes-node cache info|prune|clean`** exists, which is what makes the
  budget reachable on demand without putting a directory walk on every
  startup.
- **The best-effort contract is written down**, along with the one thing it
  excludes -- allocation failure -- and pinned by a test that makes every
  cache write fail at once.

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
- **A test written for a fix could not catch it.** The `struct_size` test
  handed over a fully allocated struct that merely claimed to be small, so
  reading a callback field out of it was harmless either way -- it passed
  with and without the fix. Catching the real defect needs a genuinely
  truncated allocation, where an early read is an out-of-bounds ASAN fails
  on. Verified in both directions before and after.
- **A partial write of the default config file is not the defaults.** A
  prefix of that text ending `max_wasm_bytes: 2` is well-formed and yields a
  two-byte budget, and a short write followed by failure would have left
  exactly that permanently, since `O_CREAT|O_EXCL` guarantees nothing
  replaces it. The file is now published with `link()` after a complete
  write, never `rename()`, which would let a race's loser clobber the
  winner.
- **A Hermes-only change does invalidate the cache -- once.** The generation
  tag carries `git describe --dirty`, and a changed submodule dirties the
  outer tree, so the first edit adds `-dirty` and every edit after leaves the
  string identical. The exposure is iterating with the gitlink uncommitted,
  which is exactly the loop a codegen fix happens in. Recorded in CLAUDE.md
  with the measurement.
