# Note: Ink does not run, and the more interesting reason why

**Status:** findings, 2026-08-24. **Partly superseded 2026-09-11 -- see
"Update" below: Ink 5 now renders.** Nothing here proposes work. Written
after asking whether Ink -- React for terminal UIs -- could be a
`--build-exe` demo. It cannot, on either of its two branches, for unrelated
reasons, and one of those reasons is worth more than the question was.

Measured on Linux x86_64 against `hermes-node` at `26e2dc1`, with
node v24.13.1 as the control.

## Update, 2026-09-11: Ink runs, up to 6.4.0

The second half of the prediction below came true. WebAssembly landed, and
with it `yoga-layout@3.2.1` runs here -- its wasm produces layout identical
to node's. Transpiled to CommonJS, **Ink renders under hermes-node,
byte-identical to the same program under node.**

**The newest version that runs is `ink@6.4.0`.** The wall is not Ink itself
but a dependency bump:

| ink | string-width | here |
| --- | --- | --- |
| 5.2.1 | ^7.2.0 | runs |
| 6.0.0 - 6.4.0 | ^7.2.0 | runs |
| 6.5.0 - 6.8.0 | ^8.1.x | **no** |
| 7.0.0 - 7.1.1 (latest) | ^8.2.0 | **no** |

`string-width@8` matches graphemes with the ES2024 `v` regex flag, and one
of its patterns is `/^\p{RGI_Emoji}$/v`. `RGI_Emoji` is a *property of
strings*, which only `v` can express -- so unlike the other four `/v`
regexes in that bundle it cannot be rewritten to `/u`. Hermes does not
implement `v`, and because this is a parse error the whole bundle fails to
compile before anything runs:

    SyntaxError: Invalid regular expression: Invalid flags

So Ink 6.5 and later are blocked on a regex flag, not on anything about
terminals, React or WebAssembly.

What that took, none of it an engine limitation:

- **ESM -> CJS** with esbuild (`--bundle --platform=node --format=cjs`), the
  same shape `examples/ditz2/build-cjs.sh` already uses.
- **`yoga-layout`'s top-level await.** Its default entry is
  `const Yoga = wrapAssembly(await loadYoga())`, which CJS cannot express.
  The package also exports `yoga-layout/load`, the same object behind an
  async function, so aliasing the import to a small shim that initialises
  once and hands back a lazy proxy is enough. Ink only touches Yoga inside
  functions, never at module scope, which is what makes the proxy viable.
- **Ink's own top-level await**, `await import('./devtools.js')` in
  `reconciler.js`, under `if (process.env['DEV'] === 'true')`. Dead in
  production; dropping the `await` is a one-line build-time patch, and
  `react-devtools-core` goes external.

**Two globals are missing, and both are cheap to stub.** `Intl` first: This build is
`HERMES_ENABLE_INTL=OFF`, and `string-width` does `new Intl.Segmenter()` at
module scope purely to walk graphemes, so Ink dies on import with
`ReferenceError: Property 'Intl' doesn't exist`. A ten-line
code-point-granular stub was enough to get through it, and **nothing else
was behind it** -- the render succeeded immediately after. A real
integration needs either a proper `Intl.Segmenter` polyfill or an Intl-
enabled build; whether Hermes can be built with Intl on Linux was not
investigated.

And `performance`, which node exposes as a global and this runtime does not,
along with `perf_hooks`. Ink 5 does not touch it; Ink 6.4 compiles, starts,
and dies at its first render inside its own `onRender` timing.
`performance.now = () => Date.now()` was enough. Filed as `01a08e1d-d4e5`,
because a missing global that stops a library at its first frame is worth
more than its size suggests.

Ink 6.5+ and 7.x carry two more top-level awaits than 5 does -- a
`loadPackageJson()` and one inside `devtools.js` -- but both sit in the same
dead `process.env['DEV'] === 'true'` branch and patch out the same way. They
are not what blocks those versions; `string-width@8` is.

Ink 3's verdict below is unchanged and still correct: do not chase it.

Measured on Linux x86_64 with a Release `hermes-node` at `0ad2400`, node
v24.13.1 as the control, esbuild 0.28.2, against `ink@5.2.1` + `react@18.3.1`
and `ink@6.4.0` + `react@19.3.0`, both on `yoga-layout@3.2.1`. `ink@6.8.0`
and `ink@7.1.1` were built and failed to compile, as above.

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

*(As of 2026-08-24. See the update at the top: Ink 5 renders now.)*

There is no React-for-the-terminal option: Ink is effectively the category,
and every version of it is blocked. A TUI demo has to use `blessed` or
`terminal-kit`, which is where `examples/tetris` and `examples/gtop` already
are.
