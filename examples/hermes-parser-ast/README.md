# hermes-parser-ast

Parses a file with the native Hermes parser addon (`hermes-parser`) and
prints its ESTree AST as JSON, run from disk and then from an AOT bundle.
It is the native-addon sibling of `examples/babel-parser/ast.js` -- same
job, one argument, JSON on stdout -- and exists to prove the AOT bundle
format's native-addon support end to end on a real package.

```sh
npm install
../../cmake-build-release/bin/hermes-node ast.js sample.js
./run.sh                      # everything below, checked
./build-bundle.sh             # the AOT bundle + its sidecar, kept in ./dist
```

| file | what it is |
| --- | --- |
| `ast.js` | reads a file named by argument, parses it with `hermes-parser`, prints the AST |
| `sample.js` | a small file with a class private field and an optional chain, so the AST assertion is specific to this input |
| `build-bundle.sh` | stages the addon, builds the bundle; `run.sh` calls it too |

## Why this one and not the babel sibling

`hermes-parser`'s own loader
(`external/hermes-parser-native/package/dist/HermesParserAddon.js`) reaches
its `.node` through three computed `require()` calls tried in a
`try`/`catch` loop over candidate paths:

```js
const override = process.env.HERMES_PARSER_NATIVE_ADDON;
if (override != null && override !== '') {
  return require(path.resolve(override));
}
const devBuildPath = path.join(__dirname, ..., 'hermes-parser.node');
const prebuiltPath = path.join(__dirname, '..', 'prebuilds', target, 'hermes-parser.node');
for (const candidate of [devBuildPath, prebuiltPath]) {
  try { return require(candidate); } catch (e) {}
}
```

None of those three are string literals, so the `require()` scanner cannot
follow any of them -- the addon is invisible to static discovery and has to
be named at build time. The `try`/`catch` means one candidate (the
development-build path) legitimately misses at run time before the second
one (`prebuiltPath`, an absolute path once `path.join` resolves it) hits.
`@babel/parser`, `ast.js`'s sibling in `examples/babel-parser`, needs none
of this -- it's an ordinary literal `require()`, and its bundle is one
file. This example's bundle is a file plus a shared object, because the
addon it needs ships as a sidecar next to the container rather than inside
it.

## Pinning the addon where the container can record it

`examples/flow-bundler/run.sh` pins the addon with
`HERMES_PARSER_NATIVE_ADDON=<absolute path into the build tree>`. This
example deliberately does **not** do that for the bundled run: a bundle
serves only what its container records, and that override is an absolute
path meaningful on the build machine alone, which no container records.
(`run.sh` also `unset`s it, so a shell that has it exported from
flow-bundler does not turn this into a confusing hard failure -- and then
asserts that a bundled run *with* it set exits non-zero, which is the
demonstration.) Instead, `build-bundle.sh` copies the freshly built addon
into the package's own
`node_modules/hermes-parser/prebuilds/<platform>-<arch>/hermes-parser.node`
-- a path the walk can reach, where `--include` can name it and the
container can record it. The `<platform>-<arch>` pair comes from
`hermes-node` itself (`hermes-node -e 'console.log(process.platform + "-"
+ process.arch)'`), not the host `node`, so the copy matches the runtime
that will load it and the example works on machines the repository's
committed `linux-x64` prebuilt does not fit.

That copy and the `--include` that names it live in `build-bundle.sh`, not
in `run.sh`. `run.sh` calls it with a temporary output directory and checks
what came out, so the bundle a person keeps (`./build-bundle.sh`, which
writes `./dist`) and the bundle the check exercises are built by the same
two flags. `run.sh` runs the build first for a reason that is easy to miss:
staging the addon is a side effect of building, and the from-disk run has
to load that freshly built copy rather than whatever `npm install` left in
`prebuilds/`.

## Measured build output

```
$ hermes-node --build-bundle=out/ast.hbb --verbose \
    --include=./node_modules/hermes-parser/prebuilds/linux-x64/hermes-parser.node \
    ast.js
...
warning: 2 computed require()/require.resolve() calls in 1 file, not packaged:
  node_modules/hermes-parser/dist/HermesParserAddon.js:11:12
  node_modules/hermes-parser/dist/HermesParserAddon.js:20:14
  no target is named: a computed specifier is not knowable until the
  program runs. It resolves at run time only if the container already
  holds it -- name it with --include to be certain.
...
native  node_modules/hermes-parser/prebuilds/linux-x64/hermes-parser.node -> hermes-parser.node
  from .../examples/hermes-parser-ast/node_modules/hermes-parser/prebuilds/linux-x64/hermes-parser.node
  to   out/hermes-parser.node
  2521344 bytes sha256:4f9e454bf80ac5122ba6333ee539cdf84b8ba47f5e518fd8e3245b0393c4147b
modules:    37  (34 js, 2 json, 1 native)
edges:      61  (41 distinct specifiers)
natives:    1 file, 2521344 bytes alongside (not in the container)
strings:    80 entries, 3403 bytes
payload:    235600 bytes
bytecode:   234431 bytes
largest:    node_modules/hermes-parser/dist/HermesParserNodeDeserializers.js  56807 bytes
total:      240560 bytes
compile:    79.99 ms
bundle root: .../examples/hermes-parser-ast
native: hermes-parser.node (from node_modules/hermes-parser/prebuilds/linux-x64/hermes-parser.node)
note: this bundle requires 1 native addon alongside it; ship them together.
```

The bundle is built into `out/` rather than beside `ast.js`, and that is
deliberate: a directory holding nothing but the build's own output is what
lets the check below assert that `out/` contains exactly the container and
its sidecar and no `node_modules`. (Nothing requires a container to sit at
the printed bundle root -- the consumer re-roots identities at the bundle
file's own directory. It would matter only for a bundled module reading an
unpackaged file through `__dirname`, which this example does not do.)

`out/` ends up with exactly two files:

| file | size |
| --- | --- |
| `ast.hbb` (the container: 37 modules' bytecode/JSON, edge table, string pool) | 240,560 bytes |
| `hermes-parser.node` (the addon, verbatim, alongside the container) | 2,521,344 bytes |

The two computed calls the warning counts are `require(devBuildPath)` and
`require(prebuiltPath)` in `HermesParserAddon.js` above (the third,
`require(path.resolve(override))`, only runs when
`HERMES_PARSER_NATIVE_ADDON` is set, which this build does not do).

`--verify-natives` against the built bundle:

```
$ hermes-node --bundle=out/ast.hbb --verify-natives
OK       hermes-parser.node       (node_modules/hermes-parser/prebuilds/linux-x64/hermes-parser.node)
```

exit code 0.

## The AST comparison

`run.sh` runs `ast.js` against `sample.js` from disk, then again from the
bundle, and diffs the two outputs. Both are 999 lines and `diff` reports no
differences -- the bundled run reaches the identical native addon (its
sidecar) and produces the identical AST, byte for byte. `sample.js`'s
private class field and optional chain show up as ESTree's
`PropertyDefinition`/`PrivateIdentifier` and `ChainExpression` nodes (the
form Hermes's own `babel: false` adapter produces), which is what `run.sh`
checks for -- not just "some JSON appeared".

## Two things this example demonstrates that its babel sibling cannot

1. **A computed absolute `require()` of a `.node` file, needing
   `--include`.** `@babel/parser` is a literal `require()`; `hermes-parser`
   reaches its addon only through paths built at run time, which the
   scanner cannot see and the closed world therefore cannot package
   without being told.
2. **An artifact that is two files, not one.** A JS-only bundle like
   `babel-parser/ast.hbb` is fully self-contained in a single file. This
   one needs its sidecar shipped alongside it -- `out/ast.hbb` and
   `out/hermes-parser.node` travel together, and `--verify-natives` is how
   a deployment checks that the sidecar it has is the one the container
   expects.

Measured on one Linux box (`linux-x64`) with a release build; treat sizes
and timings as approximate.
