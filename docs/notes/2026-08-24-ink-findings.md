# Note: Ink does not run, and the more interesting reason why

**Status:** findings, 2026-08-24. Nothing here proposes work. Written after
asking whether Ink -- React for terminal UIs -- could be a `--build-exe`
demo. It cannot, on either of its two branches, for unrelated reasons, and
one of those reasons is worth more than the question was.

Measured on Linux x86_64 against `hermes-node` at `26e2dc1`, with
node v24.13.1 as the control.

## Short answer

| | version tested | blocked by |
| --- | --- | --- |
| Ink 4 and later | 7.1.1 (current latest) | ESM, **and** WebAssembly |
| Ink 3 | 3.2.0 (last CommonJS release) | `eval` does not capture local scope |

The first row is expected to clear itself: WebAssembly is in progress and
ESM is planned. **The second is not** -- it is an engine restriction rather
than a gap in this compatibility layer.

So the forward guidance is: **do not chase Ink 3.** When WASM and ESM land,
re-check `ink@latest` directly. It does not need `eval`, because it dropped
the emscripten layout engine for a WebAssembly one -- the same change that
blocks it today is what will make it work later.

## Ink 4+ : ESM and WebAssembly

`ink@7.1.1` is `"type": "module"`, which the CJS loader cannot run. It
depends on `yoga-layout ~3.2.1`, which ships
`dist/binaries/yoga-wasm-base64-esm.js` -- the layout engine as
base64-encoded WebAssembly. And:

```
$ hermes-node -e 'console.log("WebAssembly:", typeof WebAssembly)'
WebAssembly: undefined
```

Two independent blockers, either sufficient on its own.

## Ink 3 : the emscripten `eval` problem

`ink@3.2.0` is CommonJS and its layout engine, `yoga-layout-prebuilt@1.10.0`,
is pure JavaScript -- emscripten asm.js output, no WebAssembly. It should
have worked. It fails at load:

```
$ hermes-node -e 'require("yoga-layout-prebuilt").Node.create()'
ReferenceError: Property 'HEAPU32' doesn't exist
    at anonymous (:1:27)
    at _nbind_value (.../yoga-layout/build/Release/nbind.js:1038:110)
```

The frame with no filename (`:1:27`) is generated code. nbind builds its
native-call shims at run time with **direct `eval`**:

```js
// nbind.js:1445
var sourceCode = "function(" + argList.join(",") + "){" + ... + "}";
return eval("(" + sourceCode + ")");
```

That only works if `eval` can see the enclosing scope, because the generated
body references module locals -- `HEAPU32`, `dynCall`, and the rest of the
emscripten heap views. Hermes's `eval` cannot:

```js
function f() { var x = 42; return eval("x"); }
function g() { var y = 7; return eval("(function(){ return y; })")(); }
globalThis.z = 5;
function h() { var w = 1; return new Function("return typeof w")(); }
```

| | hermes-node | node v24.13.1 |
| --- | --- | --- |
| `f()` -- direct eval reads a local | **ReferenceError: Property 'x' doesn't exist** | `42` |
| `g()` -- eval-built closure over a local | **ReferenceError: Property 'y' doesn't exist** | `7` |
| `eval("z")` -- a global | `5` | `5` |
| `h()` -- `new Function` reads a local | `undefined` | `undefined` |

Hermes's `eval` behaves as **indirect** eval: globals only. The last row is
correct in both -- `new Function` is specified to see only the global scope,
and does.

This is an engine restriction, not something the compatibility layer can
paper over. Capturing local scope is what a register-allocating compiler
cannot cheaply support, and Hermes does not.

**Precision about what was and was not established:** the failure is at
`yoga-layout-prebuilt` load, before Ink initialises. So Ink 3 is *blocked*;
nothing here shows it would otherwise work.

## The part that outlives the question

The `eval` limitation is not about Ink. It breaks **any emscripten or asm.js
output that generates code with direct `eval`**, which is how nbind and
several other C++/JS bridges of that era work. That is a large family: a C
or C++ library compiled to JavaScript, from before WebAssembly was the
default target.

Both routes into that family are closed right now -- old ones by `eval`, new
ones by the missing `WebAssembly`. WASM landing opens the newer route, which
is the one that matters, since anything still shipping emscripten asm.js in
2026 is doing so for browser-support reasons that a server-side runtime does
not share.

It is also worth knowing that this has nothing to do with bundling. It fails
identically from disk, from a bundle, and from an executable, because it is
the engine underneath all three.

## Reproducing

```sh
mkdir /tmp/inktest && cd /tmp/inktest && npm init -y
npm install ink@^3 react@^17
hermes-node -e 'require("yoga-layout-prebuilt").Node.create()'   # ReferenceError
node       -e 'require("yoga-layout-prebuilt").Node.create()'    # fine
```

## What has no demo, as a result

There is no React-for-the-terminal option: Ink is effectively the category,
and every version of it is blocked. A TUI demo has to use `blessed` or
`terminal-kit`, which is where `examples/tetris` and `examples/gtop` already
are.
