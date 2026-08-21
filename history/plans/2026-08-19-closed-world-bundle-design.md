# Design: Closed-world bundles

**Status:** Draft for review, 2026-08-19.

## The property

A bundle is a closed world. Every module the program loads comes from the
container, and the loader never touches the filesystem to resolve or read
code. A `require()` the container cannot answer is an error, not a disk
read.

Today it is the opposite: a miss falls through to Node's real loader,
which resolves and compiles from disk. That fallback was scaffolding for
the first version, and it costs two things.

It makes "self-contained" unverifiable in practice. A bundle that still
needs `node_modules` behaves identically to one that does not, right up
until the tree is gone -- which is the situation a bundle exists for. That
is how `examples/babel-parser/transform.js` shipped a container that looked
complete and was not (see `progress-aot-bundle.md`, "a bundle is not
self-contained when specifiers are computed").

And it makes the bundle's boundary meaningless. A computed specifier can
name any file on the machine, and the loader will compile and run it. For
a deliverable meant to be shipped and run somewhere else, code loading has
to be bounded by the artifact.

So: the fallback goes away, `--include` is added to put in what static
discovery cannot see, and the container answers resolution itself.

## Failure is reported at run time, not build time

The producer keeps the tolerance added on 2026-08-16: an unresolvable
specifier, a file that will not compile, and a computed `require()` are all
warnings, and the build succeeds. Most of those code paths are never
reached -- an optional-dependency probe, a `.cts` branch, a loader for
another module system -- and refusing to build over them is what made
`transform.js` unbundlable in the first place.

The bundle fails only when the program actually asks:

```
Error: Cannot find module '@babel/preset-env'
  required by node_modules/@babel/core/lib/config/files/plugins.js
  Not in the bundle. Add it with:
    --include=@babel/preset-env
```

## Flag surface

`--include=<specifier>`, repeatable. Each value is either a bare specifier
(`@babel/preset-env`) or a path (`./lib/plugins`, `/abs/path`), resolved
from the **entry's directory**, and then walked exactly like the entry:
same discovery, same classification, same compilation. An `--include` that
does not resolve is a build error, unlike a require() that does not
resolve -- the user named this one explicitly, so silence would be wrong.

Nothing else about the producer changes.

## The resolver: one algorithm, two backends

`resolveSpecifier` in `lib/bundle/bundle_resolve.cpp` already implements
the subset of Node resolution this project supports. It touches the
filesystem in exactly three places: `isRegularFile`, `isDirectory`, and
`readPackageMain` (which reads `<dir>/package.json`).

Those three become an interface:

```cpp
/// Everything resolution needs to know about a file tree. Two
/// implementations: the real filesystem, for the producer, and the
/// container's identity set, for the consumer.
class FileSource {
 public:
  virtual ~FileSource() = default;
  virtual bool isRegularFile(const std::string &path) const = 0;
  virtual bool isDirectory(const std::string &path) const = 0;
  /// The text of <dir>/package.json, or nullopt when there is none.
  virtual std::optional<std::string> readPackageJson(
      const std::string &dir) = 0;
};
```

`DiskFileSource` is today's code, used by the producer.
`BundleFileSource` answers from the container's identities, used by the
consumer. `resolveSpecifier` gains a `FileSource &` parameter and is
otherwise unchanged.

This is the whole point of the design. A second implementation of
"resolution" written in JavaScript against the identity list would
eventually disagree with the C++ one, and a specifier that resolves one way
at build time and another at run time is the worst failure this system can
produce -- it is invisible until a program silently loads the wrong module.
Sharing the algorithm makes the disagreement impossible rather than
unlikely.

It also settles a question that would otherwise need its own bookkeeping:
which `package.json` files must be packaged (below). `DiskFileSource`
records every one it successfully reads, so the producer learns the exact
set as a side effect of resolving.

### Directories from a flat identity list

Identities are relative paths; the container has no directory records.
`BundleFileSource` answers `isDirectory(d)` as "some identity begins with
`d/`", over the identity list held sorted, so it is a binary search and a
prefix compare. `isRegularFile(p)` is an exact-match lookup in the same
list. Both are pure string work over data already in the container.

## Format change: resolution inputs

A container today holds **no** `package.json` records at all -- verified by
dumping the yargs example, which has zero. The producer reads them from
disk while resolving and never packages them, because nothing `require()`s
them. The consumer therefore cannot answer `main` for any package, and
without that it cannot resolve a bare specifier at all.

So every `package.json` the producer consulted is packaged. Not as a new
module kind, though: the program may *also* require one, and a container
holds one record per identity, so a second record for the same file is not
representable. It is a flag on the existing record instead:

```cpp
struct BundleModuleRecord {
  uint32_t identityString;
  uint32_t kind; // ModuleKind
  uint32_t flags; // new
  uint32_t payloadOffset;
  uint32_t payloadSize;
};

constexpr uint32_t kRequirable = 1u << 0;
```

A separate field, not spare bits in `kind`. The record is fixed-width and
the version bump covers the width change, so the only thing packing would
buy is four bytes per module against a reader that has to mask before it
can compare a kind.

A `package.json` packaged only because the resolver read it is `kJSON`
with `kRequirable` clear: `BundleFileSource` reads it, `require()` cannot.
If the program requires that same file, the producer sets `kRequirable` on
the one record and it serves both roles. Everything discovered the ordinary
way, through a `require()`, has the flag set.

Storing the fact as a flag rather than a kind is what keeps
`require('pkg/package.json')` failing exactly where Node fails, without a
duplicate record and without the resolver and the loader disagreeing about
what a given identity is. `--dump` prints the flag, so the container's
growth is explained rather than mysterious.

This is a format change, so the format version goes to **v2**. A
format-version mismatch is already fatal in both `BundleReader::open` and
`openForInspection` -- the tables are laid out by the version, and a reader
guessing at a different layout would print confident nonsense -- so old
containers are rejected with the message they should get and no new
compatibility machinery is needed.

## Run-time wiring

`libjs/bundle-loader.js` gets one new native call:

```
bundle.resolve(fromIdentity, request, paths) -> identity | undefined
```

`paths` is the `options.paths` array when the caller passed one, otherwise
undefined.

Resolution happens entirely in identity space. An absolute path -- whether
it arrives as the request or inside `paths` -- is mapped in by stripping
the bundle root, and one that does not lie under the root maps to nothing.
Such an entry is a miss rather than an error, since that is what the
caller's own `try`/`catch` around `require.resolve` expects, and in a
closed world a directory outside the artifact genuinely holds nothing.

**`Module._load` from a bundled importer** consults the edge table, then
`bundle.resolve`, then throws. The `originalLoad` fallback is deleted.

**`Module._load` with no bundled importer** is now unreachable through the
bundle's own graph, since everything the entry loads is bundled. It throws
rather than falling back, so that if it does happen it is reported instead
of quietly reopening the disk.

**`require.resolve`** consults the edge table, then `bundle.resolve`,
including the `options.paths` form. Today that form deliberately skips the
edge table and defers to `Module._resolveFilename`; with no filesystem
behind it, deferring means failing, and Babel reaches exactly this path:
`require.resolve(id, { paths: [dirname] })` in
`@babel/core/lib/config/files/plugins.js`.

**Absolute paths need no special case.** An absolute path under the bundle
root is an identity once the root is stripped, and `BundleFileSource`
answers it through the same code as everything else -- including the
extension and `index.*` rules, which a bare string comparison would miss.

**Builtins are unchanged** and still checked first, via
`BuiltinModule.normalizeRequirableId`, so the bundle can never shadow one.

## Error policy

| Situation | Behavior |
| --- | --- |
| Specifier not in the bundle | `Cannot find module '<x>'`, naming the importer's identity and suggesting `--include=<x>` |
| Specifier resolves to a `.node` addon | Its own message: native addons are not supported in a bundle yet |
| `require()` with no bundled importer | Error naming the specifier; not a fallback |
| `--include` that does not resolve | Build error |
| Everything the producer cannot follow | Build warning, as today |

The not-found error keeps `code = 'MODULE_NOT_FOUND'`, because programs
branch on it -- an optional-dependency probe must still see the error it
expects, and in a closed world the probe's answer is legitimately "no".

## What stops working

**Native addons in a bundle.** They are skipped at build time and loaded
from disk at run time today, which is exactly the mechanism being removed.
They get a specific error until they have their own mechanism.
`examples/bufferutil-addon` continues to work unbundled.

**Computed requires of anything not packaged.** This is the change working
as intended: the failure moves from "silently reads the disk" to "says what
is missing and how to add it".

**Tests.** `test/bundle-fallback.js` is retired -- every case in it asserts
the fallback. The COLLIDE / BARE / THROW cases in `test/bundle-run.js` are
rewritten as closed-world assertions. `test/bundle-tolerant.js` keeps its
shape: an unresolvable specifier still throws `MODULE_NOT_FOUND`, now from
the container instead of the disk resolver.

**Corrected during implementation, 2026-08-19.** "Every case in it asserts
the fallback" was wrong: only three did. The rest assert what the *scanner*
reports -- a literal require is not counted as computed, the wrapper's own
`require` parameter is not an escape, `typeof require` is not, a shadowed
`require` contributes no edges -- which are properties of the scan, not of
the loader, and are covered nowhere else. The file was renamed
`test/bundle-scanner.js` and those cases kept; the three that rested on the
fallback were rewritten around `--include`, and its two shared-`Module._cache`
cases moved into `test/bundle-run.js`.

## Testing

The no-drift property gets a direct test rather than an indirect one: a
fixture tree, a list of specifiers exercised from several directories, and
an assertion that `DiskFileSource` and `BundleFileSource` return the same
answer for every one. A resolver bug that affects only one backend fails
here, which is the failure this design exists to prevent.

Beyond that:

- `BundleFileSource` unit tests: `isDirectory` on a prefix that is not a
  whole segment (`node_modules/foo` must not make `node_modules/foobar` a
  directory), exact-match files, missing `package.json`.
- Closed-world lit tests: a computed require of a non-packaged module
  errors and names `--include`; the same module with `--include` resolves;
  a `.node` addon gets the addon message; `require.resolve` with
  `options.paths` answers from the container.
- `examples/babel-parser`: `transform.js` -- the unmodified one, naming its
  preset by string -- builds with `--include=@babel/preset-env` and runs
  with the tree hidden. That is the end-to-end statement of the feature,
  and `run.sh` already hides the tree for every bundle case.

## Out of scope

- **`fs` reads of data files.** A program that reads its own data files
  still needs them. That is not module loading and the loader has no
  business intercepting it.
- **`package.json` `exports`.** Still unconsulted -- now in exactly one
  place rather than two, which is a small improvement in itself.
- **Native addons.** Deferred deliberately, to be added as its own case.
- **A trace-based producer.** Running the program to record what it loads
  would find these dependencies without annotation, and remains the
  alternative to `--include`. Not in this round.

## Closed in review, 2026-08-19

Three doors the design's "the loader never touches the filesystem" did not
account for, found by the final whole-branch review and closed:

- **`globalThis.require`.** `libjs/loader.js`'s `loadModule()` falls back to
  `readFileSync(filepath)` + compile for any id it has no embedded bytecode
  for, and that loader is still the global `require`. A bundled module's
  own wrapper `require` parameter shadows it, so an ordinary `require()`
  never reached it -- but `(0, eval)('require')`, `global.require` and
  `new Function('return require')()` all do. `installBundleLoader()` now
  calls `globalThis.__closeDiskModuleLoading()`, a one-way switch that
  disables the fallback (and the `readFileSync` probing in
  `resolveRelative()`) for the rest of the process. The global function
  itself has to stay: `libjs/shims/domain.js` requires `events` through it
  at its own load time, so deleting it would break `require('domain')`
  inside a bundle.
- **`Module.createRequire()`.** The `require` it returns is Node's own,
  around a `Module` with a filename and no `__bundleIdentity`, so every
  specifier it named hit the "no bundled importer" throw -- including ones
  the container holds -- and its `resolve()` walked the real filesystem.
  `identityOf()` derives an identity from `parent.filename` when it lies
  under the bundle root (same lexical rule as
  `BundleFileSource::stripRoot`), and `Module._resolveFilename` is wrapped
  next to `Module._load` so resolution is closed as well as loading.
- **The `--include` the error suggests.** `--include` resolves from the
  **entry's** directory, so echoing a relative request back verbatim
  printed advice that fails: a computed `require('./helper')` inside
  `node_modules/foo/index.js` suggested `--include=./helper`, where the
  invocation that works is `--include=./node_modules/foo/helper`. The
  suggestion is now computed (importer's identity directory + request,
  expressed relative to the entry's directory); where none can be computed
  the message says where `--include` resolves from rather than printing a
  value that does not work.

A fourth, in the producer rather than the loader: `DiskFileSource::
readPackageJson` concatenated `dir + "/package.json"` without trimming the
trailing slash `lexically_normal()` leaves behind when a `..` cancels a
segment, so `require('..')` recorded `<dir>//package.json` and packaged the
same file twice under one identity. Both backends now share
`trimOneTrailingSlash()` from `file_source.h`.

## Risks

**The yargs test may surface a real gap.** `test/bundle-yargs.js` runs a
real CLI, and the build reports two computed requires and one escape in
that tree. If any of them is on the live path, closing the world will fail
that test -- correctly, and it will need an `--include` or an admission
that the case is not yet covered.

**`BundleFileSource` models directories from a flat list.** Cheap, but the
prefix compare has to be segment-aware, and a wrong answer there changes
resolution rather than merely slowing it.

**The refactor touches the producer's resolver,** which every existing
bundle test depends on. `--include` and the run-time half are additive;
the `FileSource` extraction is not, and it lands first.
