# Design: baking compiled WebAssembly into a bundle

**Status:** Draft for review, 2026-09-09.

An AOT container carries compiled JavaScript and never compiled
WebAssembly, so a bundled program that instantiates a Wasm module pays the
whole compile at every launch unless a disk cache happens to be warm. This
puts the compiled bytecode inside the container, where it travels with the
artifact and reaches a `--build-exe` executable for free.

Builds directly on `docs/superpowers/specs/2026-09-07-wasm-compile-cache-design.md`:
the Hermes-side hook that design added is the entire mechanism used here, and
**no Hermes change is needed**.

## The problem

`--build-bundle` compiles every JavaScript file to bytecode so that a
`--bundle` run and a produced executable compile nothing. A WebAssembly
module inside one is the exception: Hermes compiles it ahead of time to
Hermes bytecode at the point `WebAssembly.Module` receives the bytes, and a
container has nowhere to put the result.

Measured on `examples/hermes-parser-ast-wasm` (Release, one module whose
cached bytecode entry is 2,054,332 bytes): cold 5.03 s, warm 0.07 s. Delete
only the Wasm entry from an otherwise fully warm cache and the run costs
5.2 s again, so compiling that single module is essentially the whole cold
run. The disk cache already removes that cost for a developer running the
same program repeatedly. It does not remove it for the case a container
exists to serve:

- A produced executable is shipped to someone who has never heard of
  hermes-node. Their first run pays the full compile, and their machine's
  cache is cold by construction.
- Any environment that starts from a clean cache -- CI, a container image, a
  fresh checkout -- pays it on every job.
- `HERMES_NODE_DISABLE_COMPILE_CACHE=1`, or a read-only or absent
  `$HOME/.cache`, pays it every time.

The artifact is supposed to be the thing that has already done the work. For
JavaScript it is; for Wasm it is not.

## Scope: the compile, not the packaging

The bytes a program hands to `WebAssembly.Module` reach it in one of two
ways, and they are not equally tractable.

1. **The bytes are already inside a packaged JavaScript module** -- an inline
   byte string, a base64 string, a typed-array literal; the encoding does not
   matter. `examples/hermes-parser-ast-wasm` is exactly this, and its actual
   shape is worth knowing because it is easy to guess wrong: emscripten emits
   the module as a **raw binary string literal** inside
   `HermesParserWASM.js`, decoded by a `charCodeAt` loop
   (`findWasmBinary()` -> `binaryDecode()`), and `getBinarySync()` is the
   identity function because the "file" already is the bytes. Not base64 --
   the file contains NUL bytes and `file(1)` calls it `data`, which is also
   why `grep` silently refuses to match in it without `-a`. The bytes are
   UTF-8-encoded in the source, so 846,106 bytes of literal decode to
   **768,303** characters, each 0-255 -- confirmed by intercepting
   `WebAssembly.Module`, which sees the same 768,303 under both node and
   hermes-node. (Reading the file as latin1 and `eval`ing the literal gives
   831,577 and is wrong; both engines parse the source as UTF-8.)
   The producer packages it as ordinary JavaScript with no
   `--include` and nothing staged beside the container, so the bytes arrive
   at run time for free.
2. **The bytes come from a standalone `.wasm` file read with `fs`.** The
   producer does not package data files, so today that file must ship beside
   the container for the program to work at all.

**This design covers the compile, for both cases. What it does not cover is
packaging the bytes.** The distinction matters and the first draft of this
document got it wrong. The cache callback receives bytes and a codegen
configuration and nothing about their origin (`hermes_napi.h:309`), and the
digest is over content with no path in it, so a `.wasm` file shipped beside
the container is recorded and baked exactly like a blob inside a JS
module -- the `fs` read still happens at run time, and the compile that
follows it hits the container. Case 1 is the one that needs nothing else;
case 2 works too, but the `.wasm` file must still travel beside the artifact,
as it must today.

What is left for a later step is removing that second file: teaching the
container to answer the `fs` read, and then carrying only bytecode with a
token where the `.wasm` was. Both are reachable -- there is no streaming path
in this runtime, so every Wasm load here goes through bytes the program
already holds, and the interception point is `fs` rather than a Wasm API:

```
$ hermes-node -e 'console.log(typeof fetch, typeof WebAssembly.instantiateStreaming)'
undefined undefined
```

Neither is designed here. Nothing below forecloses them: the container
section this adds is keyed by content, so bytes that arrive by a future route
hit the same entries.

### The module ends up in the container twice

This falls out of case 1 and is the feature's real cost, so it belongs here
rather than in a footnote. A baked container carries the module's **source
bytes** -- inside the compiled JavaScript that holds them -- and its
**compiled bytecode**, and both are load-bearing. The source copy cannot be
dropped: content keying means the digest is computed over the bytes the
program hands to `WebAssembly.Module`, so the program has to produce them
before the container can answer. The bytecode copy is the point of the
feature.

Measured on `examples/hermes-parser-ast-wasm` (Release):

| | bytes |
| --- | --- |
| the Wasm module, raw | 768,303 |
| `HermesParserWASM.js` compiled to bytecode, which holds it as a string | 1,620,013 |
| the baked bytecode entry | 2,054,308 |
| whole baked container | 3,912,872 |

So about 94% of that container is the same module twice, and the first copy
costs roughly **twice** the raw bytes, because a string literal containing
any character above U+007F is stored as UTF-16. Measured directly rather than
inferred: 100,000 ASCII characters compile to a 100,560-byte container,
100,000 characters drawn from 0x00-0xFF to 200,552. 768,303 characters at two
bytes each is 1,536,606, and the module compiles to 1,620,013 -- the
difference being the emscripten runtime around it.

**Decoding that string is now a visible share of startup.** `binaryDecode` is
a `charCodeAt` loop over every byte, and with the compile gone it is what
remains: measured on the real literal, 38 ms under hermes-node against 1 ms
under node. The baked run of this example is 60 ms in total.

The gap is interpretation against compiled code -- `hermes/lib/VM/JIT/` has
an `arm64` backend and no x86-64 one, so there is no JIT on this machine to
enable, and `--vm=-Xjit=on` measures nothing here. Nor would a JIT
necessarily close it: one compiles hot functions, and this is a single pass
over a large loop.

Nothing in this feature can fix it either -- the loop belongs to the
program -- but it is the reason a baked container does not go to zero, and it
is what step 3 would remove, since a token needs no decoding.

Removing the first copy is step 3 of the scope above -- carry only bytecode
and leave a token where the bytes were -- and it is not designed here. It is
also the step with the sharpest trade: a program that inspects its own Wasm
bytes, validates them, or hashes them would see the token instead.

## The decision

1. **A run records what it compiled; a build bakes that recording in.**
   `--record-wasm=<file>` writes a record file naming every Wasm module the
   run compiled or looked up. `--build-bundle --bake-wasm=<file>`
   (repeatable) copies those entries into the container.
2. **The producer never executes the program.** It cannot see a byte string
   decoded, so it cannot obtain the bytes; only a run can. Keeping the two
   apart preserves the invariant that makes `--vm=` recorded rather than
   applied at build time, and it means the recording run can be anything --
   the unbundled program, a test, several runs.
3. **The record file carries the bytecode, not a reference to a cache
   entry.** A reference is smaller and was the first choice; it is valid only
   while the cache is, and `cache prune`, the `max_wasm_bytes` sweep or a
   `cache clean` between the run and the build would invalidate it silently.
4. **Format v6 adds a Wasm table to the container**, in the shape the preload
   and native tables already use.
5. **At run time the container is a cache tier consulted before the disk
   cache**, through the same `hermes_set_wasm_cache` hooks. A container miss
   behaves exactly as today.
6. **The hooks are installed whenever any tier could answer** -- a disk
   cache, a recorder, or a container about to be opened -- not only when a
   disk cache exists. This reverses an existing early return, and without it
   a baked container would be invisible to every configuration that disables
   the cache, including a shipped executable on a machine with no writable
   cache directory.
7. **`--build-exe` gains no flag.** The entries are inside the container and
   travel with it.

## The record file

### Why a purpose-built format

`tar` was considered, and the deciding argument is the read side rather than
the write side. Writing tar is easy: 512-byte headers, octal fields, a
checksum. Reading it robustly is not -- ustar against GNU against pax, the
long-name extensions -- and it raises a question with no requirement behind
it: whether to accept archives we did not write. Nothing in `external/`
is an archive library, and the codebase's habit is the opposite: small
purpose-built formats with an explicit version and validation at every read
(the container, the compile-cache entry, `kit.manifest`).

It also buys a simplification. The record file's contents and the
container's new section are nearly the same shape -- a digest and a bytecode
blob, repeated -- so baking is an append with relocated payload offsets
rather than a translation.

What tar would have given free is `tar tvf`. That is bought back deliberately
by `--dump-wasm` below.

### Layout

```
magic[8]            "HNWASMRC"
formatVersion       u32
count               u32
versionOffset       u32    versionLength u32
recordTableOffset   u32
payloadOffset       u32    payloadSize   u32
--- version bytes, padded to 4
--- record table: BundleWasmRecord[count]
--- payload: bytecode blobs, each 8-aligned
```

Fixed-width header; everything variable is reached through an offset. That
is not decoration. A length-prefixed version string written inline ahead of
the record array leaves that array at an arbitrary byte offset -- 21 bytes in
for a version of `0.3.0` -- and reading a `u32` there is undefined behaviour
whatever x86-64 tolerates. The container reader already refuses a misaligned
table for exactly this reason (`bundle_reader.cpp:193-198`), and this file
does the same: the writer aligns `recordTableOffset` and `payloadOffset`, and
the reader validates both before casting.

A fixed `char version[64]` would remove the question and is rejected: the
string is `git describe` output, so a cap is a landmine that fires years
later on a long tag, and truncation handling costs more than an offset pair.

`versionOffset`/`versionLength` hold `HERMES_NODE_VERSION_STRING`. The digest
is the raw 32 bytes rather than the 64-hex spelling, matching
`BundleNativeRecord::hashString` and `kNativeDigestBytes`;
`compileCacheWasmDigest()` returns the hex form, so there is exactly one
conversion and it happens where the file is written.

One record serves both files:

```cpp
struct BundleWasmRecord {
  uint8_t digest[kNativeDigestBytes]; // raw SHA-256, not hex
  uint32_t payloadOffset;
  uint32_t payloadSize;
};
```

**Nothing checksums a payload, in either file.** Both are validated
structurally -- magic, format version, and every offset, length and range,
which is what keeps a reader from walking off the end of a mapping -- and
neither verifies the bytes inside a payload.

A CRC over the record file's payloads was drafted and cut. It would have
guarded against a file damaged between the recording run and the build: same
machine, minutes apart, no transfer in between.

Removing it **accepts undetected corruption** rather than postponing a
diagnostic, and that is worth saying plainly. Damage bad enough for Hermes to
refuse the bytecode does terminate the run -- but damage that leaves the
header valid does not, and executes. The container has always been exposed
that way (`bundle_reader.h`), the fatality rule narrows the window rather than
closing it, and the trade taken here is the same one the container already
takes: no per-payload verification, because a partial guarantee is not worth
a permanent cost.

The container follows the policy it already has (`bundle_reader.h`): payloads
are unverified, because loading it must cost nothing. Integrity of a
container is all or nothing -- either the whole file is checksummed and a bad
one is refused, or none of it is -- and that is a separate change to the
format, not something this feature introduces one entry kind at a time.

**The digest is inline rather than a string-table index**, which deviates
from `BundleNativeRecord::hashString` deliberately. A native record's digest
is metadata: it is read by `--dump` and `--verify-natives` and by nothing on
the run path, so where it lives costs nothing. This digest *is* the key, and
every Wasm lookup binary-searches on it -- an index would put a string-table
dereference inside each step of that search. Inline in both files, so the
digest crosses into the container untouched and only the payload offset is
recomputed.

### When it is written

The file is written **once when the runtime is created -- after the
flag-conflict checks, before user code runs** -- and rewritten atomically on
every change after that.

The ordering is load-bearing, not incidental. `checkToolOptions()` runs after
the parse loop (`hermes-node.cpp:955`), and the same-file refusal below lives
there; a recorder that wrote at parse time would have already overwritten
`app.js` in `--record-wasm=app.js app.js` before the guard that exists to
stop it ever ran.

Rewriting rather than writing at the end is a correctness choice, not an
optimisation. Writing at the end of `runHermesNode` would lose the file
whenever the program called `process.exit()`: both exit paths call `_exit()`
deliberately, to keep ASAN from reporting the live Hermes runtime as
thousands of leaks. That is the same hazard the stdio flush had to solve, and
a recording run of a CLI that ends in `process.exit(0)` is an ordinary thing
to do. Rewriting also survives an uncaught exception and a crash midway. A
run compiles one to three Wasm modules, so the cost is nothing.

The write at startup earns its place twice. A program that instantiates no
Wasm still produces a well-formed empty recording, which is what the
zero-entry case below assumes exists -- without it, "no Wasm" and "no
recording run happened" would be the same state, and a stale file from a
previous run would silently be baked instead. And an unwritable path fails
immediately, before the program runs, rather than after it has done its work.

**A recording run replaces the file; it never merges with what is there.**
Merging is what repeated `--bake-wasm` flags are for. A recorder that
accumulated would make a stale entry impossible to remove without deleting
the file by hand.

Within one run, entries are keyed by digest, so a program that instantiates
the same module twice contributes one. **A `store` replaces whatever a lookup
recorded for that digest**, which is not a detail: see the rejected-hit path
below, where the bytes a lookup returned are exactly the bytes that must not
be kept.

### What a recording run needs

Nothing. `--record-wasm` is orthogonal to the compile cache, and so is a
baked container -- which forces a change to when the hooks are installed at
all. See "The install rule" below; it is the one place this design has to
undo an existing decision rather than extend one.

Requiring an enabled cache was offered and declined, and it is the more
expensive side: "the cache is enabled" is not one flag but two flags and an
environment variable, so it would need three refusals, one of them forbidding
`--record-wasm --inspect` -- two features with nothing to do with each other.
A cache-off recording run against a container with nothing baked in it is
also the one to trust most, because every module genuinely compiles and
nothing depends on what the cache happened to hold. With a baked container it
is no longer true that everything compiles -- the container tier answers
first whether or not a disk cache exists, which is the point of the install
rule -- and the recording is still complete, because a container hit is
recorded exactly like a disk hit.

The recording captures bytes wherever they become known: returned from a
container hit, returned from a disk hit, or handed to `store` after a
compile. That is what makes a second recording run against a warm cache
produce the same file as the first. Recording only stores would produce an
empty file on the second run -- a build that silently gets slower the more
you use it.

## What changes in the container: format v6

`BundleHeader` gains two fields:

```cpp
  uint32_t wasmTableOffset;
  uint32_t wasmCount;
```

and the table is `BundleWasmRecord[wasmCount]`, sorted by digest so a lookup
binary-searches it with `memcmp`. `payloadOffset` is relative to
`header.payloadOffset`,
exactly like a module's, so the writer's payload cursor and the reader's
bounds checks are reused unchanged, and Wasm bytecode lands at the existing
`kBundlePayloadAlign`.

A section of its own, for the reason the preload and native tables are ones:
a real container has ~1500 modules and one or two Wasm entries, so three more
fields on every module record would be twelve bytes of zeros per module to
describe the exception.

`kBundleFormatVersion` goes 5 -> 6, and two things about that are worth
stating exactly, because the first draft of this document got both wrong.

A format-version mismatch is fatal in **both** open modes.
`openForInspection()` relaxes only the generation check
(`bundle_reader.cpp:130` against `:136`), so a v5 container cannot even be
`--dump`ed by a v6 binary. That is how every previous bump has behaved and
nothing here changes it.

And adding two header fields changes the bytes of **every** container,
including one with no Wasm entries: the writer derives its section positions
from `sizeof(BundleHeader)` and always stamps the version
(`bundle_writer.cpp:161`, `:193`). The invariant that holds is behavioural and narrower than
"unchanged": a container with no Wasm entries runs exactly as before, and its
dump gains no `WASM` section, because that section prints only when non-empty.
Its dump does still differ in the two places a dump always reflects the
format -- the header's format version and the total file size
(`bundle_tools.cpp:208`, `:392`) -- and in the new `wasm` row under
`SECTIONS`, which is unconditional like `vmopts`.

`wasmTableOffset` joins the reader's alignment check
(`% alignof(BundleWasmRecord)`) and its bounds validation, alongside the four
tables already there.

## What changes at run time

### The install rule

**The hooks are installed whenever any tier could answer, not whenever a
cache exists.** `installWasmCacheHooks` returns early today on a null cache,
and Hermes calls nothing at all when `hooks.installed()` is false
(`WebAssembly.cpp:660`). Left alone, that early return would make a baked
container invisible under `--no-compile-cache`, under
`HERMES_NODE_DISABLE_COMPILE_CACHE=1`, under `--inspect`, and on any machine
with no writable `$HOME/.cache` -- which is precisely the deployment case
this feature exists for. A shipped executable whose user has the cache
disabled would silently compile its Wasm on every launch, with a container
full of bytecode it never looked at.

So the hook context becomes a small struct with two optional members -- a
`CompileCache *` and a recorder -- and installation is decided by whether
**any** tier could be active:

```
install if  cache != nullptr
         || recorder != nullptr
         || !config.bundlePath.empty()          // --bundle=<f>
         || config.embeddedBundleData != nullptr // a produced executable
```

The last two are the whole of the fix: both are known before the runtime
exists (`hermes_node_runtime.cpp:1481-1485` branches on exactly them), so
"a container will be opened" is answerable at install time even though the
container itself is opened later.

**Installing unconditionally instead would not be free, and the first draft
said it was.** Hermes sets `cacheUsable` from `hooks.installed()`, not from
what a lookup found (`WebAssembly.cpp:659`), so an installed hook that always
misses still makes every Wasm compile serialize its bytecode, hash the input
with SHA-1 (`WasmCompile.cpp:167`), call `store`, and reload from the stored
bytes. A null token does not suppress any of it -- the ABI has no "cache
unusable" channel, the same gap the disk cache's OOM note already records.
That is real work added to every cache-disabled unbundled run, which is why
installation is conditioned rather than unconditional.

One residual case remains and is accepted: a `--bundle` run with the disk
cache disabled and a container carrying **no** Wasm entries pays that
serialization on each Wasm compile. Narrowing it further would mean reading
the container's header before the runtime exists -- which
`readBundleVmOptions` already does for VM options, so it is possible -- for a
saving that is a fraction of the compile it follows.

The digest is computed once, in the callback, and passed to whichever tiers
are active. `CompileCache::lookupWasm` takes it as a parameter rather than
deriving it again: two derivations from the same inputs can drift, and the
existing code already avoids that between lookup and store for the same
reason.

### Lookup order

**Container first, then the disk cache.** A container miss falls through to
exactly today's path, and a module the container did not bake is still saved
to the disk cache normally -- the two tiers cooperate rather than one
replacing the other.

**The container is read lazily, per lookup, through `openBundleState()`.**
Not captured at install time, and this is forced rather than stylistic:
`installWasmCacheHooks` runs during runtime creation
(`hermes_node_runtime.cpp:820`), while `openBundle()` runs later, from
`runBundle(env, ...)`, once the env already exists. Reading the global at
install time would find nothing, every time.

A container hit hands Hermes a pointer straight into the container's mapping
with `finalize_cb` NULL, which the ABI allows for an externally managed
buffer. The mapping outlives the run, so there is nothing to release and no
finalizer ordering to reason about.

**That is not the same as allocation-free, and this design initially claimed
it was.** The token allocates, the digest allocates its 64-character string,
and Hermes copies the returned bytes into a `MemoryBuffer` before building a
provider from them (`WebAssembly.cpp:673`). What the container path avoids is
a second file mapping and a finalizer, not allocation, so it sits inside the
same OOM caveat the disk cache's Wasm hooks carry, not outside it.

Returned bytes take the trusted precompiled-bytecode path, exactly as disk
cache hits do. No trust gate moves.

### A baked entry Hermes refuses is fatal

Hermes does not execute returned bytes directly. It builds a bytecode
provider from them, which validates the header -- magic, bytecode version,
structural sanity -- and on failure returns no provider, keeps the store
token, compiles the module from source, and calls `store`
(`WebAssembly.cpp:673-710`).

**So `store` arriving for a token whose lookup was a container hit means one
thing: this container's bytecode was refused.** That is unambiguous, it costs
nothing to detect, and the response is to print the container and the digest
and **terminate** -- not to carry on with the freshly compiled module.

A broken artifact is not a slow artifact. `BundleReader::open()` already
treats a structurally invalid container as fatal, and a payload the engine
refuses is the same class of thing. Continuing would make this the one place
in the bundle path that tolerates a bad container, and it would hide the
problem behind nothing worse than a longer startup, which is exactly how it
would reach a user and stay there.

**The disk cache keeps the opposite rule, and the difference is the point.**
A refused *cache* entry falls back to compiling and says nothing, because a
cache is an optimisation with a right answer behind it -- the best-effort
contract this repo has written down and defends. A container is not an
optimisation. It is the artifact.

**The rule reaches provider rejection and nothing else.** That is narrower
than it first appears, and the limits are worth listing because each is a way
a damaged container does *not* terminate:

- **The recompile after a refusal fails.** Hermes calls `discard` instead of
  `store` (`WebAssembly.cpp:698-704`), so the termination does not fire --
  but this one is not silent: the `WebAssembly.Module` call fails and the
  program sees the compile error.
- **The recompile succeeds but serializes to nothing.** The store is guarded
  by `cacheUsable && !serialized.empty()` (`:706`) and the trailing
  `if (tokenOutstanding) discard` (`:723`) takes the rest. That `discard` is
  indistinguishable from the one a good hit receives, so this case runs the
  freshly compiled module and says nothing. It is the genuinely silent one.
- **The provider is accepted and the module fails afterwards.** Hermes
  discards the token as soon as a provider is built (`:681`), then runs the
  module's top level, which can throw, return a non-object, or produce
  invalid descriptors (`:741`, `:746`, `:760`). Those are ordinary catchable
  errors and no `store` ever happens.
- **Damage that leaves the header valid is not detected at all.** Those bytes
  run, and may run wrong. That is the exposure every payload in the container
  has (`bundle_reader.h`), and removing the checksums *accepts* it rather
  than deferring it.

So this is not "corruption is always caught". It is: when Hermes tells us it
refused the container's bytecode, we do not paper over it. Catching the rest
means a status the ABI does not return, or checksumming the whole container
at `open()` -- both out of scope here and recorded rather than implied.

### What the token has to carry

Hermes discards a token only after a provider is successfully built from the
returned bytes; a refused hit keeps it through the compile and into `store`.

So the token carries two things on every path, not only on a miss. **Which
tier answered**, which is what makes the termination above possible and
distinguishes it from a refused disk entry, where falling back is correct.
And **the disk entry's identity**, so a `store` following a refused disk hit
still has somewhere to write.

The recorder must also let a `store` **replace** what the lookup recorded for
the same digest. A disk hit records the bytes it returned; if Hermes refuses
them, the recording has to end up holding what was compiled instead, or the
next bake carries the bad entry forward.

## The bake step

`--build-bundle=<out> --bake-wasm=<file>`, repeatable. Each file is read, its
version string compared, and its entries added to the container's Wasm table,
deduplicated by digest across files.

**Adding, not copying: payload offsets are relocated.** A record file's
offsets are relative to its own payload region; the container assigns each
entry a fresh offset from the same cursor module payloads use
(`bundle_writer.cpp:177`). What the shared record struct buys is that the
digest and the length need no translation -- not that the bytes of the table
can be memcpy'd across.

**The version string is compared for exact equality against
`HERMES_NODE_VERSION_STRING`, and a mismatch is a hard error naming both.**
This is `kit.manifest`'s rule, and it is a **provenance policy rather than a
proof** -- a distinction the first draft blurred by claiming a different
build "produces digests that never match".

It does not. Hermes composes the codegen configuration from the bytecode
version, the Wasm codegen version and `t262` (`WebAssembly.cpp:568`), and
hermes-node's own build version is not in it. So two hermes-node builds over
one Hermes can produce identical keys, and conversely the same binary run
under `--vm=-test262` produces different ones, which means a same-build
recording can still miss. What the check actually buys is that a stale
recording is caught **at the moment it is baked**, where the error can name
the file, rather than becoming a container that quietly compiles at every
launch. Execution by a different build is refused independently, by the
container's generation tag (`bundle_reader.cpp:136`).

The check is commit-granular, like every other version currency here, so it
does not catch an edit to an already-dirty tree. That limitation is
inherited, not introduced, and it is the same one `kit.manifest` has.

Baking runs after compilation, alongside the native sidecar copies, so a
build that fails to compile has not yet touched the Wasm table.

A record file with **zero entries** is well-formed -- a recording run of a
program that instantiates no Wasm produces one -- and baking it warns rather
than failing. It is not corrupt, and the user may have several record files
of which only some carry entries; but a build that quietly bakes nothing when
the user asked for baking is worth one line on stderr.

## Tooling

**`--dump` gains a `WASM` section**: the count, then one line per entry with
the truncated hex digest and the bytecode length. Printed only when the
container carries at least one entry, mirroring the `NATIVES` rule.
`--verbose` on the bake lists each baked entry and the total.

**`--dump-wasm=<file>`** is a sixth tool verb, dispatched by `runToolVerb()`
before `runHermesNode` like the other five, so it needs no runtime, event
loop or `napi_env`. It prints the record file's format version, its recorded
build version and whether that matches this binary, then one line per entry:
truncated digest and bytecode length. This is what `tar tvf` would have given
for free, and it prints strictly more -- the version comparison is the thing
a person actually needs when a bake fails.

## Tracing

`HERMES_NODE_DEBUG_NATIVE=COMPILE_CACHE` gains events that distinguish the
tiers, and this is a requirement rather than a convenience: the existing
`wasm hit` trace fires when the entry is read (`compile_cache.cpp:464`),
before Hermes has accepted the bytes, and Hermes may reject them and compile
anyway. A test asserting "hit" therefore cannot currently prove that no
compile happened -- which is the only thing this feature claims.

| Event | When |
| --- | --- |
| `wasm container hit` / `wasm container miss` | the container tier answered |
| `wasm hit` / `wasm miss` | the disk tier answered (unchanged) |
| `wasm store` | bytes arrived from a compile |
| `wasm container refused` | a container hit came back to `store`; emitted just before terminating |
| `wasm record` | the recorder took bytes, naming the tier they came from |

A container hit that comes back to `store` is the refused-entry case. It
traces `wasm container refused` **before** terminating, so the trace explains
the exit rather than stopping mid-story; a healthy bake test asserts a
container hit, no `wasm store`, and a clean exit. Timing is never used for any
of this.

## Flag surface

Rows added to `checkToolOptions()`, each naming both flags, as that table
already does:

| Refused | Why |
| --- | --- |
| `--bake-wasm` without `--build-bundle` | nothing to bake into |
| `--record-wasm` with any tool verb | a verb creates no runtime, so nothing can be recorded |
| `--dump-wasm` with any other verb | two verbs at once, as for the existing five |
| `--dump-wasm` with `--bundle` or `--build-bundle` | it takes its own file, as `--dump-bytecode` does |
| `--record-wasm` with `--build-bundle` | the producer compiles and never runs, so it can compile no Wasm; the file would always be empty |
| `--dump-wasm` with `--inspect`/`--inspect-brk` | as for the existing five |
| empty `--record-wasm=`, `--bake-wasm=`, `--dump-wasm=` | names the flag rather than reporting a missing file with no filename in it |
| `--record-wasm` naming the same file as `--bundle`'s container or the script being run | see below |

**The same-file refusal is not hypothetical.** The recorder's write is a
rename over the destination, and a running container keeps the mapping it
already has, so `--record-wasm=app.hbb --bundle=app.hbb` would replace the
container with a recording while the run continued happily to completion.
`--extract-module --out` already guards exactly this shape, comparing
`st_dev`/`st_ino` through `isSameFile()` (`bundle_tools.cpp:402`) so a
symlink and a hard link count too; this reuses it.

`--record-wasm` is otherwise orthogonal: it is honoured for a plain script,
`-e` and a `--bundle=` run alike.

**A produced executable cannot record**, and gets no environment variable to
compensate. Every argument there belongs to the program -- which is what
keeps `process.argv.slice(2)` meaning what it means -- and `bundle_main.cpp`
parses no flags at all, so `--record-wasm=f` passed to an executable is an
application argument and nothing more. The workflow does not need it: the run
that feeds a bake is an ordinary script or `--bundle=` run, and the
executable is built afterwards from the already-baked container.

Failure to write the record file is an **error**, not a swallow. This is the
one place in this feature where that is worth stating, because the compile
cache next door swallows everything by contract. The difference is the one
that contract itself names: the cache has a right answer to fall back on, and
a recording that silently did not happen produces a container that silently
compiles at every launch. Nothing about `--record-wasm` is on a hot path or
in a program's way; it is a diagnostic the user asked for by name.

## Behaviour

| Situation | Result |
| --- | --- |
| Bundled run, module baked in | container hit, no compile, no disk write |
| Bundled run, module not baked | disk cache as today; saved to disk on compile |
| Executable, module baked in | container hit; nothing written to `~/.cache` for it |
| Any of the above with the cache disabled | unchanged: the hooks install regardless |
| Baked entry Hermes refuses | fatal: names the container and the digest, and terminates |
| Baked entry damaged but still loadable | undetected, exactly as for a module payload; it runs |
| Baked entry refused and the recompile fails | the `WebAssembly.Module` call fails and the program sees it; no `store`, so no termination |
| Baked entry refused but serializing produces nothing | falls back silently on the compiled module; no `store`, so no termination |
| Disk cache entry Hermes refuses | falls back to compiling, silently, as today |
| Record file from a different build | hard error at bake time naming both versions |
| Record file missing or unreadable | hard error at bake time naming the file |
| Record file structurally invalid | hard error at bake time: magic, version, offsets, alignment |
| Record file payload corrupt | undetected at bake time; the run terminates only if Hermes refuses the bytecode |
| Record file with zero entries | warning at bake time, build proceeds |
| `--record-wasm` with the cache off | works; every module with no baked entry compiles and is recorded |
| `--record-wasm` path unwritable | error before the program runs |
| `--record-wasm` run against a container Hermes refuses | the refused bytes are recorded, then the run terminates fatally; the record file must be overwritten by a working run before it is baked again |
| Program calls `process.exit()` mid-run | record file already written |
| Recording run that compiles no Wasm | well-formed empty record file |
| Container with no Wasm entries | runs as before; dump gains no WASM section, but reports v6 and a new size |

## Testing

Hits and misses are asserted from `HERMES_NODE_DEBUG_NATIVE=COMPILE_CACHE`
tracing, **never** from timing: the suite runs 16-way parallel and both of
its known flaky tests got that way through timing dependencies.

`test/lit.cfg` sets `HERMES_NODE_DISABLE_COMPILE_CACHE=1` for the whole
suite, so any case that needs a live disk cache opts in with its own
directory under `%t`, exactly as the existing compile-cache tests do. A case
that needs the cache *off* needs nothing, which is the suite's default.

- `test/test-wasm-record.js` (`REQUIRES: wasm`) -- record a run and assert
  `--dump-wasm` reports one entry; record again against a warm cache (its own
  cache directory) and assert the same entry, which is the regression test
  for recording lookups rather than only stores; record a program that
  instantiates no Wasm and assert a well-formed empty file; record to an
  unwritable path and assert the error precedes the program's output.
- `test/test-wasm-bake.js` (`REQUIRES: wasm`) -- record, bake, run bundled,
  assert `wasm container hit` with no following `wasm store`, and no disk
  entry written. A second case leaves one module unbaked and asserts it falls
  through to a normal compile while the baked one still hits. A third runs
  the bundled program with the cache disabled and asserts the container hit
  is still there, which is the regression test for the install rule.
- `test/test-wasm-bake-refused.js` (`REQUIRES: wasm`) -- overwrite a baked
  payload with bytes Hermes is certain to refuse (zeros, so the bytecode
  magic fails) while leaving every offset and length valid, and assert the
  process **exits non-zero** with a message naming the container and the
  digest. This is the only route to the ABI's refused-hit path, and it is
  what proves the artifact fails loudly rather than quietly recompiling.
  Patching the container is a few lines over `fs`.
  A second case does the same to a **disk cache** entry and asserts the
  opposite: the run succeeds and compiles, because the cache is best effort
  and the container is not.
- `test/test-wasm-record-exit.js` (`REQUIRES: wasm`) -- a program that
  instantiates a module and then calls `process.exit(0)`, asserting the
  record file is complete. Both exit paths call `_exit()`, so this is the
  case a write-at-the-end recorder would silently lose.
- `test/test-wasm-bake-errors.js` -- version mismatch, absent file, truncated
  file, zero-entry warning, the same-file refusal, and each
  flag-conflict row. Ungated, so it survives a checkout with no linker and no
  kit; the corrupt-file cases build their inputs by editing bytes of a
  checked-in-shaped file rather than needing a Wasm build.
- `test/test-wasm-bake-build-exe.js` (`REQUIRES: wasm, linker-available`) --
  the same through a produced executable, which is the case the feature
  exists for.
- `unittests/` -- record-file writer/reader round trip; truncated and
  misaligned files; replacement of a lookup-recorded entry by a later
  store; and v6 cases in `BundleFormatTest`, including that a container with
  no Wasm entries still validates and that a v5 container is refused by both
  open modes. These cover the recorder's own bookkeeping; the callback
  completion contract and the fatality rule are covered by the lit cases
  above, which are the only place a real `hermes_set_wasm_cache` round trip
  happens.

## Deliberately not in scope

- **Case 2 and case 3** (a standalone `.wasm` served from the container, and
  carrying only bytecode behind a token). See Scope above.
- **Detecting a baked entry that never hits.** `--dump` shows what a
  container carries and the debug trace shows what a run consulted; a
  build-time proof that the two will meet needs the codegen configuration,
  which is only available to a callback during a compile. The version check
  at bake time is the cheap approximation, and its limits are stated above.
- **Recording from a produced executable.** No flag surface exists there and
  none is added. See the flag surface section.
- **Detecting a damaged container.** Nothing here verifies a payload, and
  nothing here should: a checksum over one entry kind buys a partial
  guarantee at a cost paid on every launch. What this feature does do is
  refuse to continue when damage surfaces on its own -- see the fatality
  rule. Checksumming a whole container and refusing a bad one at `open()` is
  a coherent feature, additive to the format, and a different one from this.
- **Packaging `.wasm` data files.** Unchanged: they warn and are skipped.
- **Any Hermes change.** `hermes_set_wasm_cache` is already sufficient.
