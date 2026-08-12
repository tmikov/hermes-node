# Design: On-Disk Bytecode Compile Cache

**Status:** Design approved 2026-08-12. Implementation plan to follow.

## Goal

Cut the latency of running the same program repeatedly under `hermes-node` by
caching compiled Hermes bytecode on disk, so a second run skips parsing and
compiling the JavaScript it already compiled once.

## Context

Built-in JavaScript (`libjs/`, `libjs-node/`, shims) is already compiled to
bytecode at build time and embedded in the binary. Everything else -- the
user's script, and every file under `node_modules/` -- is compiled from source
on every run.

Measured on the `examples/flow-bundler` workload with a Release build, by
instrumenting `Module.prototype._compile` and timing a second compile of each
module through the same native entry point:

| | files | source bytes |
| --- | ---: | ---: |
| `node_modules` (Babel, prettier, ...) | 2829 | 18,286,893 |
| outside `node_modules` | 12 | 301,728 |
| -- of which Babel-transformed | 9 | 297,503 |
| **total passed to `_compile`** | **2841** | **18,588,621** |

Compile-only cost was **~3.15 s of a ~6.3 s run** -- roughly half the wall
clock. 98% of the compiled bytes are unmodified npm packages.

### The path a module takes today

1. `@babel/register` installs a `pirates` hook over `Module._extensions['.js']`.
2. The CJS loader reads the file, the hook transforms it (for files matched by
   `only:`), and calls `module._compile(content, filename)`.
3. `Module.prototype._compile` (`libjs-node/internal/modules/cjs/loader.js:1711`)
   calls `wrapSafe` (`:1650`), which -- since `patched` is false -- calls
   `compileFunctionForCJSLoader` (`:1692`).
4. That is `internalBinding('contextify').compileFunctionForCJSLoader`, i.e.
   `compileFunctionForCJSLoaderCb` in `lib/bindings/node_contextify.cpp:594`.
5. It concatenates the CJS wrapper prefix, the source, `"\n})"` and a
   `//# sourceURL=` directive, then calls `napi_run_script`
   (`node_contextify.cpp:650`), which parses, compiles and evaluates the
   parenthesised function expression. The bytecode is reachable only through
   the returned closure and is discarded with it.

So a single native function is the choke point for every CJS module load, and
nothing is keyed, stored or reused.

### Why the artifact is worth caching

V8's code cache is *supplementary*: V8 still needs the source and may reject
the blob. Hermes bytecode is the *complete* artifact, so a cache hit can skip
reading and parsing source entirely. The hit path is cheaper than Node's, and
the win is correspondingly larger.

### The Babel case, and what it forces

For files `@babel/register` transforms, three artifacts exist:

| | what | where |
| --- | --- | --- |
| 1 | Flow + ESM source, 5,358 bytes (`BuildBundle.js`) | on disk |
| 2 | Babel output, CJS, 21,222 bytes | **memory only** |
| 3 | Hermes bytecode | what we cache |

The cache sits on the second arrow: its input exists only in memory. Verified
by instrumenting `_compile`: the `filename` it receives is the real, existing
on-disk path in every case, while the `content` differs from the file on disk
in every case (90,556 bytes on disk expand to 297,503 handed to Hermes, 3.3x).

This settles the keying scheme. The **path** is a stable identifier and makes a
good location key; the **content** is what must be validated, because hashing
the post-transform text transitively covers the source file, `babel.config.js`,
the Babel version, the plugin set and `hermes-parser`'s output. Under
mtime-based keying, editing `babel.config.js` while leaving `BuildBundle.js`
untouched would serve bytecode compiled from the previous transform, with no
way to detect it. Hashing content makes that class of bug impossible.

The Babel transform itself is not cached by us, and does not need to be:
`@babel/register` has its own cache at
`node_modules/.cache/@babel/register/.babel.<version>.<env>.json`, keyed on
filename + mtime + options hash. The two caches stack and are independent.

## Scope

In scope:

- A new `lib/compile-cache/` library implementing the cache.
- Cache consultation in the CJS loader entry point
  (`compileFunctionForCJSLoaderCb`) and in `compileAndRunCallback`
  (`lib/module-loader/module_loader.cpp:102`), which serves both the `.ts`
  extension handler and the bootstrap loader's disk fallback.
- Enabling the cache during bootstrap, on by default.
- Unit and lit test coverage.

Out of scope:

- `vm.Script` (`contextifyScriptNew`), `vm.compileFunction`
  (`compileFunctionCb`) and `-e` eval. `contextifyScriptNew` keeps its own
  private `hermes_compile_to_bytecode` usage; reconciling it with Node's
  `cachedData` API semantics is separate work.
- Any JavaScript-visible cache API. The existing stubs keep reporting that
  there is no caching; see "The JavaScript API" below.
- Any change to the `hermes/` submodule.
- Windows. CI is Linux and macOS only, and the design uses POSIX `mmap` and
  `rename`.
- ESM. The CJS loader is the only module path in use.

## Design

### Architecture

A new library `lib/compile-cache/` with public header
`include/hermes/node-compat/compile-cache/compile_cache.h`, following the
project's `lib/<name>/` + own `CMakeLists.txt` convention. It links `uv_a` for
filesystem access (as Node's `compile_cache.cc` does) and `zlib_a` for CRC32.

The cache is entirely native and invisible to JavaScript, mirroring Node, where
`CompileCacheHandler` is C++ and is consulted inside `compileFunctionForCJSLoader`.
Beyond parity, this is forced: the cache must work during bootstrap, before
`fs` is loadable, so a JS-side implementation is not possible.

One class, `CompileCache`:

```
CompileCacheEnableStatus enable(root);   // status is for tracing only
CompileCacheEntry *getOrInsert(source, filename, kind);
void save(entry, bytecode, size);
```

`CompileCacheEntry` carries the key, source CRC and size, the cache file path,
and on a hit the mapped bytecode region.

`kind` identifies the calling entry point, because the two entry points hash
differently shaped strings for the same file and must not share a key:

| `kind` | caller | the string passed as `source` |
| --- | --- | --- |
| `kCommonJS = 0` | `compileFunctionForCJSLoaderCb` | the **unwrapped** module source; native code applies the CJS wrapper |
| `kLoaderWrapped = 1` | `compileAndRunCallback`, `enableTS` false | the **already-wrapped** source, wrapped in JS by the caller |
| `kLoaderWrappedTS = 2` | `compileAndRunCallback`, `enableTS` true | as above; separate kind because `enableTS` changes compile flags |

`compileAndRunCallback` serves two call sites in `libjs/loader.js`: the
bootstrap loader's disk fallback (`:64`) and the `.ts` extension handler
(`:327`). Both pass an already-wrapped source, so both are covered by the two
loader kinds.

Consumers:

| Consumer | Role |
| --- | --- |
| `lib/bindings/node_contextify.cpp:594` | CJS hook: consult, hit -> `hermes_run_bytecode`, miss -> `hermes_compile_to_bytecode` + `save` + run |
| `lib/module-loader/module_loader.cpp:102` | Same, with the loader kinds |
| `tools/hermes-node/hermes-node.cpp` | Enables the cache at bootstrap; skips enabling under `--inspect` / `--inspect-brk` |

### Debug info

`napi_run_script` hard-codes `compileFlags.debug = true` under
`HERMES_ENABLE_DEBUGGER` (on in all our builds), giving `DebugInfoSetting::ALL`.
`hermes_compile_to_bytecode` has no `debug` field and always produces
`DebugInfoSetting::THROWING`
(`hermes/lib/BCGen/HBC/BCProviderFromSrc.cpp:156`). THROWING still carries
source locations for exception stack traces; ALL is what the debugger needs to
set breakpoints anywhere.

Cached bytecode is therefore THROWING-level, and **the cache is not enabled at
all when `config.inspect` or `config.inspectBrk` is set**. Debugging keeps
today's `napi_run_script` path unchanged. Putting the bypass at the enable site
rather than the compile site means neither consumer needs a per-call branch and
debugger behaviour is unchanged by construction rather than by intent.

A side effect, considered acceptable: normal runs stop emitting full debug
info, which makes them faster and the cache smaller. Exception stack traces are
unaffected.

The alternative -- adding a `debug` field to `hermes_compile_flags`, which the
struct's `struct_size` field is designed to permit -- was rejected for v1
because it requires a `hermes/` submodule change.

### Activation and configuration

On by default. No `NODE_*` environment variables are consulted; supporting two
naming schemes for one feature only creates confusion.

| Control | Effect |
| --- | --- |
| `--compile-cache=<dir>` / `--no-compile-cache` | CLI, highest precedence |
| `HERMES_NODE_COMPILE_CACHE=<dir>` | Override the cache root |
| `HERMES_NODE_DISABLE_COMPILE_CACHE=1` | Off |
| `HERMES_NODE_DEBUG_NATIVE=COMPILE_CACHE` | Per-module hit/miss/reason tracing |
| `--inspect` / `--inspect-brk` | Off (see above) |
| default | On, at the XDG root |

Default root: `$XDG_CACHE_HOME/hermes-node/compile-cache`, falling back to
`~/.cache/hermes-node/compile-cache`. Chosen over `tmpdir` because the whole
point is surviving between runs, and `tmpdir` is cleared on reboot and by
`tmpfiles.d`; and over a per-project `node_modules/.cache` because that
requires a project root, which standalone scripts do not have.

If the root cannot be created (no `HOME`, read-only home), the cache is
disabled for the process and the run proceeds normally.

### The JavaScript API

**The cache is not observable from JavaScript, deliberately.** Every
cache-related JS API continues to report that there is no caching:

| API | Returns |
| --- | --- |
| `module.enableCompileCache()` | `{status: 0}` (`FAILED`) |
| `module.getCompileCacheDir()` | `undefined` |
| `module.flushCompileCache()` | no-op |

This is the current behaviour, so **no JavaScript changes at all**.
`libjs/shims/internal/modules/helpers.js:142-148` already shims the first two
with hard-coded returns, which means Node's implementation at
`libjs-node/internal/modules/helpers.js:418` is not loaded and native
`_enableCompileCache` is never called. The stubs in
`lib/bindings/node_modules.cpp:87-124` are likewise left as they are.

The reasoning is that a JS API which half-works is worse than one that reports
nothing. Exposing the cache properly means the whole surface -- enabling,
disabling, re-pointing the root mid-run, flushing, and Node's `portable`
option -- along with the implicit-versus-explicit enable distinction that an
on-by-default cache requires. If that is wanted later it should be done
comprehensively, as its own piece of work, not by flipping a single return
value.

Consequence for tests: they detect the cache through the filesystem rather than
through the JS API, and one test asserts that the JS API keeps reporting no
cache even while the cache is active.

### On-disk layout

```
<root>/v1/<generation>/<ab>/<key>
```

- `v1/` is a format-structure escape hatch: a fundamentally different layout
  later becomes `v2/` and can coexist.
- `<generation>` is `<version>-<arch>-bc<BYTECODE_VERSION>-<configCrc>`, e.g.
  `0.3.0-x86_64-bc96-3f9c21ab`. Readable, so the active generation is
  answerable by looking. `configCrc` covers the exact CJS wrapper text applied
  by `compileFunctionForCJSLoaderCb` and the `hermes_compile_flags` bit
  patterns used by every kind. It does not need to cover the JS-side wrapper
  used by the loader kinds: that text is part of the hashed source for those
  entries.
- `<ab>` is the first byte of the key in hex, giving 256-way fanout.
- `<key>` is `crc32(kind, filename)` in hex.

Node uses the same generation scheme
(`<dir>/$NODE_VERSION-$ARCH-$CACHE_DATA_VERSION_TAG-$UID/<key>`,
`compile_cache.cc:538-548`); the fanout and the pruning below are our
additions. Fanout because our cache is on by default and user-global, so it
accumulates across every project on the machine, whereas Node's is opt-in and
usually per-project.

### Two kinds of invalidation

The layout exists to separate them.

**Per-file.** One module's source changed. The entry's `sourceCrc32` and
`sourceSize` catch it; the lookup becomes a miss and the entry is overwritten.
Because the key depends only on the path, an edit rewrites one entry rather
than leaving a new one behind.

**Global.** A new `BYTECODE_VERSION`, an edited wrapper, a changed compile
flag, a new `hermes-node` build. Nothing about any source file changed, but no
entry is usable. Handled by the generation directory, not per entry.

If the config tag lived only in entry headers, global invalidation would still
work -- 2841 opens, 2841 mismatches, 2841 recompiles -- but entries that are
never looked up again (an abandoned project, a deleted `node_modules`) would
hold unusable bytecode forever, because reclamation only happens when that
exact path is requested again.

### Generation pruning

On `enable()`, compute the generation name -- a pure function of the binary --
and use `<root>/v1/<generation>/`. If it exists, use it; this is the common
case. If not, `mkdir` it (tolerating `EEXIST`, since processes race) and delete
sibling generations beyond the **3 most recently modified**.

Pruning runs only on the rare path where a new generation was created, never on
a normal startup. Keeping 3 rather than 1 matters for this project's own
developers: version strings come from git tags via `cmake/version.cmake`, so
two checkouts at different commits produce different generations, and
"delete every sibling" would make alternating between them pay full compile
cost every time.

Deleting a generation another process is using is safe: on POSIX, unlinking a
mapped file removes only the directory entry, and the inode and its pages
survive until the last mapping is dropped.

### Entry format

A 24-byte header, then raw Hermes bytecode.

| off | size | field |
| ---: | ---: | --- |
| 0 | 4 | magic `'HNCC'` |
| 4 | 4 | header format version |
| 8 | 4 | `sourceCrc32` |
| 12 | 4 | `sourceSize` (UTF-8 bytes) |
| 16 | 4 | `bytecodeSize` |
| 20 | 4 | reserved |

24 is a multiple of 8, so a page-aligned `mmap` base puts the payload at an
address satisfying `BYTECODE_ALIGNMENT` (`alignof(uint32_t)` = 4).

CRC32 comes from the already-vendored `external/zlib`, which is already linked
into `hermesNodeBindings` as `zlib_a`, and is exactly what Node uses
(`compile_cache.cc:42-67`). llvh ships `xxhash.h` but no `xxhash.cpp`, so
`llvh::xxHash64` is declared and never defined and would not link; and the
performance case for a faster hash does not survive the numbers -- CRC32 over
18.6 MB is ~15-25 ms against a warm run of roughly 3.2 s.

A key collision costs thrash (two paths evicting each other), not correctness.
A content collision requires the same path to hold two different sources of
identical length whose CRC32 also matches -- the risk Node already ships.

**No payload hash, deliberately.** Node stores and verifies a hash over the
blob because V8 will not validate a code cache itself. Hermes validates for us:
`sanityCheck` (`hermes/lib/BCGen/HBC/BCProvider.cpp:56`, always run via
`populateFromBuffer`) checks alignment, magic, `BYTECODE_VERSION`,
`functionCount > 0` and `buffer.size() >= header->fileLength`, returning an
error rather than crashing. With atomic writes, a torn file is unreachable.
Hashing the payload would fault in every page and destroy the benefit of
mapping the file. `BCProviderFromBuffer::bytecodeHashIsValid()`
(`BCProvider.cpp:737`, full SHA-1 against the bytecode footer) remains
available for a paranoid mode; it runs automatically only under
`HERMES_SLOW_DEBUG`.

### Hit path

1. Compute `key = crc32(kind, filename)` and `sourceCrc32` / `sourceSize` over
   the source string the caller passed -- unwrapped for `kCommonJS`, wrapped
   for the loader kinds, per the table above.
2. Look up `key` in the in-memory store. A match with an equal `sourceCrc32`
   returns immediately. This alone makes the duplicate compile observed in the
   bundler run (`@babel/parser/lib/index.js`, compiled twice in one process)
   free the second time.
3. Open `<root>/v1/<generation>/<ab>/<key>`, read 24 bytes, and check in
   cheapest-first order: magic, header version, `sourceSize`, `sourceCrc32`.
   Bytecode version, compile flags and wrapper shape are *not* rechecked -- the
   generation directory already established them for every entry inside it.
4. `mmap(NULL, 24 + bytecodeSize, PROT_READ, MAP_PRIVATE, fd, 0)`, then close
   the fd; the mapping keeps the inode alive.
5. `hermes_run_bytecode(env, base + 24, bytecodeSize, unmapCb, mapping,
   sourceURL, flags, &fn)`. It wraps the pointer in a `CallbackBuffer` without
   copying (`hermes/API/napi/hermes_napi.cpp:744`), so `unmapCb` releases the
   mapping when the RuntimeModule dies. `rmFlags.persistent` stays false,
   matching `napi_run_script`'s default.

Hermes reads bytecode straight out of the mapping, so only functions actually
executed are faulted in, and concurrent `hermes-node` processes share page
cache.

### Miss path

1. Build the wrapped source exactly as today and call
   `hermes_compile_to_bytecode`.
2. Write `<ab>/<key>.<pid>.tmp` -- header then bytecode -- and `rename()` it
   into place. `rename` is atomic on POSIX, so a concurrent reader sees the
   complete old file or the complete new one. Two processes compiling the same
   module both write valid identical content, so a lost race overwrites with
   the same bytes and needs no locking.
3. Run from the heap buffer already in hand, with a finalizer calling
   `hermes_free_bytecode`. What was just written is never read back.
4. Record the entry in the in-memory store.

### Error handling

The cache is best-effort and must never be why a program fails. Unwritable
cache directory disables it for the process; open, read or `mmap` failure,
header mismatch, and truncated or garbage files all become a miss; write
failure is ignored and the run proceeds uncached.

**The critical rule.** `hermes_run_bytecode` reports a bad buffer by raising a
JavaScript `SyntaxError` through `raiseSyntaxError` and returning
`napi_pending_exception` (`hermes_napi.cpp:751`). Propagating that would make a
corrupt cache file appear as a syntax error in valid user code. On that path we
must `napi_get_and_clear_last_exception`, delete the offending file, and fall
back to compiling from source.

This swallowing applies **only** to the cached-bytecode path. A genuine
`SyntaxError` from compiling the user's source propagates unchanged.

## Testing

**Unit (GTest, `unittests/`, via `add_node_compat_unittest()`)**, no runtime
required: key derivation and stability; header round-trip; the generation name
is a pure function of configuration; pruning retains the 3 most recent by
mtime; rejection on each individual header field; tolerance of truncated and
garbage files.

**Lit (`test/`)**: cold then warm run produce identical output; editing a
source between runs picks up the change; a deliberately corrupted cache file
still produces correct output, exercising the swallow-and-recompile path;
`--inspect-brk` creates no cache files; a real `SyntaxError` reports the same
file and line warm and cold; `--no-compile-cache` and
`HERMES_NODE_DISABLE_COMPILE_CACHE` suppress all writes; and
`module.enableCompileCache()` still reports `FAILED` with
`module.getCompileCacheDir()` still `undefined` while the cache is active, so
the JS API cannot start reporting the cache by accident.

**Isolation**: `test/lit.cfg` sets `HERMES_NODE_DISABLE_COMPILE_CACHE=1` for
the whole suite; compile-cache tests opt in with their own directory under
`%t`. Leaving the cache on across the suite would make tests share state
through `~/.cache` and turn ordering into a variable. This also guarantees
`check-hermes-node` never writes to the user's real cache.

**End to end**: `examples/flow-bundler` is the workload that produced the
3.15 s figure. Assert that the cache directory is populated and that output
still matches `expected/`, rather than asserting a wall-clock threshold, which
would be flaky in CI.

## Expected outcome

A warm run should eliminate the ~3.15 s of compilation, leaving the ~3.2 s of
other work. It will not approach zero: a warm run still reads 18.6 MB of source
in JS, converts it to UTF-8 and CRC32s it. The cache removes parsing and
compiling, not I/O. The real figure is to be measured after implementation, not
promised here.

Cache size should be roughly source-sized: measured bytecode/source ratios are
0.75-1.10 for large files, with ~400 bytes of fixed overhead per entry against
a median npm `.js` file of 586 bytes.

## Known limitations

- **Duplicate package copies.** Two copies of the same file at different paths
  (observed: `@babel/parser/lib/index.js`) get two identical entries.
  Content-addressed keys would dedupe but would leak a dead file on every edit.
- **One path, two configurations.** The same file transformed under two Babel
  configs maps to one key, so the configs evict each other on alternation.
  Correctness holds -- the content hash makes each a miss -- but it thrashes
  silently.
- **More than three builds.** Alternating between more than 3 `hermes-node`
  builds re-prunes generations each time.
- **`portable` is not implemented.** Node's relative-path cache keys are not
  supported; with a user-global XDG root they would essentially never yield a
  usable relative path. Nothing can request it in any case, since the JS API
  reports no caching.
- **No programmatic control.** A program cannot enable, disable, re-point or
  inspect the cache from JavaScript; only CLI flags and environment variables
  control it.
- **Debug builds and `--inspect` get no caching**, by design.
