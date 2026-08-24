# Implementation Progress

Tracks progress on `docs/superpowers/plans/2026-08-15-aot-bundle-plan.md`
(implementation plan) and its companion design doc
`docs/superpowers/specs/2026-08-15-aot-bundle-design.md`.

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

## 2026-08-16: a bundle is not self-contained when specifiers are computed

Recorded from bundling `examples/babel-parser/transform.js`.

**Read this with the entries below it.** What follows describes the system
as it was on 2026-08-16, and the shape of the problem is still right, but
the specifics are not: the disk fallback it keeps referring to was removed
on 2026-08-19, and three of the four directions it lists as unchosen were
subsequently taken -- `--include`, the build-time warning, and recording a
literal `require.resolve` as an edge. Where it says the gap is "passed over
in silence", the build now prints a count and `--verbose` the positions,
and the run fails with an error naming `--include` rather than quietly
reading the disk.

What remains open is the part no static analysis can close: a specifier the
program computes names nothing the walk can follow, so if nothing else
pulled the target in, the container does not hold it and the program fails
when that code path runs. The remedy is to declare it (`--include`), which
works -- `examples/babel-parser` bundles the unmodified `transform.js` that
way. The unimplemented direction is the first one listed below: a
`--verify` pass that runs the entry from the container with the tree hidden
and reports every miss, which is the only answer that finds these without
being told where to look.

The static walk is an under-approximation of what the program loads. Every
specifier that only a computed `require()` can reach is missing from the
container, and the disk fallback covers it, so the bundle runs correctly
wherever the source tree is still present. What it does not do is say so.
The build reports nothing, `--dump` shows a container that looks complete,
and the gap appears only when the tree is gone -- which is the situation a
bundle exists for.

The Babel case is the archetype: `transform.js` requires only
`@babel/core`, and Babel resolves preset and plugin names at run time from
its configuration. `@babel/preset-env` and its whole subtree are therefore
never discovered. The bundle builds without complaint, runs correctly with
`node_modules` in place, and dies with `Cannot find module
'@babel/preset-env'` once it is removed. `HERMES_NODE_DEBUG_NATIVE=BUNDLE`
is the only thing that reports it, and only at run time, on the run that
happens to take the fallback.

This is the opposite direction from the two build-time failures fixed on
2026-08-16 (an unresolvable specifier and an uncompilable file, both of
which were fatal for programs that run fine). Those were over-approximation
punished as error; this is under-approximation passed over in silence.

Directions, none of them chosen:

- Report it at build time. The producer already knows nothing about these
  specifiers, so the honest version is a run-time-shaped answer: a
  `--verify` pass that runs the entry from the container with the tree
  hidden and reports every fallback as a gap.
- Record `require.resolve` specifiers as edges (see the limitation above),
  which would close the subset that is resolved statically but required
  dynamically. It does not reach the Babel case, where the name comes from
  configuration.
- An explicit include list on the producer (`--include=<specifier>`), which
  makes the program's dynamic dependencies a declared input rather than
  something to be inferred.
- Accept it and document it, on the grounds that a static bundler cannot
  see a dynamic require and every ecosystem bundler solves this with
  configuration.

### Partly addressed 2026-08-16: the build now says how much it could not see

The scanner records the position of every `require()` whose argument is not
a literal (`ComputedRequire` in `require_scanner.h`), and the producer
reports the count by default and the positions under `--verbose`. That is
the cheap half of the first direction above: it does not close the gap, but
the container no longer looks complete when it is not. Webpack's "Critical
dependency: the request of a dependency is an expression" is the same
warning.

It reaches less than expected. Bundling `examples/babel-parser/transform.js`
warns about 4 computed calls, all in `browserslist`, and about **none** of
the preset loads that actually make that bundle non-self-contained. Babel
does not write `require(expr)`; it writes

```js
module = (0, _rewriteStackTrace.endHiddenCallStack)(require)(filepath);
```

`require` escapes as a first-class value, so there is no `require(...)` call
in the source to warn about. Catching that means warning when the identifier
`require` is used anywhere other than as a call callee (or as the object of
a member expression, which covers `require.resolve` and `require.cache`).
Webpack warns about this shape too, separately. Not implemented; it is the
next increment and it is what would have flagged the Babel case.

### 2026-08-16, second increment: require is identified by binding

The scanner now wraps each source in the CommonJS module wrapper before
parsing, runs `sema::resolveAST`, and treats an identifier as `require`
only when it resolves to the wrapper's `require` parameter. Three things
follow, two of them fixes rather than features.

**A top-level `return` no longer breaks the build.** A module body is a
function body, and `return` is an ordinary CommonJS early-exit idiom.
Scanning the source as a Program rejected it. Before the tolerant-build
change that was a hard error; after it, it was worse -- a module that runs
correctly from disk was silently packaged as one that throws. Wrapping is
what the compile step always did; now the scan does it too, from the same
`kCJSWrapperPrefix` in `include/hermes/node-compat/bundle/cjs_wrapper.h`.

**A shadowed `require` is no longer followed.** `(function (require) {
require('./x'); })` is what browserify and older webpack output ship
inside a package's `dist/`. Those specifiers were only ever meaningful
inside that bundle's own module map, and the producer used to look for
them on disk -- a hard error before, a wrong "cannot be resolved" warning
after. Now they are not edges at all.

**`require` used as a value is reported.** A second warning line, counted
like the computed-argument one and listed by `--verbose` as `escape`. This
is the shape that hides Babel's preset loads:
`endHiddenCallStack(require)(filepath)` at `module-types.js:57:57` is now
named. It reports the hole without naming what is in it -- there is no
specifier to recover -- but issue 3 above is no longer silent for this
case.

Two things learned in the process, both recorded in the tests:

- Hermes records an expression decl on the wrapper's `require` **parameter**
  as well, so without excluding that one node every module reports an
  escape at its own line 1.
- `sema::resolveAST` constant-folds, so `0+1+1+...` reaches the visitor as
  a single literal. `RequireScannerTest.ReportsErrorOnExcessiveNesting`
  now uses a non-foldable chain; with the old fixture it silently stopped
  testing the recursion guard.

Build time over the ~1500-file Babel example is unchanged at ~1.1 s.

**Follow-up the same day: the escape warning over-reported.** Checked
against `examples/yargs-cli`, which reported five escapes; four were
`typeof require` or an equality comparison against it, and one was real
(`require: require` stored on an object, later called with computed
arguments). Testing whether require exists yields a boolean and can load
nothing, and the idiom is in essentially every UMD-flavored file a package
ships, so the warning was 80% noise on the first real tree it met.

The scan now accounts for `require` as the operand of `typeof`, `!` or
`void`, and on either side of `==`/`!=`/`===`/`!==`. Only equality:
`f(require)`, `var r = require` and `'' + require` still count, because
each of those can hand the value on. Both examples now report exactly one
escape, and each is genuine.

### 2026-08-19: the world is closed

The run-time disk fallback is gone. Design
`2026-08-19-closed-world-bundle-design.md`, plan
`2026-08-19-closed-world-bundle-plan.md`, seven tasks; this is the outcome
of the last one, which is the only breaking one.

A bundle now answers every `require()` from the container -- edge table
first, then the container's own resolver, running the producer's
`resolveSpecifier` against `BundleFileSource` -- and throws when neither
can. The error names the importer's identity and the remedy
(`--include=<specifier>`), and keeps `code = 'MODULE_NOT_FOUND'` so an
optional-dependency probe still sees the answer it branches on. A `.node`
specifier gets its own text instead, since `--include` cannot help it.

Two things are still served from outside the container, and neither is a
filesystem read: builtins, and a vendored package (`ws`) with no packaged
copy. Both come out of the binary. The precedence is unchanged --
`normalizeRequirableId` decides builtins before the container is consulted,
`Module.isBuiltin` (which also answers for `ws`) is consulted only after
both container lookups have missed, so an installed `node_modules/ws` that
was packaged still wins.

`require.resolve` was closed the same way, and this went beyond the letter
of the task: it used to fall through to `Module._resolveFilename` on a
miss. A `resolve()` that answers where the `require()` after it throws is
worse than either, and with no filesystem behind it deferring means failing
anyway. Only a builtin, a vendored name, and a malformed `options.paths`
(whose `ERR_INVALID_ARG_VALUE` is Node's own error to construct) still
reach `baseResolve`.

**`probeForContainer` is gone.** Task 5 bounded the `options.paths` branch
of the container resolve to each entry's nearest `node_modules`, because a
climb past an ancestor that was merely unpackaged could land on a
different, real package of the same name while the disk fallback behind it
knew better. With no fallback there is no "merely unpackaged": a level with
no records is a level with nothing there, and the walk answers exactly what
the producer's own walk answered. One algorithm, one set of inputs, both
sides -- which is the point of the whole design.
`test/bundle-require.js`'s OPTPATHS case now asserts that (the container's
NEAR, not the disk's FAR), plus a `paths` miss throwing rather than going
looking.

**`test/bundle-yargs.js` needed no `--include`.** The design flagged it as
the case at genuine risk -- a real 16-package tree with two computed
requires and one escape reported at build time. None of the three is on the
live path, and the container's resolver answers what is. That was the main
open question of the round and it came out clean.

**The Babel example now proves the feature.** `examples/babel-parser`
bundles the *unmodified* `transform.js` -- the idiomatic one, which names
`@babel/preset-env` in a string that no static bundler can follow -- with
`--include=@babel/preset-env`, and `run.sh` runs it with `node_modules`
moved aside. `transform-static.js` reaches the same place by requiring the
preset instead. Two routes, one of which does not touch the source, which
is the usual situation for a dependency several packages down.

**Tests.** `test/bundle-fallback.js` became `test/bundle-scanner.js`: the
brief said to delete it, but only three of its cases rested on the fallback
(the computed-require run, the escaped-require run, and the two
shared-`Module._cache` cases). The rest are assertions about what the
*scanner* reports -- that a literal require is not counted as computed,
that the wrapper's own `require` parameter is not an escape, that
`typeof require` is not, that a shadowed `require` contributes no edges --
which are properties of the scan and not of the loader, and are not
duplicated anywhere else. Deleting the file would have dropped them. The
two `Module._cache` cases moved into `test/bundle-run.js` as the brief
asked; the other two were rewritten around `--include`.

Also: the producer's computed-require warning no longer says "resolved from
disk at run time", because that is not what happens any more.

**Known limitation, unchanged in shape but sharper in consequence.** A
native addon cannot be loaded from a bundle at all. It could not be
packaged before either, but the fallback loaded it off disk;
`examples/bufferutil-addon` therefore runs unbundled only. Addons need
their own mechanism, deliberately deferred.

### 2026-08-20: bundle preloads (format v3), and the `-r` injection point closed

Design `2026-08-20-bundle-preload-design.md`, plan
`2026-08-20-bundle-preload-plan.md`, four tasks. `--build-bundle
--preload=<specifier>` (repeatable) resolves from the entry's directory
exactly as `--include` does, packages the module, and additionally records
its index in a new preload table -- format v3 -- in flag order (duplicates
collapse to one entry, same as `--include`). `--bundle` runs the table
through `loadIdentity` before the entry, in order; a preload observes
`require.main === undefined`, the same thing Node's `-r` observes, and a
throwing preload stops the run before the entry executes. `--dump` prints
a `PRELOADS` section.

This closes the loop `dedb781` left open. That commit hardened
`__closeDiskModuleLoading` against a preload reassigning it, but its own
message said plainly: "`-r` is still not rejected with `--bundle`; that
combination is unchanged by design, only the property's mutability is
hardened." The design doc traced the sharper problem underneath: preloads
ran at step 12c of `runHermesNode`, before the bundle loader was installed
at step 13, resolved from the real filesystem -- so `--bundle app.hbb` and
`--bundle app.hbb -r x.js` were different worlds, and a preload could reach
into the run that followed it (demonstrated against `dedb781`: a preload
planting `Module._cache[<root>/<identity>]` replaced a bundled module's
exports before the program ever saw the container's copy). This round's
last task makes `checkToolOptions()` refuse `--bundle` combined with
`-r`/`--require` outright, which removes the phase a preload could occupy
by construction rather than by further hardening a property on it.
`-r` with `--build-bundle` is untouched -- a build runs in the disk world.
`test/bundle-errors.js` NORFLAG is the regression test, asserting both the
refusal and (`--implicit-check-not`) that the preload's own output never
appears.

Two risks carried over from the design doc, neither addressed this round:

- **A container can now run code before its entry, and only `--dump` says
  so.** That is the point of the feature, but a bundle is no longer simply
  "run the entry" -- auditing one means reading the preload table, which is
  why `--dump` prints it rather than leaving it inspectable only through
  the format.
- **`examples/flow-bundler` is still not bundled.** It is the natural
  end-to-end case for this feature (it already uses `-r` to install
  `@babel/register` ahead of its own entry point), but bundling it means
  solving the computed-require problem in its strongest form -- the
  bundler loads Flow sources through `@babel/register` at run time -- which
  is a larger piece of work than this design. Tested with synthetic
  fixtures instead; confirmed this round that `run.sh`, which uses `-r`
  with no `--bundle`, is unaffected by the new refusal.

`dedb781`'s non-writable `__closeDiskModuleLoading` is now unreachable for
the attack it was written for: `installBundleLoader()` calls the closer as
its first statement, before `run()` executes any bundle-carried preload, so
a preload never occupies the window that hardening defended (a
`-r`/`--require` preload, the only thing that used to run earlier, is
refused outright by this round's flag check). It stays in place as defence
in depth rather than being reverted, but no test now exercises its
mutability -- the only test that reached it, `test/bundle-escapes.js` case
3, was rewritten to pin the refusal instead (a `-r` invocation that used to
reopen the escape now never starts the bundle at all).

### 2026-08-21: bundle residuals cleared

Six tasks closing the fourteen findings from a full-subsystem walkthrough
recorded at `.superpowers/sdd/2026-08-19-closed-world-bundle-plan/progress.md`
(plan `docs/superpowers/plans/2026-08-21-bundle-residuals-plan.md`, per-task ledger
`.superpowers/sdd/2026-08-21-bundle-residuals-plan/progress.md`). None of it
is new capability; all of it is the closed-world design's own guarantees
made to hold in cases the shipping round did not exercise.

**What the scanner and producer see.** A literal `require.resolve(spec)` is
now a discovery edge, exactly like `require(spec)` -- it used to be
invisible to the walk (only `require(...)` calls were followed), so a
target reached solely through `require.resolve` could be missing from the
container with no build-time signal at all, only a run-time
`MODULE_NOT_FOUND`. This changes what gets packaged, so both example
bundles were rebuilt to check for drift: container size on `babel-parser`
and on `yargs-cli` came out byte-for-byte identical before and after,
meaning every project in the offline test corpus already reached its
`require.resolve` targets through an ordinary `require()` as well, and no
fixture's expected file count needed updating.

That measurement says the size risk is *unexercised by this corpus*, not
that it is absent. The shape that triggers it is the pure feature probe --
`try { require.resolve('typescript'); hasTS = true; } catch (e) {}` -- a
widespread idiom whose whole point is that the package is never loaded.
A bundle built from a program containing one now packages that package
and its entire transitive graph, for a `require.resolve` whose only use
is to answer a boolean. Nothing in `examples/` does this, so nothing here
measured it; a real project that does will see the container grow, and
the answer for it (should one be needed) is a way to say "resolve, but do
not package", which does not exist today. A non-literal
`require.resolve(x)` now also counts toward the "computed require() call"
warning, by the same reasoning that already applied to `require(x)`: both
reach the run-time loader through the identical path (edge table, then
`bundle.resolve`, then throw), so both deserve the identical build-time
warning. Separately, the bundle root computation had a bug where a
`package.json` belonging to a *failed* resolution probe (a bare specifier
that does not resolve, e.g. an optional dependency one directory shallower
than every module the walk actually packaged) could widen the root outward
from what the packaged modules alone would justify -- `BundleFileSource`
can never serve anything outside the root it was given, so that widening
bought nothing and only made the root wider than it needed to be.

The shipped rule: a recorded `package.json` is kept when its directory is
an ancestor of (or equal to) some packaged module's directory, and dropped
otherwise -- and the common ancestor is computed AFTER that filter runs,
from the modules plus the kept `package.json` files, so a kept file can
still widen the root. Both halves matter, and the obvious simpler rule
does not work: computing the root from the walked modules first and then
keeping only the resolution inputs already under it was tried (58a8529)
and reverted (a0b88b0), because it drops a `package.json` a dynamic
resolution genuinely needs. A package whose whole reachable module graph
lives under its own `lib/` has its `package.json` one level ABOVE every
packaged module, so a modules-only root excludes it -- and without it the
container cannot answer a run-time `require.resolve('foo', {paths})`,
which does not go through the edge table and needs foo's `"main"` to find
the package by name. Being an ancestor of a packaged module is exactly the
test that separates that file from the failed probe's, and letting the
kept file widen the root afterwards is what puts it inside the container's
reach. `test/bundle-resolution-inputs.js` pins both halves: its `%t.wide`
case (prefix `WIDE`) pins the drop, its `%t.deep` case (prefix
`DEEPROOT`) pins the keep, and changing either half breaks one of them.
A `package.json` kept for resolution only is flagged `kResolveOnly` (a
named zero, not a bare `0`) rather than `kRequirable`; one the program
also require()s keeps `kRequirable`.

One residual, known and deliberately left: reachability for a resolution
INPUT is not the same as reachability for a module, so a package whose
`"main"` escapes its own directory (`"main": "../../../outside/real.js"`)
has its `package.json` dropped, and a computed require of that package by
name throws `MODULE_NOT_FOUND` at run time. The shape is pathological and
the alternative is the bug this filter fixed.

**What the debug log says.** `HERMES_NODE_DEBUG_NATIVE=BUNDLE` used to log
only a miss. It now traces all three outcomes a bundled `require()` can
have -- an edge-table hit, a resolve-time hit through `bundle.resolve`, and
a miss -- because a resolve hit today is one dependency change away from
becoming a miss tomorrow, and there was no way to tell that apart from an
edge-table hit without logging both. This is the log an `--include` set
gets built from, so the three outcomes being distinguishable is the point.
The `resolve:`, `edge:` and `miss:` lines are now all anchored with
`{{$}}` in the tests that pin them, closing a substring-match gap the
`resolve:` line's tightened wording opened (`... -> extra.js` unanchored
would also match `... -> extra.json`).

**What got de-duplicated.** `bundle_run.cpp` used to carry its own
hand-copy of `BundleFileSource`'s root-stripping rule
(`identityUnderRoot()`, including the root == "/" special case); one
implementation of that rule now lives in a public
`BundleFileSource::identityFor()`, and the C++ resolve callback calls it
directly. Three performance items landed alongside it: `DiskFileSource`
dedupes its read-path list through a set instead of a linear scan,
`BundleFileSource`'s sorted identity index is now built lazily on first
query instead of eagerly in the constructor, and the root-prefix string
used by every `identityFor()` call is precomputed once rather than
rebuilt per call.

**What the format now rejects.** `BundleReader::open()` (and
`openForInspection()`) validate two more identity invariants: an empty
path segment (`"a/"`, `"a//b"`) is rejected the same way a `"."` segment
already was, and a container carrying the same identity twice is rejected
outright. The duplicate check matters concretely, not just in principle --
it was reachable in practice through a trailing-slash bug, fixed in the
closed-world round that preceded this one, before that fix made it merely
defensive. A duplicate identity would let the run-time identity index
(which keeps the first record) and `BundleFileSource` (which sorts and
binary-searches every record, with no defined tie-break of its own) each
answer for a different record, with nothing outside able to tell which one
answered. The
`--include` dedup tests (one record for a repeated identical `--include`,
and for an `--include` the entry graph already reaches on its own) landed
in the same round, specifically so a dedup regression would surface as a
failing test rather than as an unopenable bundle once the reader started
enforcing uniqueness.

**What the tests cover now that they used to assert past.** The
disk-vs-container agreement test (`BundleFileSourceTest.
AgreesWithTheDiskBackend`) gained fixture shapes that used to have no
discriminating power at all -- a file beside a same-named directory, a
trailing-slash directory specifier, `.ts`/`.json` extension probes, a
nested `node_modules`, an absolute specifier both inside and outside the
root -- plus a companion test that builds a container which is a strict
*subset* of its tree (skipping `.node`/`.mjs`/unrecognized extensions the
way the real producer does) and checks that a skipped file misses cleanly
rather than being picked up by a same-stem packaged neighbour. The
agreement test also gained two hardening passes: a positive check that
every specifier meant to resolve actually resolves from at least one
fixture directory (agreement alone was satisfied just as well by two
`nullopt`s as by two real hits, so a fixture edit that silently broke a
specifier would have gone unnoticed), and an explicit miss assertion for
`"missing"` and for an absolute specifier outside the root, rather than
leaving both to the same agreement check that a vacuous pass could satisfy.
The five hand-copies of the `mkdtemp`-plus-cleanup test fixture (two of
them in the unrelated compile-cache tests) are now one `unittests/
TempTree.h`.

**Two items looked at and deliberately left.** `libjs/loader.js`'s
`probeFile()` has a guard (`if (diskLoadingClosed) return false;`) that
stops it from touching the filesystem once a bundle has closed disk
loading. No test exercises the true branch of that guard, because nothing
reaches `probeFile()` at all while a bundle is running: bundled modules
resolve through the C++ `bundle.resolve()` path, never through this
loader's `resolveRelative()`. The guard is defense in depth for a path
that is unreachable today, so it stays unpinned rather than pinning
something a test cannot actually exercise. Separately, an earlier round's
`COLLIDE` test pinned `mod.path` against a `relativeResolveCache`
collision; that collision is now out of reach, because
`relativeResolveCache` is only ever written on the embedded-vendored (`ws`)
path, not on the general resolution path the old test exercised. The
rewritten test asserts the same property a different way -- keying on
`(importer, request)` instead -- and the `mod.path` assertion specifically
was not restored, since there is no longer a code path that would let it
fail.

**The debug gate stopped re-reading `process.env` per require.** The
`debugBundle` flag in `libjs/bundle-loader.js` used to be recomputed from
`process.env.HERMES_NODE_DEBUG_NATIVE` inside `logOutcome()`, called on
every `require()` and `require.resolve()`,
hit or miss, ahead of the `Module._cache` check. `process.env` is a Proxy
whose get trap is a native callback around `getenv()`
(`lib/process/node_process.cpp`), so that was a proxy trap plus a native
call plus a `getenv()` on the hottest path of an artifact whose whole
reason for existing is startup cost -- paid twice per check, since `&&`
reads the property again. `installBundleLoader()` now reads the variable
once, at install time, and caches the result. The deliberate consequence:
assigning `process.env.HERMES_NODE_DEBUG_NATIVE` from inside a running
bundled program no longer turns the log on or off. This matches the native
side, which also takes the variable from `getenv()` once, at startup
(`lib/runtime/hermes_node_runtime.cpp`), and never rereads it.


## 2026-08-21: the --include hint is unfollowable for one specifier shape

Not fixed. Recorded here because it lived only in a code comment
(`includeSuggestion()` in `libjs/bundle-loader.js`), which is not where
someone hitting it will look.

When a bundled module requires a bare specifier the container cannot
answer, the error suggests the specifier verbatim:

```
Error: Cannot find module 'baz'
  required by node_modules/foo/index.js
  Not in the bundle. Add it with:
    --include=baz
  (--include resolves from the entry's directory.)
```

`--include=baz` then fails with `error: --include=baz cannot be resolved`
whenever the target sits under a nested `node_modules` -- here it is at
`node_modules/foo/node_modules/baz`, and the value that works is
`--include=./node_modules/foo/node_modules/baz`.

The suggestion is not computable. The module did not resolve, so the
loader does not know which of Node's candidate directories would have held
it; only the program's own tree does. The options were to print every
candidate the walk would have tried, which is several lines of mostly
wrong paths, or to state the rule, which is the parenthetical above. The
message is therefore honest but unhelpful in this shape: it tells you
where `--include` resolves from and leaves you to derive the path.

A relative specifier does not have this problem -- `includeSuggestion()`
computes the exact value from the importer's identity, and
`test/bundle-include.js` runs the suggested command to prove it resolves.
The bare case is the one with no correct answer to print.


## 2026-08-21: native addons in a bundle (format v4)

Tracks `docs/superpowers/plans/2026-08-21-bundle-natives-plan.md` and its design doc
`docs/superpowers/specs/2026-08-21-bundle-natives-design.md`. Eleven tasks, all done;
the per-task ledger is in
`.superpowers/sdd/2026-08-21-bundle-natives-plan/progress.md`.

### What shipped

A `.node` addon is no longer skipped by the producer and refused by the
loader. It is packaged as a `kNative` module record with an empty payload,
and its bytes are copied to a flat sidecar file in the bundle's own
directory. `dlopen(3)` takes a path and there is no portable way to load a
shared object out of memory, so the bytes have to be a real file at load
time whatever else is true; the decision was to make that visible -- the
artifact is a container plus N named files, and the producer says so --
rather than extracting to a cache directory at startup, where the
verify-then-`dlopen` gap cannot be closed by any amount of checksumming.

- **Format v4** adds a native table: one
  `BundleNativeRecord {moduleIndex, sidecarString, byteLength, hashString}`
  per `kNative` module, in its own section
  (`nativeTableOffset`/`nativeCount`), keyed by module index.
- Sidecar naming is three passes. An addon that already *is* the file it
  would be copied to claims its plain basename first, so the ordinary
  `proj/binding.node` plus `node_modules/foo/build/Release/binding.node`
  pair costs zero writes rather than one that overwrites a build input;
  everyone else takes the basename or, on collision, a
  `-<crc32 of the identity>` suffix; then destinations are computed. Two
  addons that still want one name, a sidecar that would land on the
  container, and a sidecar that is another addon's source file are all hard
  build errors. The copies run after compilation, so a build that fails
  leaves no sidecar from this run beside a container from the last one.
- The loader builds an identity-to-sidecar map once from `bundle.natives()`
  (`__bundleNatives`) and `dlopen`s `<bundle dir>/<sidecar>`;
  `module.filename` stays the identity path, like every other bundled
  module. `__bundleLoad` refuses a native, since its bytes are not there.
- `--verify-natives` is a fourth read-only verb: it hashes each sidecar and
  compares against the record, printing `OK`/`MISSING`/`MISMATCH` and
  exiting 1 on any failure. It is an audit, not an enforcement -- the run
  path never hashes an addon, and nothing closes the gap between the check
  and the later `dlopen`. SHA-256 rather than the CRC32 the generation tag
  uses, because a CRC can be forged to any value.
- `--dump` grew a `NATIVES` section and a `natives` row in `SECTIONS`;
  `--extract-module` refuses a native by name, since "extracting" it would
  write the empty payload and call it the addon.

### Two findings from the design round

**`MODULE_NOT_FOUND` for a missing addon is continuity, not a new choice.**
The design argued for `MODULE_NOT_FOUND` over `ERR_DLOPEN_FAILED` on the
grounds that the code which exists in the world to handle an unavailable
addon -- an optional-dependency probe, a napi-rs try/catch chain --
branches on it. Checking the code it was replacing showed the argument had
already been made: the old "Native addons are not supported in a bundle
yet" error set `err.code = 'MODULE_NOT_FOUND'` too. So this round did not
pick a code; it kept one, for both the unrecorded-addon case (now the
ordinary not-in-bundle error, with a working `--include` suggestion) and
the recorded-but-missing-sidecar case (`missingSidecar()`, which names the
file to ship). `test/bundle-natives-errors.js` pins `e.code` for both.

**Absolute-path resolution worked only by accident, and is now explicit.**
`require(path.join(__dirname, 'build', 'Release', 'x.node'))` is how
`bindings`, `node-gyp-build` and hermes-parser's own loader all ask for an
addon, so a bundle that packages natives resolves absolute specifiers
constantly. `resolveSpecifier()` had no absolute branch: the bare
`node_modules` walk produced the right answer anyway, because
`joinNormalized()` ends in `fs::path::operator/`, which discards its left
operand when the right one is absolute. Correct behaviour resting on a
detail of a helper that has nothing to do with resolution. There is now an
explicit branch, mirroring what Node does (`Module._findPath` overrides the
lookup paths to `['']` for an absolute request and probes the path itself;
`Module._resolveLookupPaths` has no absolute case at all and hands back a
`node_modules` list `_findPath` then throws away -- the same shape as the
accident). `BundleResolveTest` covers it.

### Measured: `examples/hermes-parser-ast`

New example, the native-addon sibling of `examples/babel-parser/ast.js`:
parse a file with the native Hermes parser addon and print the AST. The
addon is reached through three computed `require()` calls inside
hermes-parser's own loader, so static discovery cannot see it and it is
named with one `--include`. Numbers from a Release build on linux-x64,
2026-08-21:

| Artifact | Bytes |
|----------|-------|
| `ast.hbb` (the container: 37 modules, 61 edges) | 240,560 |
| `hermes-parser.node` (the addon, verbatim, beside the container) | 2,521,344 |

Container sections: strings 3,403 B, modules 740 B, edges 732 B, payload
235,600 B, natives 16 B. The bundled AST is byte-identical to the
unbundled one, and `--verify-natives` exits 0:

```
OK       hermes-parser.node       (node_modules/hermes-parser/prebuilds/linux-x64/hermes-parser.node)
    expected 2521344 bytes sha256:4f9e454bf80ac5122ba6333ee539cdf84b8ba47f5e518fd8e3245b0393c4147b
    actual   2521344 bytes sha256:4f9e454bf80ac5122ba6333ee539cdf84b8ba47f5e518fd8e3245b0393c4147b
```

### Measured, because the design only predicted it: bufferutil falls back

The design's "What does not work" section claims `examples/bufferutil-addon`
silently falls back to its pure JavaScript `./fallback` in a bundle, because
`node-gyp-build` stats a directory before it requires. That was a reading of
the package, not an observation, so it was run.

**It does fall back, silently.** But the mechanism is not the one the design
described, and which half fails depends on where the bundle sits.

Nothing packages the addon at all: `bufferutil/index.js` is
`try { module.exports = require('node-gyp-build')(__dirname) } catch (e) { module.exports = require('./fallback') }`,
no `.node` appears as a literal specifier anywhere, and the example is built
with no `--include`. So the build prints no `native:` line and writes no
sidecar -- an 11,448-byte container and nothing else.

With the bundle at its printed root and the source tree still on disk beside
it (`--build-bundle=examples/bufferutil-addon/bu.hbb` then `--bundle` of the
same path), `HERMES_NODE_DEBUG_NATIVE=BUNDLE` shows what actually happens:

```
[bundle] edge: bufferutil from mask.js -> node_modules/bufferutil/index.js
[bundle] edge: node-gyp-build from node_modules/bufferutil/index.js -> node_modules/node-gyp-build/index.js
[bundle] miss: .../node_modules/bufferutil/package.json from node_modules/node-gyp-build/node-gyp-build.js
[bundle] miss: .../node_modules/bufferutil/prebuilds/linux-x64/bufferutil.node from node_modules/node-gyp-build/node-gyp-build.js
[bundle] edge: ./fallback from node_modules/bufferutil/index.js -> node_modules/bufferutil/fallback.js
```

node-gyp-build's `readdirSync` reached the **real disk** and found the real
prebuild, so `load.resolve()` returned a real absolute path and did not
throw. What threw was the `require()` of that path, refused by the closed
world. The design's sentence -- "that check fails against a path that
genuinely is not there, and the `require` interception never gets a turn" --
is right only when the source tree is gone. When it is present, the
interception gets exactly one turn, and takes it.

The program then runs to completion on the fallback and prints `PASS`. Worse
for anyone debugging this: `mask.js`'s own self-check calls
`require('node-gyp-build').path(...)` and asserts the result ends in
`.node`. That check **passes and prints the real prebuild path**, while the
module actually loaded is the JavaScript fallback. A `require.resolve`-shaped
probe cannot tell you which one you got.

Move the bundle away from the source tree (`--build-bundle=/tmp/bu.hbb`,
then `--bundle=/tmp/bu.hbb`, which also violates the "bundle must sit at the
printed root" rule and so re-roots the identities at `/tmp`) and the other
half fails instead: `readdirSync` finds nothing, node-gyp-build throws its
own error, the same catch fires, the same fallback loads --

```
bufferutil loaded, exports: [ 'mask', 'unmask' ]
Error: No native build was found for platform=linux arch=x64 runtime=node abi=undefined uv=1 libc=glibc node=24.13.0
    loaded from: /tmp/node_modules/bufferutil
```

-- and the failure is the example's own assertion, which re-runs the search
outside bufferutil's try/catch. `require('bufferutil')` itself succeeded in
both runs.

The design doc's claim was corrected in place to match this.

### Limitations recorded rather than fixed

- **A loader that stats before it requires does not find a flat sidecar.**
  `node-gyp-build` and `node-pre-gyp` `readdirSync`/`existsSync` a candidate
  directory and only then require the winner. The escape hatch costs no
  code: additionally place the real file beside the bundle, at
  `<bundle dir>/<identity>`, and the stat succeeds -- flat is the contract,
  the tree remains available to whoever needs it.
- **`fs` was not taught to answer for recorded native identities.** It is
  small and purely additive, and it was deliberately deferred: it is a
  half-truth discoverable in one line of user code, since `existsSync`
  would say yes where `readFileSync` on the same path says no.
- **Nothing hashes an addon on the load path.** The digest exists for
  `--verify-natives` alone; hashing at load would read the whole addon on
  every launch, and would still not close the TOCTOU gap it looks like it
  closes.
- **`--preload=<x>.node` is not refused.** It works through the existing
  preload machinery, and a special case forbidding something harmless would
  cost code and buy nothing.
- **A sibling shared library an addon pulls in is still the user's job.**
  Nothing `require()`s it, so nothing discovers it. Flat placement does make
  this better rather than worse: every native lands in one directory, so an
  `$ORIGIN`-relative RPATH finds a sibling dropped beside the bundle.
