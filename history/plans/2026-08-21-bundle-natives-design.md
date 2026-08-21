# Design: native addons in a bundle

**Status:** Draft for review, 2026-08-21.

Follows `history/plans/2026-08-19-closed-world-bundle-design.md`, which made
a bundle a closed world, and `history/plans/2026-08-20-bundle-preload-design.md`,
which made preloads a property of the artifact. This settles what a `.node`
addon means in a bundle.

## The problem

Today a native addon cannot be bundled at all. The producer warns and skips
every `.node` file (`lib/bundle/bundle_build.cpp:215`), and the run-time
loader carries a special case that rejects a `.node` request outright with
"Native addons are not supported in a bundle yet"
(`libjs/bundle-loader.js:123-129`). A program that loads one is simply
outside what `--build-bundle` can produce.

The obstacle is not our format. `dlopen(3)` and `LoadLibraryW` take a
**path**, not a buffer, and there is no portable way to load a shared object
out of memory. So the addon's bytes have to be a real file on a real
filesystem at the moment it is loaded, whatever else is true.

Every runtime that ships single-file executables hits this and answers it
differently:

- **Node.js** does not try. Its SEA can embed arbitrary assets
  (`sea.getAsset()`), but nothing extracts or `dlopen`s them, and the
  injected main script's `require()` "can only be used to load built-in
  modules" (`doc/api/single-executable-applications.md:448`). Addons ship
  next to the executable and the ordinary disk loader finds them.
- **Deno** embeds a VFS and, because a dynamic library cannot be loaded from
  it, unpacks native code to a temporary directory at run time; a
  `--self-extracting` mode instead extracts everything to a stable
  `<exe_dir>/.<exe_name>/<hash>/` on first run, explicitly "for scenarios
  where code needs real files on disk, such as native addons".
- **Bun** embeds `.node` files, copies each to a temporary file at load time
  and `dlopen`s that; since v1.2.13 it unlinks the temporary file
  immediately after loading, having previously left it for the OS to reap.

## The decision

**Native addons ship alongside the bundle, flat, in the same directory.**
They are recorded in the container but their bytes are not.

```
out/
  app.hbb
  hermes-parser.node
  node_sqlite3.node
```

Three reasons, in ascending order of weight.

**It is honest.** A program with a native addon is not one file and cannot
be made into one. Embedding buys the appearance of a single file by writing
an executable to disk at startup, and the appearance is the part that is not
true.

**Extraction cannot deliver the property it charges for.** The verify-then-
`dlopen` sequence has a TOCTOU gap that no amount of checksumming closes,
and the natural extraction target makes it easy to exploit: a per-user cache
directory is writable by every process running as that user, so an attacker
who can write there both plants the file and wins the race, and the checksum
attests to their bytes. A file sitting next to the binary lives under
permissions the deployer chose, which is the same trust model as any shipped
`.so` and is understood everywhere. It is worth noting that Bun's fix for
this shape was to unlink immediately after load -- which gives up precisely
the "do not write it every time" saving that motivates an extraction cache.

**Flat is what makes the artifact distributable.** A mirrored identity tree
was considered and rejected. It has real advantages: `bindings`-style
packages would find their addon by accident, because our bundled modules
already have `__dirname` and `module.paths` set from real paths under the
bundle root (`libjs/bundle-loader.js:444-493`), and an addon that reads a
sibling file would find it. But "bundle plus tree" is not a better
distribution unit than "a tree", and if a tree is being shipped anyway then
`--bundle` is only buying compile time, not a distribution story. The whole
point is a small, countable set of files.

Two consequences follow from flat, one bad and one good.

Discovery by accident is gone: nothing that computes a path finds an addon
that is no longer where the package thinks it is. That is the cost, and the
"What does not work" section below names who pays it.

`$ORIGIN`-relative loading gets *better*. Every native we place shares one
directory, so an addon that pulls a sibling shared library finds it there --
where a mirrored tree would have scattered them. We still cannot discover
those siblings, since nothing `require()`s them; copying them stays the
user's job and is documented, not automated.

## What is packaged

### Discovery: two routes, both existing

**Literal.** The scanner already produces a specifier for a literal
`require('./foo.node')` or `require.resolve('./foo.node')`; the producer
stops skipping it and records a native instead. This route carries more
weight than it first appears, because **napi-rs packages are literal**. Their
generated `index.cjs` is a platform/arch switch in which every branch is a
literal require -- either `require('./pkg.linux-x64-gnu.node')` or, as the
fallback, the scoped optional-dependency package
`require('@scope/pkg-linux-x64-gnu')`, which resolves through `main` to a
`.node`. The scanner follows all branches, so every platform variant becomes
a specifier; the ones that do not resolve on this host take the existing
"resolves to nothing, leave it to the run-time loader" path, which is exactly
what their try/catch is written to absorb. The one that resolves is
packaged.

**Declared.** `--include=<path>.node` covers the computed cases --
`bindings('x')`, `node-gyp-build(__dirname)`, `node-pre-gyp`. No new flag:
`--include` already means "package what the walk cannot see", and an addon
loaded by a computed path is exactly that.

A native is a **graph leaf**: no source to scan, no out-edges, no compile
step.

### Root participation

A native's identity is relative to the bundle root like any other, so it
participates in computing that root. Consequence worth stating because the
same lever bit us in the `package.json` filtering round (see
`history/plans/progress-aot-bundle.md`): `--include`ing an addon can widen
the root and therefore shift every other identity.

### Build output

`--build-bundle=out/app.hbb` writes `out/app.hbb` and copies each native
into `out/`, which is the bundle root, which is where the bundle must sit
anyway. Each copy is announced in the register `bundle root:` already uses:

**Corrected after implementing it (2026-08-21).** `out/` is the directory
the container is written to; it is not necessarily the bundle root, which
is computed as the common ancestor of the packaged files and can be
anywhere. The implementation deliberately does not assume the two coincide:
it copies each native into the *container's own directory*
(`sidecarDir = fs::path(outPath).parent_path()`), which is where the run
time looks for a sidecar (`<bundle dir>/<sidecar>`, realpath'd). That makes
a build into a directory of its own -- which is what
`examples/hermes-parser-ast/run.sh` does, and what
`test/bundle-natives.js`'s `%t.out` case pins -- produce a working
two-file artifact. Nor "which is where the bundle must sit anyway": a
container is internally consistent under any consumer root, because the
consumer re-roots every identity at the bundle file's directory rather than
reading the producer's root out of the container. Sitting at the printed
root only matters for filesystem access through a bundled module's
`__dirname`/`__filename` -- unpackaged data files, and the
stat-before-require escape hatch below.

```
bundle root: /home/me/proj
native: node_sqlite3.node  (from node_modules/sqlite3/build/Release/node_sqlite3.node)
note: this bundle requires 1 native addon alongside it; ship them together.
```

The closing note is deliberate. Packaging a native changes the distribution
contract from "one file" to "one file plus these", and a build that changed
that contract silently would be the same class of failure this subsystem
exists to remove -- found on the deployment machine instead of at build time.

The producer copies rather than printing a list to copy, because the
alternative is the status quo: a build that succeeds and produces an
artifact that throws at run time.

**Naming.** The sidecar name is the basename. When two identities collide on
it, a short hash of the identity is appended before the extension
(`node_sqlite3-3f2a9c11.node`). The identity-to-sidecar map is **recorded in
the container** and never re-derived at run time, so the disambiguation rule
can change later without invalidating existing bundles.

**Overwrite.** Copying overwrites a file of that name in the output
directory. One refusal, reusing the check `--extract-module --out` already
performs: the destination must not be the bundle itself, compared by
`st_dev`/`st_ino`.

## Container format: v4

The record for a native carries **no payload**. Putting the bytes in the
container would be extraction with extra steps.

- New `ModuleKind::kNative` (`include/hermes/node-compat/bundle/bundle_format.h:28`).
  Kind is already stored in the record and never re-derived from the
  identity's extension, so this slots in. The identity is unchanged -- the
  path relative to the bundle root -- so edges, `require.cache` keys and
  error messages keep their shape.
- Payload offset and length are zero. `__bundleLoad` refuses a `kNative`
  record the way it already refuses a record packaged only for the resolver:
  a native is loaded by `dlopen` and never by the JS loader, and the two
  paths must not be able to blunder into each other.
- **The per-native facts live in a section of their own**, not in the module
  record: a header `nativeTableOffset`/`nativeCount` pair and a
  `BundleNativeRecord {moduleIndex, sidecarString, byteLength, hashString}`,
  sorted by `moduleIndex` and binary-searched. This is the preload table's
  precedent rather than the flag-bit one, and for the same reason: three
  more `uint32_t` on every module record would be paid by the ~1500 modules
  of a real bundle that are not natives, to describe the one or two that
  are. A container with no natives pays nothing.
- The native record stores the addon's **byte length and SHA-256, recorded
  at build time and never read on the run path.** Verifying at load would mean
  reading the whole file on every launch, in an artifact whose reason for
  existing is startup cost -- the same objection that moved the `BUNDLE`
  debug gate off the require path. Recording is free at build time and pays
  for itself in `--dump` and in `--verify-natives`.
- `BundleFileSource` indexes natives like any other identity, so
  `require.resolve` answers consistently with what `require` will do. That
  agreement is the invariant the two-backend resolver design exists to
  protect.

Format version goes to **v4**: fatal on mismatch in `open()`, reported in
`openForInspection()`, as v2 and v3 established.

## Run time

**Resolution order is unchanged.** A native is an ordinary identity, present
in the edge table and in `BundleFileSource`, so `require` and
`require.resolve` reach it through the path they already take: builtin, edge
table, `bundle.resolve`, throw. The only new thing is what happens once the
record is in hand -- kind `kNative` branches to `dlopen` instead of to the
CommonJS wrapper.

**One resolution gap has to be closed explicitly.** The addon's own loader
is very often reached by an *absolute* request --
`require(path.join(__dirname, 'prebuilds', target, 'x.node'))` is the shape
`hermes-parser`, `bindings` and friends all produce. Absolute requests
happen to resolve correctly today, on both backends: `resolveSpecifier`
(`lib/bundle/bundle_resolve.cpp:328`) classifies a specifier as relative or
bare and nothing else, so an absolute one takes the bare branch, and the
first `node_modules` iteration calls
`joinNormalized(dir / "node_modules", specifier)` -- where
`fs::path::operator/` **discards the left operand because the right one is
absolute**, yielding the absolute path itself, which `resolveBase` then
probes directly. `DiskFileSource` finds the file; `BundleFileSource` strips
the root and finds the identity.

That is the right answer, reached by accident, unremarked and untested --
and this design puts it on the load path of its flagship example. So it
becomes an explicit third branch in `resolveSpecifier`, commented as Node's
own behavior -- `Module._findPath` overrides the lookup paths to `['']` for
an absolute request (loader.js:699-701), discarding whatever
`_resolveLookupPaths` returned, which is the same shape as the C++ accident
-- and pinned by an
agreement case in `BundleResolveTest` plus a `BundleFileSourceTest` case.
No behavior change; a behavior that currently survives only as long as
nobody rewrites `joinNormalized`.

Note what this does *not* rescue. A computed absolute require produces no
edge, so it can only ever be answered by `bundle.resolve` -- which means
`--include` packages the addon but the run-time resolver is what has to find
it. That is precisely why the branch has to be dependable rather than
incidental.

**The `Module` looks like every other bundled module.** `module.filename` is
`path.join(root, identity)` -- the path the package believes it lives at --
and `module.paths`, `__dirname` and the `require.cache` key follow from it as
they do today. This is the *consistent* choice rather than the deceptive
one: every bundled module already has a filename that no file sits at,
because the JavaScript is not on disk either. The real sidecar path is
passed as `process.dlopen`'s second argument and appears in dlopen error
messages, which is where it is actionable.

**The sidecar path is `path.join(root, sidecarName)`**, root being the bundle
file's own directory, which the consumer already computes. One directory
holds the bundle and all of its natives.

**Caching and publication order are unchanged.** `Module._cache[filename]` is
written before the load and deleted if it throws, exactly as the JavaScript
path does, so `delete require.cache[require.resolve(x)]` keeps working.

**The load is `process.dlopen(mod, sidecarPath)`**, no flags, matching Node's
own `.node` extension handler (`lib/internal/modules/cjs/loader.js:1920` in
`/home/tmikov/3rd/node`). Our implementation uses `RTLD_NOW | RTLD_LOCAL`
(`lib/process/node_process.cpp:691`) and is not changed.

**One special case is deleted.** The `.node` rejection at
`libjs/bundle-loader.js:123-129` goes away. A `.node` request that is not a
recorded identity falls into the *general* miss path and receives the
`--include` suggestion `includeSuggestion()` already builds. One fewer
branch, and the error now names the fix.

**Cost when a bundle has no natives is zero.** No stat, no hash, no work at
startup. Only a `require` that lands on a `kNative` record touches any of
this.

Two judgment calls, recorded because they could each have gone the other
way:

**A missing sidecar throws `MODULE_NOT_FOUND`**, with a message saying the
addon is recorded in the bundle but its file is not beside it, naming both
the expected filename and the identity. `ERR_DLOPEN_FAILED` is arguably more
precise -- the module was found, its file was not -- but the surrounding
code that exists in the world (napi-rs try/catch chains,
optional-dependency probes) is written to absorb `MODULE_NOT_FOUND`, and the
practical meaning is identical: this addon is unavailable. Precision that
breaks a fallback is not worth having.

This is continuity rather than a new choice: `notInBundle()`
(`libjs/bundle-loader.js`) already gives every miss `MODULE_NOT_FOUND`,
"including the addon one", for exactly this reason -- only the
human-readable text differs. The text is what changes here; the code is
what it always was.

**`--preload=<x>.node` is allowed and needs no code.** The preload table
holds module indices and running one means requiring it; requiring a native
means `dlopen`. It falls out of the existing mechanism, and writing a
refusal for something harmless that already works would be worse than
leaving it.

**A boundary restated, not moved.** A program that calls
`process.dlopen('/tmp/evil.so')` itself is outside the closed world, exactly
as code that drops to `Module.prototype.load` or `require.extensions`
already is. Adding a sanctioned route for natives makes that line clearer
rather than blurrier, but the documentation must say so, because "we now
load native code" invites the question.

## Flag surface: `--verify-natives`

A fourth read-only verb, alongside `--dump`, `--extract-module` and
`--dump-bytecode`.

```
$ hermes-node --bundle=app.hbb --verify-natives
OK       node_sqlite3.node        (node_modules/sqlite3/build/Release/node_sqlite3.node)
MISMATCH libfoo_binding.node      (node_modules/foo/build/Release/libfoo_binding.node)
MISSING  bar.node                 (node_modules/bar/bar.node)
error: 2 of 3 native addons failed verification
```

For each `kNative` record it locates `<bundle dir>/<sidecar>`, stats it,
hashes it and compares against the recorded length and SHA-256. A non-zero
exit when any entry is not `OK` is what makes it usable as a deployment gate
rather than merely something to read. `--verbose` adds expected and actual
hex and sizes, joining `--dump`, `--build-bundle` and `--dump-bytecode` as a
legitimate `--verbose` consumer, so that row of the conflict matrix widens
rather than a new mechanism appearing.

**SHA-256, not CRC32.** CRC32 is trivially forgeable -- a file can be
constructed to carry any CRC -- so presenting a CRC check as a security step
would be an overclaim of exactly the kind the last documentation round
removed. CRC32 keeps its existing jobs (the generation tag, compile-cache
corruption detection). `external/picohash` is already vendored, public
domain, header-only, and already linked by `lib/bindings`, so
`hermesNodeBundleTools` can link it and stay free of the Hermes VM -- which
is what keeps `BundleToolsTest` runnable with no runtime.

**This is an audit, not an enforcement.** It verifies the file at the moment
the verb runs, not at the moment the program `dlopen`s it, and a TOCTOU gap
sits between them that no hashing closes. The help text and the
documentation must say so. The precedent is `openForInspection`, which
reports the generation tag rather than enforcing it, for the same reason.

**Placement** follows the established rules exactly: dispatched from
`runToolVerb()` before `runHermesNode`, so no runtime, event loop or
`napi_env` exists while it runs; opened through `openForInspection`, not
`open`; implemented in `lib/bundle/bundle_tools.cpp`. `checkToolOptions()`
gains the rows that follow mechanically -- it is a verb (not with another
verb, not with `--inspect`/`--inspect-brk`) and a container verb (requires
`--bundle`) -- all of it after the parse loop, so flag order stays
irrelevant.

## Diagnostics

**`--dump` gains a `NATIVES` section**, structured like `PRELOADS`: identity,
sidecar name, byte length and a short SHA-256 prefix, with the full hash
under `--verbose` alongside the in and out edge counts modules already get.
In-edges are the useful column here, because "who requires this addon" is
the question one has when it is missing.

**`--build-bundle --verbose`** names the specifier that pulled each addon in
during discovery, adds source path, destination, length and SHA-256 to each
copy line, states a basename disambiguation explicitly rather than leaving a
hash in a filename to be noticed, and adds one summary line -- count and
total bytes of natives -- kept distinct from the container's own byte
totals, because those bytes are not in the file.

**`--extract-module` on a native is an error**, and an informative one:
there is no payload to extract, and the message says the file ships
alongside as `<sidecar>` rather than reporting an empty write or an unknown
identity.

**`HERMES_NODE_DEBUG_NATIVE=BUNDLE`** already names the outcome of every
bundled require -- edge hit, container resolve, miss. A native gets its own
outcome name, so the log distinguishes "resolved to an addon and dlopen'd
it" from an ordinary hit.

## What does not work

**Loaders that stat before they require.** `node-gyp-build` and
`node-pre-gyp` do not try-and-catch; they `existsSync` or `readdirSync` a
candidate directory and only then require the winner. With a flat sidecar
that check has nothing to find at the path the package believes in.

We already own an instance. `examples/bufferutil-addon` uses
`require('node-gyp-build')(__dirname)` inside a try/catch with a pure
JavaScript `./fallback`, and in a bundle it **silently falls back to
JavaScript** -- correct results, slower.

**Corrected after measuring it (2026-08-21).** The paragraph above
originally continued "the `require` interception never gets a turn", and
said node-gyp-build throws. Run, neither is reliably true. Which half fails
depends on whether the source tree is still on disk beside the bundle:

- Tree present (the ordinary development shape): `readdirSync` reaches the
  real disk, finds the real prebuild, and `load.resolve()` returns a real
  absolute path without throwing. The **`require()` of that path** is what
  the closed world refuses, so the interception gets exactly one turn and
  takes it. `mask.js`'s own `require('node-gyp-build').path(...)` check
  then passes and prints the native addon's path while the loaded module is
  the fallback -- a `require.resolve`-shaped probe cannot tell which one it
  got.
- Tree absent: `readdirSync` finds nothing and node-gyp-build throws its
  own "No native build was found", as originally described.

Either way the same catch fires and the same fallback loads, so the
conclusion -- silent fallback -- stands; the mechanism does not. The full
transcript is in `history/plans/progress-aot-bundle.md` under
"2026-08-21: native addons in a bundle (format v4)".

**napi-rs's local-file branch, in one marginal shape.** Its generated code
guards the local branch with
`existsSync(join(__dirname, 'pkg.linux-x64-gnu.node'))`, which is false under
a flat sidecar, so it takes the else branch and requires the scoped package.
Normally that is fine, because the scoped package is what npm installed and
what resolved at build time anyway. It fails when the addon was built
in-place *and* no scoped package is installed: the file we packaged is the
one whose branch is now unreachable.

**The escape hatch, which costs no code.** Anyone hitting either case can
*additionally* place the real file at its identity path under the bundle
root. Their stat then succeeds, and whether we intercept the following
require or let it load from disk, the bytes are the same. Flat is the
contract; the tree remains available to whoever needs it.

**Teaching `fs` to answer** for recorded native identities was considered and
deferred. It is small, but it is a half-truth discoverable in one line of
user code -- `existsSync` says yes while `readFileSync` on the same path says
no -- and it blurs the "closed for modules, open for data" line that the
`examples/babel-parser/ast.js` round established. It is also purely
additive: adding it later makes `fs` answer for more paths, breaks nothing
that worked, and needs no format change and no re-ship.

## Testing

The fixture question is already answered. `CMakeLists.txt:86-97` builds
`hello_addon` as a shared library with a `.node` suffix, `test/lit.cfg:41`
substitutes `%hello_addon`, and two existing tests show both shapes:
`test/test-native-addon.js` (direct path) and `test/test-native-addon-pkg.js`
(a package whose `main` is a `.node`, assembled in `%t` with
`cp %hello_addon`). No new build infrastructure. The package shape is the
more valuable one to mirror, since a bare specifier resolving through `main`
to a `.node` also exercises the v2 "package.json kept for the resolver"
machinery against a native.

New lit tests:

- `test/bundle-natives.js` -- build with an addon, assert the sidecar lands
  beside the bundle, run it, assert the addon's export works. Both the
  literal-require and the `--include` route.
- `test/bundle-natives-errors.js` -- sidecar deleted (message and
  `MODULE_NOT_FOUND` code); an unrecorded `.node` request (the `--include`
  suggestion, replacing the deleted "not supported yet" case);
  `--extract-module` on a native.
- `test/bundle-verify-natives.js` -- `OK`, `MISSING`, and `MISMATCH` with a
  flipped byte, plus the non-zero exit.

Extended: `bundle-dump.js` (NATIVES section), `bundle-verbose.js` (copy
lines, disambiguation, summary), `bundle-tool-errors.js` (the new matrix
rows), `bundle-tool-no-runtime.js` (`--verify-natives` creates no
compile-cache directory).

GTest: `BundleFormatTest` for a v4 round-trip of a `kNative` record and the
v3-to-v4 mismatch; `BundleToolsTest` for verification against a temporary
file with no runtime; `BundleFileSourceTest` for a native being indexed and
answering a resolve; `RequireScannerTest` for a literal `require('./x.node')`
producing a specifier; and `BundleResolveTest` for the explicit
absolute-specifier branch, as an agreement case so the two backends are
pinned against each other on it.

## Example: `examples/hermes-parser-ast/`

Paired deliberately with `examples/babel-parser/ast.js` -- the same job, two
parsers. One bundles to a single file; the other bundles to a file plus a
shared object. That contrast is the documentation for this feature, and
`ast.js` already established the tool-not-demo shape, positional argument
and all.

It is also the right example because `HermesParserAddon.js`
(`external/hermes-parser-native/package/dist/`) is not a toy:

```js
if (override) return require(path.resolve(override));
const devBuildPath = path.join(__dirname, '..','..','..','..','..','cmake-build-debug', ...);
const prebuiltPath = path.join(__dirname, '..', 'prebuilds', target, 'hermes-parser.node');
for (const candidate of [devBuildPath, prebuiltPath]) {
  try { return require(candidate); } catch (e) {}
}
```

Every require is computed. In one real file that exercises:

1. **Computed require, so no edge.** The producer's existing "cannot follow"
   warning fires and `--verbose` names the position. The addon is invisible
   to static discovery, so this is the `--include` case occurring naturally
   rather than being staged.
2. **An absolute-path request resolved against the container.** At run time
   the request is an absolute path under the bundle root, which the loader
   must map back to an identity. That is the same mechanism that makes
   `bindings` work, and this example is why it is built rather than
   speculated about.
3. **try/catch over candidates.** `devBuildPath` is not in the bundle, so it
   misses, throws `MODULE_NOT_FOUND`, is caught, and `prebuiltPath`
   succeeds -- the existing tolerant behavior, now over a native.
4. **A documented escape that must fail.** `HERMES_PARSER_NATIVE_ADDON`
   names an absolute path meaningful only on the build machine, and a
   closed world serves only what the container records -- so the bundled
   run refuses it whatever the path is. (Not "outside the bundle root": the
   root is derived from what is packaged, not a fence around it, and
   `--include=/abs/path/outside.node` is packaged and widens the root.) The
   same program, configured two ways, one of which the bundle declines.
5. **The flat sidecar in its real shape**: `app.hbb` plus
   `hermes-parser.node`.

One wrinkle handled rather than hidden. The committed prebuilt is
`prebuilds/linux-x64/` only, and `examples/flow-bundler/run.sh` sidesteps it
by exporting `HERMES_PARSER_NATIVE_ADDON` -- which the bundled run cannot
use. So this example's `run.sh` copies the freshly built
`$BUILD_DIR/external/hermes-parser-native/hermes-parser.node` into
`node_modules/hermes-parser/prebuilds/<platform>-<arch>/` before building.
`prebuiltPath` then resolves under the bundle root, is `--include`-able, and
is correct for whatever machine runs it, with no dependence on the committed
linux-x64 blob. That copy belongs inside the same
`BEGIN/END vendored native parser addon` markers `flow-bundler/run.sh` uses,
because the deletion recipe in `external/hermes-parser-native/README.md`
refers to them.

Both modes run and must produce the same AST, held to
`examples/babel-parser/run.sh`'s standard: assert node types drawn from the
input, and assert that a missing argument exits non-zero.

Like every other example, it needs a network `npm install` and so belongs to
`check-hermes-node-examples`, not `check-hermes-node`.

## Not doing

- **Embedding the addon and extracting it at run time.** The whole argument
  above.
- **Mirroring the identity tree** next to the bundle. Rejected for
  distributability, with the auto-discovery benefit knowingly given up.
- **Virtualizing `fs`** so path-computing loaders find addons that are not
  there. Deferred, additive, and a half-truth.
- **Verifying hashes on the run path.** Recorded, checkable on demand, never
  read at startup.
- **Discovering an addon's own sibling shared libraries.** Nothing
  `require()`s them; the flat layout does not fight them, and copying them
  is the user's job.
