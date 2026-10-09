# hermes-parser-ast-wasm

Parses a file with the WebAssembly build of the Hermes parser
(`hermes-parser`) and prints its ESTree AST as JSON, run four ways: from
disk, from an AOT bundle, as a `build-native` executable whose WebAssembly
module is compiled to native code, and -- when the sibling example is
installed -- against the native addon build of the same parser.

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

## The native build

`run.sh` also builds the program with `hermes-node build-native`, when the
build directory has a kit with `shermes` in it, and skips that arm
otherwise. It is two steps, because the producer never runs the program and
so cannot see the module: hermes-parser decodes it from a string at run
time. A recording run sees it:

```sh
mkdir -p out-native
hermes-node --no-compile-cache --record-wasm=out-native/ast.wasmrec ast.js sample.js
hermes-node build-native ast.js -o out-native/ast \
  --bake-wasm=out-native/ast.wasmrec --kit=<build dir>/kit
```

The build compiles the recorded module with `shermes` and prints one
`wasm: native <digest> (768303 bytes)` line for it. `run.sh` then runs the
executable with `node_modules` moved away and the compile cache off, and
checks three things: the AST is byte-identical to the unbundled run, the
module was served by its native unit (`wasm native hit` under
`HERMES_NODE_DEBUG_NATIVE=COMPILE_CACHE`), and nothing warned -- a warning
there would be the artifact saying it fell back to bytecode.

This arm is slow to build: the module becomes about 33 MB of C, and `cc`
spends a few minutes on it, alone, at the kit's optimization level.
`--bytecode-wasm` builds the same artifact with the module baked as
bytecode instead. The progress file
`docs/superpowers/plans/progress-native-wasm.md` has the measured sizes and
times of both.

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

The parse itself is not fast from disk. Compiling the module costs several
seconds on the first `parse()` call, because Hermes compiles wasm ahead of
time to bytecode rather than tiering it up lazily. The disk compile cache
removes that cost from the second run on, and the `build-native` executable
never pays it, because its module is already native code. That is a real
property of running wasm here and this example is a good place to notice
it; the native sibling is the comparison that makes it legible.
