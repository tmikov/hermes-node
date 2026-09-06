# hermes-parser-ast-wasm

Parses a file with the WebAssembly build of the Hermes parser
(`hermes-parser`) and prints its ESTree AST as JSON, run from disk and then
from an AOT bundle.

```sh
npm install
../../cmake-build-release/bin/hermes-node ast.js sample.js
./run.sh                      # everything below, checked
./build-bundle.sh             # the AOT bundle, kept in ./dist
```

| file | what it is |
| --- | --- |
| `ast.js` | reads a file named by argument, parses it with `hermes-parser`, prints the AST |
| `sample.js` | a small file with a class private field and an optional chain, so the AST assertion is specific to this input |
| `build-bundle.sh` | builds the bundle; `run.sh` calls it too |

Requires a `hermes-node` built with WebAssembly. That is the default, but a
build directory created before it became the default keeps its cached `OFF`;
`run.sh` checks and says so rather than failing inside emscripten glue.

## The point of it

Three examples in this tree parse a file and print an ESTree AST. They
differ in what the parser is, and therefore in what a bundle of them holds:

| example | parser | bundle |
| --- | --- | --- |
| `babel-parser` | pure JavaScript | one file |
| `hermes-parser-ast` | native `.node` addon | one file **+ a sidecar `.so`** |
| `hermes-parser-ast-wasm` | WebAssembly | one file |

The first and third look alike from outside and are not alike inside. This
one carries a 665 KB wasm module, base64-encoded into an ordinary JavaScript
file, which hermes-node compiles to Hermes bytecode at run time -- so the
artifact is a single file the way the pure-JavaScript one is, while the code
doing the parsing is the same C++ the native addon runs.

That makes the packaging story the exact inverse of the native sibling's.
There, `hermes-parser`'s loader reaches its `.node` through three computed
`require()` calls that no static scanner can follow, so the addon must be
named with `--include` and travels beside the container as a sidecar. Here
the producer sees nothing but a literal `require()` of a large JavaScript
file: no `--include`, no staging step, no warnings, and nothing to verify
afterwards. `run.sh` asserts the difference rather than describing it --
that the output directory holds exactly one file, and that the container
records no native addons.

## The cross-check

When `examples/hermes-parser-ast` is also installed, `run.sh` finishes by
parsing `sample.js` with both parsers and diffing the two ASTs. They are
byte-identical, and that is the assertion worth having: the two vendored
parsers are meant to be interchangeable, and nothing else in the tree checks
it end to end. It is skipped, not failed, when the sibling is not installed,
because `examples/` stays usable offline.

## Which parser this actually is

`hermes-parser` here resolves to `external/hermes-parser-wasm/package`, the
vendored wasm build, not the published npm package -- for the reasons in
that directory's README, the short version being that the wasm binary
carries a serializer fix that npm's 0.37.0 predates and that this
repository's `hermes/` submodule does not have either.

The parse itself is not fast. Compiling the module costs several seconds on
the first `parse()` call, because Hermes compiles wasm ahead of time to
bytecode rather than tiering it up lazily, and nothing caches the result
between runs. That is a real property of running wasm here and this example
is a good place to notice it; the native sibling is the comparison that
makes it legible.
