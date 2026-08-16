# Implementation Progress

Tracks progress on `history/plans/2026-08-15-aot-bundle-plan.md`
(implementation plan) and its companion design doc
`history/plans/2026-08-15-aot-bundle-design.md`.

## Status

| Step | Description | Status |
|------|-------------|--------|
| Task 1 | Container format and reader/writer round trip | done |
| Task 2 | Generation tag | done |
| Task 3 | Static discovery: find literal `require()` calls | done |
| Task 4 | Resolution and the build root | done |
| Task 5 | The producer: walk, compile, write | done |
| Task 6 | The consumer: load, validate, and run | done |
| Task 7 | Error paths and the fallback | done |
| Task 8 | Example, docs, and progress file | done |

All eight tasks are complete. Each of Tasks 1 through 7 went through at
least one review round; the per-task ledger, including every deferred minor,
is in `.superpowers/sdd/2026-08-15-aot-bundle-plan/progress.md`.

## Task 8: End-to-end verification on a real package

### The producer defect found and fixed here

Task 8 was meant to be documentation plus one lit test. Bundling
`examples/yargs-cli` immediately failed the property the whole feature
exists to provide. The producer packaged only `*.js`, `*.ts` and `*.json`,
so it printed:

```
warning: skipping .../node_modules/yargs/yargs (no extension, not packageable)
warning: skipping .../node_modules/yargs/build/index.cjs (.cjs is not packageable)
```

and with `node_modules` moved aside the bundle died with
`Error: Cannot find module 'yargs/yargs'`. Both skipped files are ordinary
CommonJS JavaScript: yargs' public entry point is a file with no extension,
and every yargs-family package ships its real code as `build/index.cjs`
because they are `"type": "module"` packages. The design puts JavaScript in
scope and reserves warn-and-skip for `.node` or other non-JS/JSON, so the
extension whitelist contradicted the spec.

Fixed in `lib/bundle/bundle_build.cpp` by replacing the three scattered
extension tests with one `classifyFile()`:

- `.js`, `.cjs`, `.ts`, and **no extension at all** are packaged as
  JavaScript. An extensionless file carries nothing to infer a kind from,
  so the kind follows the resolver's decision that the file is a module.
  The container records each module's kind explicitly and the consumer
  reads it from there (`bundle_run.cpp`'s `reader->kind()`, and
  `typeof payload === 'string'` in `libjs/bundle-loader.js`), so nothing
  re-derives a kind from the identity's extension. Checked, not assumed.
- `.json` is packaged as raw text, unchanged.
- `.mjs` stays skipped, with its own message
  (`.mjs is ESM, not packageable`). It is JavaScript, but it is ESM, and
  this runtime's CommonJS loader cannot execute ESM at all: `require()` of
  an `.mjs` throws whether or not a bundle is involved, so there is no
  working disk fallback being preserved. The reason to skip it is narrower
  and stronger than that: its `import`/`export` syntax is a syntax error
  inside the CommonJS wrapper, so packaging one would fail the entire build
  over a module that could never have run. Every other extension is skipped
  as before.
- The entry check now uses the same classifier, so a `.json`, `.mjs` or
  `.node` entry is still rejected before any work is done. Its message says
  "CommonJS JavaScript or TypeScript": an `.mjs` file is JavaScript, and a
  message that only said "JavaScript" would read like a bug.
- A dotfile classifies as extensionless, since `fs::path` reports no
  extension for `.babelrc`. That matches Node and matches this repo's own
  loader, both of which treat a dotfile as `.js`, but it does mean
  `require('./.babelrc')` now fails the build instead of warning and
  skipping. Called out in the classifier's doc comment so the change is not
  silent.

### A second producer defect: no shebang strip

Found in review, and made reachable by the fix above. `libjs/loader.js` and
`compileFunctionForCJSLoaderCb` in `lib/bindings/node_contextify.cpp` both
strip a leading `#!` line before compiling; the bundle producer wrapped and
compiled raw source. A hashbang is legal only at the very start of a
Program, so inside the CommonJS wrapper it is a syntax error:

```
$ hermes-node cli.js                    # requires an extensionless bin module
V 4
$ hermes-node --build-bundle=app.bundle cli.js
error: failed to compile .../node_modules/tool/tool: SyntaxError: 1:61:empty private identifier
```

The defect predates the classifier change (a `.js` dependency and the entry
itself fail the same way), but `bin/` CLI scripts are exactly the shape that
carries a hashbang and they are commonly extensionless, so the change moved
that case from warn-and-skip with a working disk fallback to a hard build
failure. `stripShebang()` now runs before the scan, so the text the parser
sees and the text that becomes bytecode are the same string. The newline is
kept, which is what preserves line numbers in stack traces, exactly as the
other two paths do it.

Covered by the `HASHBANG` case in `test/bundle-build.js`: entry and
extensionless dependency both carry a hashbang, the tree is deleted, and the
run asserts both the output and the two stack-trace line numbers. Without
the retained newline both frames shift by one, so the line-number checks pin
that detail specifically.

Regression coverage lives in two places. `test/bundle-build.js` gained a
synthetic case (`KINDS` / `KINDSOUT`) that packages a `.cjs` and an
extensionless `main`, skips an `.mjs`, then deletes every source file and
runs the bundle. That one is offline and always runs. `test/bundle-yargs.js`
covers the real thing but is gated (see below), so the default suite would
otherwise have had no coverage of this fix at all.

Both were confirmed to fail with the fix reverted: rebuilt with the old
whitelist, `bundle-yargs.js` fails at `BUILD-NOT: warning:` and reports the
two skipped yargs files by name, and a hand-run of the same scenario
reproduces `Cannot find module 'yargs/yargs'` exactly.

### Verified end to end

`examples/yargs-cli` is a yargs CLI with 16 packages under `node_modules`.
Built and run with `cmake-build-release`:

```bash
cd examples/yargs-cli
../../cmake-build-release/bin/hermes-node --build-bundle=/tmp/yargs.bundle ./greet.js
# bundle root: /home/tmikov/work/hermes-node-compat/examples/yargs-cli
cp /tmp/yargs.bundle ./app.bundle          # the bundle must sit at the root
mv node_modules /tmp/stashed-nm
../../cmake-build-release/bin/hermes-node --bundle=./app.bundle -- --help
../../cmake-build-release/bin/hermes-node --bundle=./app.bundle -- hello --name World --excited -r 2
../../cmake-build-release/bin/hermes-node --bundle=./app.bundle -- count --from 1 --to 5
mv /tmp/stashed-nm node_modules
```

Results:

- No warnings during the build. Every file the graph reaches is packaged.
- Identical output with `node_modules` present and absent, for the usage
  banner and for both subcommands, exit code 0 throughout.
- `HERMES_NODE_DEBUG_NATIVE=BUNDLE` logs **zero** misses on the
  `node_modules`-deleted run: nothing falls back to disk.
- Container: 21 modules, 26 edges, 261224 bytes (255 KB).

Note the `--` separator. A bare `--help` is consumed by hermes-node itself;
arguments for the bundled program go after `--`. (The plan's Task 8 sketch
named the entry `cli.js` and omitted the separator. The entry is
`greet.js`.)

### The lit gate

`test/bundle-yargs.js` carries `REQUIRES: examples-installed`, a feature
`test/lit.cfg` adds only when `examples/yargs-cli/node_modules` exists.
`examples/*/node_modules` is gitignored and installed by hand, and the
default suite is offline, which is why `check-hermes-node-examples` is a
separate target in the first place.

The gate was verified to actually gate, not just to pass on a machine where
the example happens to be installed: pointing the `os.path.isdir` check at a
directory that does not exist makes lit report the test UNSUPPORTED rather
than PASS or FAIL. Restored afterwards.

The test copies the example to `%t` before bundling (the bundle has to live
at the build root, and nothing may be written into the checkout), then
deletes `node_modules` *and* `greet.js` from the copy before running. It
asserts the build skips nothing shaped like a `.cjs` or an extensionless
file, which is what pins the producer fix. It deliberately does not ban the
word `warning:` outright: skipping an `.mjs` or a `.node` addon is the
producer working correctly, and a blanket ban would fail the day yargs ships
one. The offline `KINDS` case in `test/bundle-build.js` pins the
classification deterministically.

### Documentation

- `README.md` gains an "AOT bundles" section after "Compile cache", and its
  copy of the `--help` output was stale: `--build-bundle` and `--bundle`
  were missing from it.
- `CLAUDE.md` gains an "AOT Bundles" section.

Both state the limitations as plainly as the features: bundle must sit at
the printed root; `.node` addons and other non-JS/JSON assets stay on disk;
`.mjs` is not packaged; `--inspect` is refused; `package.json` `exports` is
unsupported in v1; a computed `require()` is invisible to static discovery
and falls back to disk, visible under `HERMES_NODE_DEBUG_NATIVE=BUNDLE`.

### Builtin module name count

A doc defect was recorded during Task 4 for Task 8 to fix: a stale claim of
"31 module names" for `libjs/shims/internal/bootstrap/realm.js`. The claim
turned out to live in the user's memory file, not in the repository's
`CLAUDE.md`, which never carried a number.

Recounted programmatically: `builtinIds` in `realm.js` has **43** entries,
and `builtinIds()` in `lib/bundle/bundle_resolve.cpp` has the same 43, in
the same order, with no set difference. (A reviewer's earlier count of 42
was off by one; the two lists agree.) The new `CLAUDE.md` section states 43.
Nothing was changed in either list.

### Concerns

- The bundle-must-sit-at-the-root rule is a real ergonomic wart: the
  producer writes the bundle wherever `--build-bundle` points, prints the
  root, and does not warn if the two disagree. The `--bundle-root` override
  the design's Risks section names as the mitigation was not built (see the
  plan's self-review notes); the printed root is the cheaper half of it.

## Final whole-branch review

A last review gate over the whole branch found four cross-task defects --
each living on a boundary no single-task review could see -- plus four
smaller items. All are fixed on `work`; each fix was reproduced first and
each new test was confirmed failing without it.

### The four defects

- **The bundled `require` was a stub.** `require.resolve` returned the
  specifier text unchanged and `require.cache` / `require.extensions` /
  `require.resolve.paths` / `module.require` did not exist, while the same
  file loaded from disk after a miss got Node's real `require`. The wrong
  `resolve()` answer reached a shipped example: `mask.js` in
  `examples/bufferutil-addon` does
  `path.dirname(require.resolve('bufferutil'))`, got `'.'`, and looked for
  the addon in the cwd. Now built with `makeRequireFunction`, with only
  `resolve()` overridden (edge table first, so it survives tree deletion).
- **`require('..')` resolved to the wrong directory** and `require('.')`
  failed the build: the producer tested for the `"./"` and `"../"`
  prefixes, so those two exact specifiers went into the `node_modules`
  walk, where `<dir>/node_modules/..` normalizes back to `<dir>`. Replaced
  with Node's own predicate from `Module._resolveLookupPaths`. The
  redundant `node_modules/node_modules` probe went at the same time.
- **A program using the vendored `ws` could not be bundled at all.** With
  no `node_modules` copy to resolve, the producer hard-errored on a
  specifier the runtime serves from the copy embedded in the binary. It
  now warns and skips; an installed copy is still packaged and still wins.
- **A module could be instantiated twice**, once from the container and
  once from disk, because the loader never registered bundled modules in
  `Module._cache`. Bundled records are now published there and
  `loadIdentity` consults it, in both directions.

### Design doc corrections

Two claims in `2026-08-15-aot-bundle-design.md` did not match what
shipped, and were corrected in place (the precedent is commit `bd54816`):

- The Testing section named `examples/flow-bundler` as "the natural
  regression test for the fallback path". No such test exists, and it is
  not achievable as written: the bundler's sources are Flow + ESM compiled
  on the fly by `@babel/register`, so the producer stops at the entry with
  `error: failed to parse .../buildBundleCLI.js: 11:12: 'from' expected`.
  Verified by running the producer against that entry. The fallback path is
  covered by synthetic fixtures in `test/bundle-fallback.js` and
  `test/bundle-run.js` instead.
- The error-policy table said "Truncated or corrupt container | Hard error"
  without qualification. `BundleReader::open` validates structure only --
  magic, version, generation, offsets, lengths, alignment, indices -- and
  nothing covers payload bytes, so a bit flipped inside a bytecode payload
  produces silent misbehaviour rather than an error. The asymmetry with the
  compile cache (which stores a CRC32 per entry) is now stated in the
  design doc and in `bundle_reader.h`. No checksum was added: that is a
  format change and out of scope for this round.

### Test coverage added for already-shipped behavior

- TypeScript was in the spec's Scope and fully wired, but had no end-to-end
  test. `test/bundle-build.js` now builds a `.ts` entry with a `.ts`
  dependency and a `.json`, deletes the sources, and runs from the
  container.
- The `__dirname` escape hatch -- the single reason the design rejected a
  virtual-filesystem prefix -- is now covered in `test/bundle-run.js`: a
  bundled module reads a data file next to where its source used to be,
  with the source deleted and the cwd elsewhere.

### Concerns from this round

Superseded by the re-review below, which judged the first bullet
understated and the second acceptable. Both are restated as known
limitations there.

- Cross-boundary circular requires: when Node's `_load` finds a bundled
  module mid-load it stamps the circular-require warning proxy onto its
  exports and removes it when the module finishes. `loadIdentity` cannot
  remove it -- the proxy is module-local to Node's loader.
- `require.resolve(x)` is not a discovery site: the scanner records
  `require()` calls only.

## Re-review of the final fixes

The re-review confirmed all eight findings addressed and every claimed
revert-test failing, and checked cycle termination, throw-eviction with
re-execution, `require.main` / `process.mainModule` / `module.id` /
`module.parent` parity, `module.children`, a symlinked bundle root, and
non-bundle startup against Node itself. It raised three further items.

### NB-3: `delete require.cache[...]` silently no-opped (fixed)

Introduced by the round above: publishing records into `Module._cache`
while also keeping the loader's own identity-keyed cache made the standard
reload idiom -- `delete require.cache[require.resolve(x)]`, then require
again -- return the stale module. Reproduced as `RELOAD 1 1 true` under a
bundle against `RELOAD 1 2 false` from disk and from Node. Before the round
`require.cache` was `undefined`, so the same code threw a loud `TypeError`;
silently answering wrong is worse.

Fixed by **deleting the identity cache entirely** and keying only on
`Module._cache[filename]`, which was the reviewer's preferred option and is
the one that removes the failure mode rather than papering over it: a cache
the loader keeps to itself is a cache the program cannot invalidate, and
the "keep it as a view" alternative would leave a second copy of the
invariant to get out of step later. The identity cache was redundant
anyway -- `identity -> path.join(root, identity)` is injective, since
identities are distinct normalized paths relative to one root.

The four behaviors that depend on the cache were re-verified against the
new one-cache shape, disk and bundle printing identical lines:

- cycle termination and partial exports: `CYCLE A B A undefined`
- throw-eviction with **re-execution** on retry: `THROW1 boom1`,
  `THROW2 boom2` (the module body runs a second time, not just the throw)
- `require.main` / `process.mainModule` / `module.id`: `. true true true`
- the entry re-required from a dependency yields the same record, not a
  second copy

`test/bundle-require.js` gained a `RELOAD` case (verified failing before
the fix) and a `CYCLE` case (a guard: it passes before and after, and
exists because the cache has now been reshaped twice).

### NB-4: `require.resolve(spec, { paths })` ignored `options.paths` (fixed)

The edge table was consulted before options were examined, so an edge beat
the caller's explicit search path: disk and Node resolved to
`other/node_modules/pkg/index.js`, the bundle answered
`node_modules/pkg/index.js`. `resolve()` now skips the edge table whenever
`options.paths` is present -- including a malformed one, which
`Module._resolveFilename` then rejects with Node's own error -- because an
explicit `paths` is the caller replacing the search path outright, a
different question from the one an edge answers. `OPTPATHS` case in
`test/bundle-require.js`, verified failing before the fix.

### Known limitations (documented, not fixed)

**Circular require across the container/disk boundary leaves the warning
proxy on `exports`.** Shape: a bundled module's load is re-entered from a
disk-fallback module -- bundled -> computed `require()` -> disk module ->
back to the bundled module while it is still loading. Node's `_load` finds
the record in `Module._cache`, sees `loaded === false`, and stamps
`CircularRequirePrototypeWarningProxy` on its exports
(`libjs-node/internal/modules/cjs/loader.js:1026`); it removes the stamp
when *its own* load finishes, but `loadIdentity` never does, so the
prototype outlives the load. Reproduced: `Object.getPrototypeOf(exports)
=== Object.prototype` is `true` from disk and `false` under a bundle, and a
read of a non-existent property fires `Warning: Accessing non-existent
property ...` at an explicit `process.on('warning')` listener. This is more
than diagnostics -- a `Object.getPrototypeOf(x) === Object.prototype`
test, which is how most `isPlainObject` helpers work, gets the wrong
answer.

Not fixed because the correct fix is Node's own: an identity-guarded reset
at the end of the load, which needs a reference to the proxy object. That
proxy is module-local to `libjs-node/internal/modules/cjs/loader.js`, and
this plan's constraints forbid modifying `libjs-node/`. The available
alternative -- resetting the prototype heuristically whenever it is not
`Object.prototype` -- would misfire on a module that deliberately sets its
own exports prototype, trading a narrow wrong answer for a broader one.
Deliberately kept out of the README: it is too narrow to sit next to the
real user-facing limitations.

**The producer resolves lexically and does not follow symlinks.** One root
cause, two visible consequences, neither addressed this round:

- *An `npm link`-shaped tree can still instantiate a module twice.* Module
  identities come from `joinNormalized` (lexical), while Node's
  `Module._cache` key comes from `_findPath`, which calls `toRealPath`
  unless `--preserve-symlinks` is set (our options shim leaves it false).
  With `node_modules/pkg -> ../store/pkg`, the bundled record is keyed
  `<root>/node_modules/pkg/index.js` and a fallback resolution of the same
  file is keyed `<store>/pkg/index.js`, so the two do not meet. Reproduced
  with a specifier that resolves to the same file by different text: disk
  prints `LINK true 1 1`, the bundle `LINK false 1 2`. NB-3 removed the
  other half of this class (the loader's private cache); this half is a
  deeper identity question -- identities would have to be realpath-based,
  which changes what an identity *is* -- and is not this round's work.
- *A pnpm-shaped tree fails the build outright* (pre-existing, out of scope
  for this round). The producer walks `node_modules` lexically from the
  importer's directory, so from `node_modules/pkg/index.js` it never
  reaches `node_modules/.pnpm/pkg@1/node_modules/`, where that package's
  own dependencies live. Reproduced: the program runs from disk
  (`PNPM DEP`) and the build stops with
  `error: cannot resolve 'dep' from .../node_modules/pkg/index.js`.

**Bundled module records are plain objects, not `Module` instances.** They
duck-type the fields Node's loader reads (`exports`, `loaded`, `filename`,
`id`, `path`, `paths`, `parent`, `children`, `require`), and nothing on the
loader's paths through `bundle-loader.js` needs the prototype. Since
`Module._cache` is now the loader's only cache, these records are also what
a third-party `require.cache` walker sees, which makes the difference
observable: `require.cache[f] instanceof Module` is `false` and
`module.constructor.name` is `'Object'`, where Node and a disk load both say
`Module`. Pre-existing, and left alone because constructing real instances
would run Node's constructor -- which does its own `updateChildren()` and
`paths` setup -- only for the fields to be overwritten immediately
afterwards. Noted at the record-construction site in
`libjs/bundle-loader.js`.

**Intra-bundle cycles do not stamp the circular-require warning proxy.**
Where Node and a disk load put `CircularRequirePrototypeWarningProxy` on a
partially loaded module's exports, a cycle entirely inside the container
does not: `loadIdentity` returns `mod.exports` directly. The exports value
is the same either way, so this costs a diagnostic warning rather than
correctness -- the opposite direction from the NB-2 limitation above, which
leaves the proxy on when Node would have taken it off.

**`require.resolve` is not a discovery site.** The scanner records
`require()` calls only, so a specifier that is *only* ever passed to
`require.resolve` has no edge and is answered by the filesystem resolver,
which throws `Cannot find module` once the tree is deleted. Accepted in
review as strictly better than the old silent wrong value; the common
idiom (`require('pkg')` alongside `require.resolve('pkg')`, which is what
`examples/bufferutil-addon/mask.js` does) is fully covered. Recording
`require.resolve` specifiers as edges would close it and is additive.
