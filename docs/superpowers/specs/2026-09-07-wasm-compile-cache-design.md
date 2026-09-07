# Design: caching Wasm-to-bytecode compilation

**Status:** Draft for review, 2026-09-07.

Extends the on-disk compile cache to cover WebAssembly, so that a program
compiling a `.wasm` module pays for it once rather than on every run.

Follows `docs/superpowers/specs/2026-09-02-vm-options-design.md` in touching
the Hermes interface: this needs two additions to Hermes that do not exist
today, and the design is largely about where that seam goes.

## The problem

Hermes compiles a WebAssembly module ahead of time to Hermes bytecode and runs
it as ordinary bytecode -- no interpreter, no Wasm JIT. That is the right
trade for steady-state execution and the wrong one for startup, because the
whole module is compiled before the first export can be called, and nothing
keeps the result.

Measured on Linux x86_64 with a Release `hermes-node`, parsing
`libjs-node/vm.js` with the vendored WebAssembly `hermes-parser`
(`external/hermes-parser-wasm`, a 665,817-byte module):

| | first `parse()` | steady `parse()` |
| --- | --- | --- |
| node (V8) | 58 ms | 3-4 ms |
| hermes-node | 4725 ms | 26 ms |

The 4.7 seconds is the module compile, and it is paid **on every run**. Two
consecutive runs of the same program measured 9646 ms and 10313 ms of
`new WebAssembly.Module` with the published npm build of the same parser: the
compile cache did not help, because it covers JavaScript only.

Meanwhile the ingredients of a fix already exist and are fast.
`hermesc --wasm -emit-binary` turns that module into a 2,388,624-byte `.hbc`
in about 10 seconds, and `WebAssembly.Module.fromHermesBytecode` loads that
`.hbc` in **0 ms**. So the compile is not merely cacheable in principle; the
bytecode is already a supported input, it simply has no route from a compile
to a later run.

This matters in every run mode. A `--bundle` container and a `--build-exe`
executable compile no JavaScript -- that is the point of them -- but a Wasm
module inside one is compiled at every launch just the same. Today such an
artifact *creates* `~/.cache/hermes-node/compile-cache/v1/<generation>/` and
writes nothing into it.

## The decision

1. **Hermes gains two things: the ability to serialize a Wasm-compiled module,
   and an embedder cache hook.** The hook is a `lookup`/`store`/`discard`
   callback set registered per `napi_env`; Hermes consults it at the one
   choke point where a Wasm module is turned into a bytecode provider.
2. **Entries are content-keyed by SHA-256, with the digest as the filename.**
   Wasm bytes usually have no path, so the existing path-keyed scheme cannot
   apply. No change to the on-disk entry format.
3. **Every run mode uses it**, including `--bundle` and a produced executable.
4. **Wasm entries are bounded by a size budget with LRU eviction, configured
   by a file in the cache directory.** Defaults: order by `atime`, 256 MB.

## What changes in Hermes

### Serializing a Wasm-compiled module

`compileWasmToModuleData` (`lib/WasmFrontend/WasmCompile.cpp`) already builds
its provider with `hbc::BCProviderFromSrc::createFromBytecodeModule`, so the
`BytecodeModule` is in hand and serialization is the same call
`hermes_compile_to_bytecode` already makes:

```cpp
hbc::serializeBytecodeModule(*provider->getBytecodeModule(), hash, os, opts);
```

This becomes a real entry point rather than something private to the cache
path, because it is the piece a build-time producer will need when a container
learns to carry precompiled Wasm. That is deferred (see "Deliberately not in
scope"), but the boundary is placed for it now.

### The cache hook

```c
typedef struct {
  size_t struct_size;
  void *ctx;

  /// Consult the cache for \p wasm. Always produces a store token.
  /// On a hit: sets *hbc/*hbc_size plus the finalizer Hermes calls when the
  /// bytecode provider dies, and returns true.
  /// On a miss: returns false, leaving only *store_token set.
  bool (*lookup)(void *ctx,
                 const uint8_t *wasm, size_t wasm_size,
                 uint32_t codegen_config,
                 const uint8_t **hbc, size_t *hbc_size,
                 void (**finalize_cb)(const uint8_t *, size_t, void *),
                 void **finalize_hint,
                 void **store_token);

  /// Persist freshly compiled bytecode against the identity in \p token,
  /// and release the token.
  void (*store)(void *ctx, void *store_token,
                const uint8_t *hbc, size_t hbc_size);

  /// Release \p token without persisting anything.
  void (*discard)(void *ctx, void *store_token);
} hermes_wasm_cache_callbacks;

napi_status hermes_set_wasm_cache(
    napi_env env, const hermes_wasm_cache_callbacks *callbacks);
```

Four properties of that signature are deliberate.

**The hook sees the Wasm bytes, not a hash.** Hashing is policy: choosing
SHA-256, or changing it later, is then a hermes-node decision needing no
Hermes change.

**`codegen_config` is Hermes's value, not ours.** Hermes knows what affects
its own output -- `runtime.test262` today, which changes memory bounds
checking and alignment handling. hermes-node computing that itself would work
until the day Hermes adds a second knob, at which point the cache would
silently serve bytecode compiled under different rules. Hermes computes it;
the embedder only mixes it into the key.

**The finalizer/hint pair mirrors `hermes_run_bytecode`.** The lifetime
problem is identical: a `JSWebAssemblyModule` holds its provider, so a mapped
cache file must outlive the call that produced it. `CacheMapping::finalizer`
in `lib/compile-cache/` already has exactly this shape and is reused unchanged.

**The token makes the two derivations one derivation.** `lookup` computes the
identity; `store` receives it rather than recomputing from the bytes. The cost
argument is thin -- SHA-256 over 665 KB measured at about 2 ms including
process spawn, and so below that on its own, against a 4.7-second compile it
only ever accompanies -- but
the correctness argument is not: two independent derivations from the same
inputs can drift, and the failure mode is writing an entry the next lookup
never finds, which presents as "the cache does not work" rather than as a bug.
It also mirrors `CompileCache::lookup()`, which already populates a
`CompileCacheEntry`'s identity on a miss and hands that same entry to `save()`.

**Ownership is single and total: `lookup` always produces a token, and Hermes
calls exactly one of `store` or `discard` on every path.** A satisfied hit
discards. A miss compiles and stores. A hit whose bytes are then rejected
compiles and stores.

### Where it is called

`createModuleFromBytes` (`lib/VM/JSLib/WebAssembly/WebAssembly.cpp:576`) is
the single place Wasm bytes become a bytecode provider, and it already has
exactly the two branches this needs: one loads precompiled `.hbc`, the other
compiles `.wasm`. The `.wasm` branch becomes: ask `lookup`; on a hit take the
existing precompiled path; on a miss compile, serialize, `store`, and load
from those same bytes.

Compiling and then loading from the serialized bytes -- rather than running
the in-memory provider directly -- costs one extra parse on a miss and buys
the property that a hit and a miss execute identical bytecode. This is what
`compileCacheRun` already does for JavaScript.

### What does not change in Hermes

**No trust gate moves.** A cache hit is embedder-supplied and takes the path
`fromHermesURL` already uses, which is ungated because, as the interface
documentation puts it, the embedder is trusted by definition.
`EnableUntrustedBytecodeFromJS` and `EnableWasmBytecodeContentSniffing` both
stay off, and nothing reachable from JavaScript gains a way to feed bytecode
in. In particular this design does **not** need
`--vm=-Xenable-untrusted-bytecode-from-js`, which hermes-node's `--vm=`
classification does not currently accept anyway.

**No JavaScript-visible surface changes.** `WebAssembly.Module`, `compile` and
`instantiate` keep their specified behaviour, constructor identity and error
messages. Nothing is wrapped or patched.

## What changes in hermes-node

### The key

SHA-256 over `codegen_config` (as four little-endian bytes) followed by the
Wasm bytes, using the picohash already vendored for `--verify-natives`
(`lib/bundle/native_digest.cpp`).

`codegen_config` goes into the digest input rather than into the directory
name because the generation name is computed before any module is seen and
covers the JavaScript entries' concerns -- version, architecture, bytecode
version, and a CRC over the native CJS wrapper and compile flags. Folding a
per-module value into it would mean a second generation directory appearing
the first time a program ran with `--vm=-test262`, with the whole JavaScript
cache duplicated alongside.

SHA-256 rather than the CRC-32 the JavaScript entries use, because the roles
differ. A JS entry is keyed by path with CRC-32 and size as a *guard*: a
collision means two files sharing a name, and the guard catches it. A Wasm
entry's key *is* its content, so key and guard would be the same value, and a
CRC-32 collision would serve another module's bytecode with nothing left to
detect it.

The digest is the filename:

```
<root>/v1/<generation>/<ab>/w<64-hex-digest>
```

which is what lets this land with **no change to the on-disk entry format**. A
hit means a 256-bit digest matched a path; the existing 24-byte header's
`sourceCrc`/`sourceSize` remain the cheap truncation guard they already are.
The `w` prefix keeps Wasm names in a different space from the JavaScript
entries' 8-hex CRC keys, so the two share a generation directory without
anyone having to reason about whether an 8-character name could collide with a
64-character one.

### Storage

`compileCacheWriteEntry`, `compileCacheReadEntry` and `CacheMapping` are
reused unchanged: write-temp-and-rename, mmap on read, ownership transferred
through the finalizer. Reusing them is the reason to expand this cache rather
than build a second one -- the atomic write, the `<ab>` fanout, generation
naming and generation pruning all come along.

New surface is small: a `CompileCacheKind::kWasm`, a `lookupWasm()` keyed by
digest rather than path, and a translation unit adapting `CompileCache` to the
C callback struct and owning the `store_token` type.

### Wiring

`RuntimeState` already owns the `CompileCache`. Bootstrap registers the hooks
immediately after `napi_env` creation, before any user script runs, and
registers nothing under `--inspect`/`--inspect-brk` -- the same reason the
JavaScript cache is disabled there, that entries are compiled at
`DebugInfoSetting::THROWING` and the debugger needs `ALL`.

## Behaviour

**Controls are the existing ones.** `--compile-cache=<dir>`,
`--no-compile-cache`, `HERMES_NODE_COMPILE_CACHE`,
`HERMES_NODE_DISABLE_COMPILE_CACHE`, and
`HERMES_NODE_DEBUG_NATIVE=COMPILE_CACHE` tracing gains Wasm hit/miss lines. No
new flag: a second switch for a second entry kind is surface nobody asked for,
guarding the same failure.

**All run modes**, including a produced executable, where
`HERMES_NODE_DISABLE_COMPILE_CACHE=1` and the configuration file below are the
only controls, because every argument belongs to the program. This turns an
existing oddity into its intended shape: such an artifact already creates the
cache directory and writes nothing into it.

**Every failure degrades to compiling.** Absent file, short file, bad magic,
wrong header version, unreadable directory, failed write -- all of it is
best-effort and none of it surfaces to the program.

**A bad hit is self-healing, and that falls out of content-keying.** If cached
bytes are rejected by `createBCProviderFromBuffer`, or the module's top level
fails while descriptors are extracted, Hermes discards them and compiles.
Because the key derives from the content, the fresh `store` writes *the same
filename*, and temp-and-rename atomically replaces the bad entry. No explicit
invalidation is needed, unlike the JavaScript cache where a path-keyed entry
must be deleted.

**Concurrency needs nothing new.** Two processes compiling the same module
race to write the same name through temp-and-rename; last writer wins and both
are correct. This is how the JavaScript cache already behaves.

## Eviction and configuration

The JavaScript cache bounds *generations* -- `compileCachePruneGenerations`
keeps the current one plus the three most recently modified -- and nothing
bounds entries within a generation. That is tolerable there only because JS
entries are keyed by absolute path, so editing a file rewrites its one entry
rather than leaving another behind; entry count is bounded by distinct paths
ever run.

**Content-keying loses that property, and the loss is linear in build count.**
Every edit to a Wasm module yields a new digest and therefore a new file, and
the old one is never touched again. At the ratio measured here -- 2,388,624
bytes of bytecode from a 665,817-byte module, about 3.6x -- a developer
iterating on Rust or C++ compiled to Wasm leaves a full entry behind on every
build, and a hundred rebuilds is roughly 240 MB that nothing collects.

So Wasm entries get a size budget with LRU eviction:

- **Scoped to Wasm entries.** The `w` prefix identifies them. JavaScript
  entries keep the path-keyed, rewritten-in-place behaviour that never needed
  a bound, and one LRU over both would evict a cheap-to-recreate JS entry to
  make room for a Wasm one without knowing the difference in cost.
- **Swept at most once per process, on the first `save()`.** A save happens
  only on a miss, which is already paying seconds of compile, so a walk over
  the 256-way `<ab>` fanout is free by comparison -- and a process that only
  ever hits does no extra work at all. Evict oldest first until under budget,
  best-effort, failures ignored.

### The configuration file

Configuration lives in the cache directory, at the **root**, outside
`v1/<generation>/` so that generation pruning can never delete it:

```
# hermes-node compile cache configuration
recency: atime
max_wasm_bytes: 268435456
```

`recency: atime | mtime` selects the timestamp eviction orders by. **`atime`
is the default**, and gives real LRU: under `relatime`, the common mount
default, atime is updated whenever the existing atime is older than
mtime/ctime or more than 24 hours old, which is ample resolution for a cache
measured in weeks. The option exists for `noatime` mounts and filesystems
where atime is meaningless, where `mtime` gives insertion-order eviction
knowingly rather than silently.

`max_wasm_bytes` is the budget over `w`-prefixed entries, defaulting to 256 MB
-- a figure that is arbitrary as a constant and reasonable as a default, which
is the argument for it being in a file rather than in the source.

Written with its defaults when the cache directory is created, rather than
left absent, so the knobs are discoverable by looking -- the same instinct as
making the generation name readable so the active generation is answerable by
looking. Parsed like `kit.manifest`: a few `key: value` lines, one parser,
unknown keys and unparseable values ignored in favour of the default and
traced under `HERMES_NODE_DEBUG_NATIVE=COMPILE_CACHE`, because a malformed
configuration must degrade to a working cache rather than break a program.
Read once at startup; editing it takes effect on the next run.

A shipped `--build-exe` artifact therefore honours a configuration its user
can edit with no flags available to them, which is a better answer than an
environment variable for the same need.

## Testing

**The `codegen_config` test matters most**, because it is the only failure
here that is silent rather than loud: the same module compiled with and
without `--vm=-test262` must produce different cache entries. Get that
plumbing wrong and the cache serves bytecode built under the other bounds-
checking policy, with correct-looking output.

**Hermes.** A unit test with a fake cache asserting the call shapes and that
the token is consumed exactly once on every path -- hit, miss, and rejected
hit. Plus one test a fake cannot cheat: install a hook whose `lookup` returns
bytecode captured earlier, then hand `WebAssembly.Module` bytes that would
fail to compile; if the module works, the cached path was genuinely taken.
Separately a serialize-then-load roundtrip for the new entry point.

**hermes-node unit tests.** `CompileCacheTest` additions: digest key
derivation, the `w<64-hex>` filename shape, a Wasm entry surviving
`compileCacheWriteEntry`/`compileCacheReadEntry`, a one-byte edit to the Wasm
missing, and the eviction sweep under both `recency` settings with a small
`max_wasm_bytes`. Configuration parsing gets its own cases, including a
malformed file falling back to defaults.

**lit, `test/test-wasm-cache.js`**, `REQUIRES: wasm`, opting into its own
cache directory under `%t` as the existing compile-cache tests do, since the
suite sets `HERMES_NODE_DISABLE_COMPILE_CACHE=1` globally. Cases: two runs
produce identical output and the second traces a hit; an entry file appears;
`--no-compile-cache` leaves the directory empty; a truncated entry recovers
and is rewritten; `--inspect` populates nothing; two different modules produce
two entries; a `--bundle` run uses the cache; and, gated on
`linker-available`, a produced executable does too.

Hit and miss are asserted from tracing output, never from timing. The fixture
modules are tiny and the suite runs 16-way parallel, so a timing assertion
would measure scheduling noise -- which is how both known flaky tests got that
way.

**Measurement, not assertion.** `examples/hermes-parser-ast-wasm` is the
realistic check: a warm run should fall from the measured 4.7 seconds to near
zero. That number belongs in the progress file.

## Deliberately not in scope

**Baking Wasm bytecode into containers.** A `--build-bundle` that compiles
packaged `.wasm` ahead of time, so a container carries bytecode and a produced
executable starts instantly with no cache at all, is a format change (v6), a
producer change, and a change to what the scanner considers a packageable
file. The serialization entry point above is placed so that work is additive
rather than a redesign.

**A size bound on JavaScript entries.** Their path-keyed behaviour is
unchanged and already accepted; bounding them is a separate question with a
separate answer.

**True LRU independent of the filesystem.** Maintaining recency ourselves
would mean a write on every cache hit, on the fast path this feature exists to
make fast. `atime` under `relatime` is maintained by the kernel for free.

**Sharing entries across generations.** A version or bytecode-version bump
invalidates every Wasm entry wholesale, as it does for JavaScript.
