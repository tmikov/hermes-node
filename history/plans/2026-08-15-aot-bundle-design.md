# Design: AOT Bundle Mode

**Status:** Design approved 2026-08-15. Implementation plan to follow.

## Goal

Package an application's compiled JavaScript into one distributable file, so
it can be shipped and run on another machine with no `node_modules` tree and
no compilation at startup.

A later, separate project fuses that file with the `hermes-node` binary to
produce a single executable. This design does not implement that, but the
consumer path is arranged so that step becomes a change of *source* rather
than a change of *mode*.

## Context

The compile cache (see `2026-08-12-bytecode-compile-cache-design.md`) already
removes compilation from the second and later runs on one machine. It does
not help distribution: the cache is keyed on absolute paths, lives in
`~/.cache`, and is useless on a machine that does not have the same tree at
the same location.

Two facts from the existing implementation shape everything below.

**Hermes bytecode is complete.** It executes with no source present and
carries its own debug info for stack traces. V8's code cache is not like
this -- it is supplementary metadata that still requires the original source,
which is why Node's SEA has to embed the script text alongside it. For us,
"a bundle of bytecode" and "a self-contained application" are nearly the same
artifact.

**The compile cache cannot be the input.** It stores `path -> bytecode` and
nothing else. It has never recorded *why* a file was compiled or which
specifier led to it. The bundle needs that graph (see "Container format"
below), so the producer must do its own discovery pass. The cache is at most
an optimization inside the producer, never its source.

### Prior art

Node's SEA performs no discovery at all: `--experimental-sea-config` takes one
already-bundled script plus an explicit `assets` key-path map. It enforces the
boundary rather than computing it -- `require()` in an injected main script can
load built-in modules only, and throws for anything on the filesystem. Node
punts the graph problem to esbuild or rollup.

Bun's `bun build --compile` statically analyses the import graph, because Bun
already contains a bundler. What it cannot resolve statically it does not
package; a dynamic relative path falls back to reading from the working
directory at runtime, and `.node` addons must be required directly or embedded
by explicit import attribute.

We take Bun's approach (derive the set by analysis) without needing Bun's
machinery, because we only need to *enumerate* files, not *bundle* them: no
scope hoisting, no import rewriting, no tree shaking, no interop shims. Parse,
find literal `require()` arguments, resolve them with the resolver we already
have, recurse.

### Rejected: Hermes CommonJS module mode

Hermes has a `-commonjs` compiler mode with `-fstatic-require`, a bytecode CJS
module table (`BytecodeFileFormat.h:530`), and a matching `RequireContext` in
the VM. It compiles several CommonJS files into a single `.hbc` and resolves
`require` inside the runtime, which is close to what this design builds.

It is deprecated and will not be used. Recorded here so the question is not
re-opened.

## Scope

In scope for v1:

- JavaScript, compiled to optimized bytecode.
- TypeScript, on the same terms. `.ts` is a headline feature of the runtime,
  and it costs nothing here: discovery parses with TS enabled, compilation
  passes `enableTS` exactly as the loader does today, and the identity keeps
  its `.ts` extension. Type stripping happens at parse time, so the packaged
  bytecode is indistinguishable from JavaScript's.
- JSON, stored as text.
- Static discovery from an entry script.
- A producer and a consumer, both modes of the existing `hermes-node` binary.

Out of scope for v1, in each case with a defined behavior rather than a gap:

- **Assets other than JS and JSON.** They stay on disk and are read normally.
- **Native addons.** A `.node` file must be a real file for `dlopen`; it can
  never live inside the bundle.
- **Trace input.** Static analysis misses `require(someVariable)`. The runtime
  fallback below makes that a visible slowdown rather than a failure, and the
  log it emits is exactly what a future trace producer would need to record.
- **Multiple entry points.** One entry per bundle.
- **Fusing into the executable.** A separate project.

Built-in modules are unaffected. They are already bytecode inside the binary
and are never packaged.

## Architecture

One new library, `lib/bundle/`, with public headers under
`include/hermes/node-compat/bundle/`, following the layout convention used by
`lib/compile-cache/`.

**Producer.** `hermes-node --build-bundle=app.hbb ./cli.js`

Parses from the entry, follows literal `require()` calls, resolves each
through the existing resolver, recurses to fixpoint. Compiles every JavaScript
file to optimized bytecode, reads every JSON file as text, writes one
container. It never executes the application.

**Consumer.** `hermes-node --bundle=app.hbb [-- args]`

Maps the container, validates it, installs a resolution table into the module
loader, runs the recorded entry module.

The two share only the container format. The producer does not link the
consumer's loader integration, and the consumer does not link the parser.

## Container format

Designed to be used in place, from an `mmap`, with no load-time pass over the
file.

```
header:   magic "HNBUNDLE" (8 bytes)
          format version   u32
          generation tag   u32
          entry module idx u32
          table offsets and counts
strings:  deduplicated UTF-8 bytes, referenced by index
modules:  [ identity: string idx | kind: JS | JSON | payload off | payload len ]
edges:    [ importer: module idx | specifier: string idx | target: module idx ]
payload:  concatenated bytecode and JSON text, each aligned as Hermes requires
```

Records in the module and edge tables are fixed-width PODs. That is the reason
the string table exists: with strings held out of line, the edge table is a
sorted array that can be binary-searched directly in the mapped file, with no
parsing, no pointer patching, and no allocation at load. Variable-length inline
strings would force a pass over the whole table at startup to find record
boundaries, which is the cost this format exists to avoid.

Deduplication is a secondary benefit and applies mostly to specifiers.
Measured over `examples/flow-bundler`'s `node_modules`: 5839 literal
`require()` arguments, 1468 distinct, about 4x. Module identities are unique
by construction and do not deduplicate.

### Edge lookup

Edges are sorted by **(importer index, specifier bytes)** and binary-searched.
A lookup compares the importer index first, then `memcmp`s the specifier
through its string-table index.

Sorting on the specifier's *bytes* rather than its string index is deliberate.
At runtime `require("foo")` supplies bytes, not an index; sorting by index
would require a string-to-index hash built at load time, reintroducing the
startup pass. Comparing bytes costs about twelve short `memcmp`s per `require`
and keeps load-time work at zero. The string index remains in the record as the
way to reach the bytes, not as the sort key.

Built-in modules keep their current precedence: the loader resolves `fs`,
`path` and the rest against the embedded builtins before the edge table is
consulted, so a bundle cannot shadow them. Only specifiers that reach disk
resolution today reach the edge table.

Because `(importer, specifier) -> target` is precomputed, runtime `require` is
a binary search and not a `node_modules` walk. A consequence worth stating:
`package.json` files are not needed at runtime for their metadata role at all.
One is packaged only if application code requires it explicitly, in which case
it is an ordinary JSON edge like any other.

### Generation tag

Computed from the same inputs as the compile cache's generation name:
`hermes-node` version, architecture, Hermes `BYTECODE_VERSION`, and the
optimize flag.

The *policy* differs from the cache. A cache generation mismatch silently
starts a new generation; a **bundle mismatch is a hard error**. A bundle is a
deliverable, and silently ignoring it to recompile from a tree that may not
exist is worse than refusing to start.

## Identity and the root

Module identities are paths relative to a **build root**, computed after
discovery as the common ancestor of every packaged file. This is deterministic,
needs no heuristics, and guarantees no identity requires `..`. The producer
prints the root it chose, so the artifact's expectations are not a mystery.

At runtime the root is the directory containing the bundle file. `__filename`
is root joined with the identity; `__dirname` follows from it. These are real
paths that exist when the tree is present, which is what makes v1's "everything
else stays on disk" escape hatch function rather than merely be declared.

`process.argv[1]` is the bundle path as given on the command line.

A virtual-filesystem prefix (Bun's `/$bunfs/`) was considered and rejected.
Both schemes store identical, machine-independent bytes, so it offers no
reproducibility advantage; it differs only in making `__dirname` point
somewhere that does not exist, which disables the escape hatch v1 depends on.
Surveying our example trees, 55 of ~5400 JavaScript files mention `__dirname`
and exactly one uses it to reach the filesystem -- `hermes-parser`, locating
its native addon, which is precisely the case that can never be packaged. The
schemes are also not symmetric: a `--hermetic` flag could later swap the root
for a synthetic prefix, but going the other way would redefine module identity
after code depends on it.

## Error policy

| Situation | Behavior |
| --- | --- |
| Literal specifier will not resolve (build) | Hard error naming importer and specifier -- except a vendored package name (`ws`), which warns and is left to the copy embedded in the binary |
| Resolves to `.node` or other non-JS/JSON (build) | Warn, skip, leave to runtime fallback |
| `require` misses the edge table (runtime) | Fall back to disk resolution and compilation; log under `HERMES_NODE_DEBUG_NATIVE=BUNDLE` |
| Generation or format version mismatch | Hard error, no fallback |
| Truncated or corrupt container | Hard error, from the structural checks |

"Corrupt" in that last row means what `BundleReader::open` can see: magic,
version, generation, and every offset, length, alignment and index. The
payload bytes themselves are not covered -- there is no checksum over a
module's bytecode or JSON text, so a bit flipped inside one is not an error
but whatever Hermes makes of it. That is a deliberate asymmetry with the
compile cache, which stores a CRC32 per entry because its entries can go
stale against a source file that changed underneath them; a bundle has no
such second copy to disagree with. A payload checksum is an additive change
to the format if this ever needs revisiting.

The runtime fallback is what converts "static analysis missed something" from a
crash into a visible performance regression. Its log is, in effect, a
hand-rolled trace naming exactly the modules a future trace producer would need
to capture.

## Interactions with existing features

**`--inspect` / `--inspect-brk` are rejected with `--bundle`**, matching the
existing rule for `--optimize=on` and for the same reason: bundle bytecode is
compiled at `DebugInfoSetting::THROWING` and the debugger requires `ALL`. The
error message directs the user to run from source.

**`--optimize` does not apply to the producer**, which always optimizes -- that
is what AOT means here. On the consumer side the flag affects only fallback
compiles.

**The compile cache is not consulted for bundle hits** and not written for
them; there is nothing to compile. Fallback compiles use it normally.

## Testing

Unit tests (GTest, `unittests/`) for the container: write/read round trip,
alignment, truncation at each table boundary, bad magic, format version
mismatch, generation mismatch, and edge lookup including the miss case.

Lit tests (`test/`) for behavior:

- Build a bundle from a fixture tree and run it.
- **Delete the tree and run it again**, proving self-sufficiency. This is the
  test that actually demonstrates the feature.
- Corrupt the container and check for a clean error rather than a crash.
- Force a fallback (a module reached only by dynamic `require`) and check that
  it runs and that the debug log names it.
- A JSON `require` round trip.

End to end, `examples/yargs-cli` (50 files) builds and runs as a bundle
(`test/bundle-yargs.js`, gated on the `examples-installed` lit feature so the
offline default suite reports it UNSUPPORTED rather than failing).

**Corrected after implementation.** This section originally named
`examples/flow-bundler` as "the natural regression test for the fallback
path", on the grounds that its `hermes-parser` dependency loads a native
addon. No such test was written, and it is not achievable as described: the
bundler's own sources are Flow + ESM compiled on the fly by `@babel/register`,
so the producer stops at the entry with `error: failed to parse
.../buildBundleCLI.js: 11:12: 'from' expected` before any dependency is
reached. The fallback path is covered by synthetic fixtures instead --
`test/bundle-fallback.js` (computed `require`, debug log, and the shared
`Module._cache`) and the COLLIDE / BARE / THROW cases in
`test/bundle-run.js` -- which pin the same behaviors with no `npm install`
and no native addon.

## Risks

**Static discovery is incomplete by construction.** Any `require()` whose
argument is not a literal is invisible to it. Mitigated by the runtime
fallback, not solved by it; solving it needs the trace producer, which is out
of scope for v1.

**The common-ancestor root can be surprising** for a tree whose entry sits deep
inside it, since the bundle must then be placed at that ancestor to rehydrate
correctly. Mitigated by printing the root at build time. If it proves annoying
in practice, a `--bundle-root=<dir>` override is an additive change.

**Verified, not a risk:** the discovery walk uses `hermes::parser::JSParser`
(`hermes/include/hermes/Parser/JSParser.h:39`), whose `parse()` returns an
ESTree `ProgramNode*`, walked with `visitESTreeNode` / `visitESTreeChildren`
from `hermes/include/hermes/AST/RecursiveVisitor.h`. We already link the VM, so
no new dependency and no submodule change.
