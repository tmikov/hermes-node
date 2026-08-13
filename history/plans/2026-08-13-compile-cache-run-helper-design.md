# Design: Shared Compile-Cache Execution Helper

**Status:** Design approved 2026-08-13. Implementation plan to follow.

## Goal

Give the compile cache one implementation of its hit/miss/fallback sequence,
instead of the two copies that exist today, and introduce the buffer
abstraction that makes memory-mapped zero-copy source possible later.

## Context

The on-disk compile cache (`history/plans/2026-08-12-bytecode-compile-cache-design.md`)
is consulted from two entry points, and each carries its own copy of the same
sequence:

- `compileFunctionForCJSLoaderCb` in `lib/bindings/node_contextify.cpp`
- `compileAndRunCallback` in `lib/module-loader/module_loader.cpp`

Roughly 55 lines each: look up, run mapped bytecode, and on failure clear the
pending exception, invalidate the entry, null the mapping, fall through,
compile, persist, run from the heap buffer with a freeing finalizer.

The final whole-branch review flagged this. The concern is not line count: the
feature's most safety-critical rule -- *a failure running cached bytecode is
swallowed and recompiled, a genuine compile error propagates* -- now exists in
two places, and any correction has to be made twice. The copies have already
drifted once: the module-loader version nulled `entry.mapping` but not
`entry.bytecode`, leaving a pointer into unmapped memory. That was Minor only
because nothing read it afterwards.

### The second problem: the source buffer type

Both call sites hand Hermes `source.c_str()` and `sourceLen + 1`, with a
comment explaining that the size counts a terminator that is not source. That
arithmetic is correct only because both callers happen to hold a
`std::string`, whose `c_str()` guarantees a NUL.

Hermes is designed around memory-mapped source and memory-mapped bytecode. An
mmap'd file is usually null-terminated for free: the kernel zero-fills the
tail of the final page, so `data[size]` reads as `'\0'` unless the file length
is an exact multiple of the page size, which is the rare case that needs a
copy. Neither `const std::string &` nor `std::string_view` can express "this
buffer may or may not have a readable terminator" -- the first demands an
owning, terminated heap string, and the second discards the information
entirely, forcing a copy on every compile.

This matters directly. The measured floor on a warm run is reading 18.6 MB of
source in JavaScript, converting it to UTF-8 and hashing it; the cache removes
parse and compile, not I/O. Reading module source natively and mapping it is
the obvious next lever, and it lands on exactly this interface. A signature
that cannot carry an unterminated mapping would foreclose it.

Hermes already works around the gap: `hermes/API/napi/hermes_napi.cpp:620`
defines `WeirdZeroTerminatedBuffer`, named for the awkwardness of folding the
terminator into the size.

## Scope

In scope:

- A `SourceBuffer` type: `hermes::Buffer` plus a terminator flag.
- A `compileCacheRun` helper holding the cache-enabled sequence once.
- Both call sites rewritten to use it.
- Widening the storage API's hashed-value parameters to `std::string_view`.
- Fixing the 4096-byte `sourceURL` truncation in `compileAndRunCallback`.
- A unit test for the helper, on a real runtime.

Out of scope:

- Actually memory-mapping source. This change makes it expressible; nothing
  maps anything yet.
- The uncached fallbacks. See below -- they are not duplicates of each other.
- Any change to `libjs/`, `libjs-node/`, or the `hermes/` submodule.
- Any behaviour change. The seven existing compile-cache lit tests must pass
  unchanged; that is the evidence this is a refactor.

## Design

### What the helper does and does not cover

The helper covers only the **cache-enabled** paths: a hit, and a miss with a
cache present. Each call site keeps its own uncached fallback, behaviourally
unchanged -- with one edit: where that fallback passes a length to Hermes it
takes it from the `SourceBuffer` rather than writing `sourceLen + 1`, so the
bare arithmetic disappears from the tree entirely rather than surviving in the
one branch the helper does not cover.

Those fallbacks look similar but are different code, and the difference is
load-bearing. `compileFunctionForCJSLoaderCb` calls `napi_run_script` on a
JavaScript string; `compileAndRunCallback` calls `hermes_run_script` on the
buffer with `enable_ts` and `persistent`. `napi_run_script` compiles with
`compileFlags.debug = true` under `HERMES_ENABLE_DEBUGGER`, which is the
entire reason the cache is disabled under `--inspect`. Unifying them would
need a strategy callback to save six lines each, and they were never copies of
one another. The duplication that matters -- swallow-versus-propagate and
mapping ownership -- is wholly inside the cache-enabled branch.

### `SourceBuffer`

New header `include/hermes/node-compat/compile-cache/source_buffer.h`,
belonging to the run library (which links Hermes anyway).

```cpp
/// A hermes::Buffer that records whether a NUL byte follows its contents.
///
/// SIZE NEVER COUNTS THE TERMINATOR. size() is the length of the source text
/// alone; when isNulTerminated() is true the NUL lives at data()[size()],
/// one past the last source byte, and is readable. This is deliberately the
/// opposite of the hermes_compile_to_bytecode convention, where the caller
/// folds the terminator into the size it passes. The two are easy to confuse
/// -- which is the whole reason this class exists -- so the difference is
/// stated here and asserted below.
///
/// Hermes compiles zero-copy from a terminated buffer and copies internally
/// otherwise. A memory-mapped file satisfies it for free, since the kernel
/// zero-fills the tail of the final page, except when the file length is an
/// exact multiple of the page size. hermes::Buffer cannot express the
/// distinction, which is why the NAPI layer works around it with
/// WeirdZeroTerminatedBuffer (hermes_napi.cpp:620).
///
/// Accessors stay non-virtual, inherited from hermes::Buffer. Only
/// destruction is polymorphic, because ownership is the only thing that
/// differs between a mapping, a heap block and a std::string.
class SourceBuffer : public hermes::Buffer {
 public:
  /// True when data()[size()] is readable and is '\0'.
  bool isNulTerminated() const {
    return nulTerminated_;
  }

  /// Number of bytes readable at data(). Includes the terminator when this
  /// buffer has one, so it is size() + 1 for a terminated buffer and size()
  /// otherwise. APIs that want the terminator counted in the length they are
  /// given take this instead of size().
  size_t readableSize() const {
    return nulTerminated_ ? size() + 1 : size();
  }

 protected:
  SourceBuffer(const uint8_t *data, size_t size, bool nulTerminated)
      : hermes::Buffer(data, size), nulTerminated_(nulTerminated) {
    assert((data != nullptr || size == 0) && "null data with nonzero size");
    // Reads the byte the caller just promised is readable. A caller that
    // lies fails here, rather than silently extending the compiled text by
    // one byte or reading out of bounds deep inside Hermes.
    assert(
        (!nulTerminated || data[size] == '\0') &&
        "isNulTerminated set but data[size] is not 0");
  }

 private:
  const bool nulTerminated_;
};
```

`readableSize()` exists so no call site decides. An unconditional `size() + 1`
is wrong for an unterminated buffer, and a conditional at each call site is
the same convention-not-enforcement this class was written to remove.

One concrete subclass now, because it is what both callers hold:

```cpp
/// Borrows a std::string. c_str() guarantees the terminator; the assert in
/// the base checks it rather than assuming it.
class BorrowedStringSourceBuffer final : public SourceBuffer {
 public:
  explicit BorrowedStringSourceBuffer(const std::string &s)
      : SourceBuffer(
            reinterpret_cast<const uint8_t *>(s.data()),
            s.size(),
            true) {}
};
```

A mapping-owning subclass is the point of the abstraction and is deliberately
not part of this change.

### The helper

New header `include/hermes/node-compat/compile-cache/compile_cache_run.h`,
source `lib/compile-cache/compile_cache_run.cpp`.

```cpp
/// Produce a runnable value for a module's source, serving it from the cache
/// when a valid entry exists and compiling, persisting and running it
/// otherwise.
///
/// What gets compiled is always what gets hashed, plus the caller's wrapper:
/// \p source is hashed and forms the body, and \p wrapPrefix / \p wrapSuffix
/// are concatenated around it before compiling. That invariant is what lets
/// the wrapper text live in the generation directory name instead of being
/// hashed per entry. Passing a finished string instead would allow a caller
/// to key an entry by text that did not produce it.
///
/// Never opens or closes a handle scope; the caller owns scoping.
///
/// Returns napi_ok with \p result set on success. Returns a non-ok status
/// with the originating exception still pending when the user's own source
/// fails to compile, or when freshly compiled bytecode fails to run -- the
/// caller propagates both. A failure to run CACHED bytecode is never
/// reported: the exception is cleared, the entry deleted, and the source
/// recompiled, because a corrupt cache file must not surface as a
/// SyntaxError in valid user code.
napi_status compileCacheRun(
    napi_env env,
    CompileCache &cache,
    CompileCacheKind kind,
    const SourceBuffer &source,
    std::string_view wrapPrefix,
    std::string_view wrapSuffix,
    const char *sourceUrl,
    napi_value *result);
```

`std::string_view` for the wrappers, not `const std::string &`: binding
`kCJSWrapperPrefix`, an `inline constexpr const char[]`, to a
`const std::string &` materialises a temporary, allocating and copying a
compile-time constant once per module compiled.

`sourceUrl` stays `const char *` because the Hermes C API takes a
null-terminated string there; a view would force a copy to add the
terminator.

**Concatenation happens only on a miss, and only when needed.** When both
wrappers are empty and `source.isNulTerminated()`, the helper compiles
straight from `source.data()` and `source.readableSize()` with no copy,
preserving what `compileAndRunCallback` does today. Otherwise it builds
`wrapPrefix + source + wrapSuffix` into a `std::string` local to the call,
wraps that in a `BorrowedStringSourceBuffer` whose lifetime is bounded by that
local, and compiles from it. Building the wrapped
string unconditionally would add roughly 18.6 MB of pointless concatenation
per warm run on the flow-bundler workload -- small in time, about 2 ms against
2.7 s, but an avoidable regression in a change whose only purpose is tidiness.

### Compile and run flags come from the kind

A table in `compile_cache_run.cpp`, deliberately not in the storage header, so
`CompileCacheKind` stays purely about identity:

| kind | `enable_ts` | `persistent` |
| --- | --- | --- |
| `kCommonJS` | false | false |
| `kLoaderWrapped` | false | true |
| `kLoaderWrappedTS` | true | true |

For `enable_ts` this is a correctness requirement, not tidiness. It is a
compile flag: it changes the bytecode produced from identical source text. If
a call site could pass `kLoaderWrapped` with `enable_ts` true, the same file
would hash to the same key under both settings and a later non-TypeScript
lookup could be served TypeScript-compiled bytecode. The kinds exist to keep
differently-compiled artifacts apart; letting the flag vary independently of
the kind makes that violable by a typo.

`persistent` is a `RuntimeModuleFlags` setting that does not affect the
compiled output and so has no bearing on cache identity. It is derived here
only because it too is a fixed property of each call site.

The `switch` has no `default:`, so adding a kind fails to compile until its
flags are stated.

### Storage API widening

Parameters whose values are only hashed or concatenated become
`std::string_view`:

- `CompileCache::lookup(entry, source, filename, kind)`
- `compileCacheKey(filename, kind)`
- `compileCacheGenerationName(version, arch, bytecodeVersion, configCrc)`

Parameters that become a null-terminated path for a syscall keep
`const std::string &`, because they need `.c_str()` and a view would move the
allocation rather than remove it:

- `compileCacheWriteEntry(path, ...)`
- `compileCacheMakeDirs(path)`
- `compileCachePruneGenerations(versionedRoot, keepName, keepCount)`

This is a source-compatible widening -- `std::string` converts implicitly --
so every existing caller and all 35 unit tests compile unchanged.

It is also what lets the helper hash a `SourceBuffer` without copying: it
bridges the two at one point, passing
`std::string_view(reinterpret_cast<const char *>(source.data()), source.size())`
to `lookup`. Note `size()`, not `readableSize()` -- the terminator is not part
of the source text and must not be hashed, or a terminated and an unterminated
buffer holding identical text would key to different entries.

### The sourceURL truncation fix

`compileAndRunCallback` currently reads the URL into `char urlBuf[4096]` via
`napi_get_value_string_utf8`, which silently truncates anything longer. The
truncated value is then used as the cache key's filename, so two long paths
sharing a 4095-byte prefix would collide on one entry and evict each other.
A path can legitimately be large -- a data URL, for instance.

Read the length first, size a `std::string` to it, then read into that. The
helper's parameter stays `const char *`.

### Build layout

A second target in `lib/compile-cache/CMakeLists.txt`:

- `hermesNodeCompileCache` -- unchanged. Storage: CRC32, keys, entry files,
  generations. Links `zlib_a` and nothing else, and includes no Hermes header.
- `hermesNodeCompileCacheRun` -- new. Execution: binds storage to Hermes.
  Links `hermesNodeCompileCache` publicly for the types, plus the Hermes NAPI
  headers privately.

Keeping them separate preserves what the storage library's comment already
demands, and keeps `CompileCacheTest` -- 35 tests that need no runtime --
linking only zlib.

`lib/bindings` and `lib/module-loader` each link `hermesNodeCompileCacheRun`
in place of `hermesNodeCompileCache`.

## Testing

**New `CompileCacheRunTest`**, a GTest binary borrowing the runtime and
`napi_env` fixture shape from `unittests/ModuleLoaderTest.cpp`, driving the
helper directly:

- A miss compiles, persists an entry, and returns a callable.
- A hit serves from the entry without recompiling.
- A corrupted entry is swallowed and recompiled, the call succeeds, and **no
  exception is left pending afterwards**.
- A genuine syntax error returns non-ok with the original `SyntaxError`
  intact.

The last two are the point. They test the swallow-versus-propagate rule
directly rather than inferring it from program output, and the pending-state
assertions catch a failure invisible end to end: a swallow that leaves a stale
exception pending would corrupt the next unrelated napi call, not the current
one.

**Existing coverage must pass unchanged**: the seven compile-cache lit tests
(`compile-cache-enable.js`, `-cjs.js`, `-typescript.ts`, `-corrupt.js`,
`-syntax-error.js`, `-ts-throws.js`, `-inspect.js`) and all 35
`CompileCacheTest` unit tests. This is a behaviour-preserving refactor; those
passing untouched is the evidence.

**Add `SourceBuffer` tests to `CompileCacheRunTest`**, not to
`CompileCacheTest`: that `readableSize()` returns `size() + 1` when terminated
and `size()` when not, and that `BorrowedStringSourceBuffer` reports
termination. These need no runtime, but `SourceBuffer` derives from
`hermes::Buffer` and so needs the Hermes headers, which `CompileCacheTest`
deliberately does not have -- keeping it free of them is the reason for the
two-target split in the first place.

## Known limitations

- Nothing memory-maps source yet. `SourceBuffer` makes it expressible; the
  mapping-owning subclass and the native file read are separate work.
- The `nulTerminated` flag is dead weight until something constructs an
  unterminated buffer. That is accepted: the alternative is a signature that
  has to change when the first one appears.
- Deriving `persistent` from `kind` means a future call site wanting an
  existing kind with different run flags needs a new kind. For `enable_ts`
  that is correct by construction; for `persistent` it is a judgement that
  call-site policy belongs with the call-site name.
- The constructor assert reads `data[size]`. A caller that wrongly claims
  termination over an unmapped byte faults in the assert rather than
  reporting cleanly -- which is the intent, and under ASAN the overread is
  caught even before the assert.
