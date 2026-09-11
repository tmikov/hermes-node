# ink

[Ink](https://github.com/vadimdemedes/ink) -- React for terminal UIs -- with
a small demo TUI (`app.mjs`) built for this example, transpiled to
CommonJS and run under hermes-node, from disk, from an AOT container, and
as a standalone executable.

## What this demonstrates

One artifact exercises most of the runtime at once:

- an AOT container, and a single-file executable built from it;
- a third-party **WebAssembly** module -- `yoga-layout`'s layout engine --
  that arrives base64-encoded inside a JavaScript module rather than as a
  `.wasm` file, which is exactly the shape
  [the Wasm bake feature](../../docs/superpowers/specs/2026-09-09-wasm-bundle-bake-design.md)
  was designed for, and why baking applies here at all (see below);
- `Intl.Segmenter`, vendored, made *visible*: the borders around the CJK
  and emoji rows only line up if grapheme segmentation matches node's;
- `performance`, which Ink calls on its own first render;
- raw-mode TTY input and a real React reconciler.

Until recently no version of Ink ran here at all. Three things changed
that: WebAssembly (`yoga-layout`'s layout engine is Wasm), `Intl.Segmenter`
(text measurement needs it), and `globalThis.performance` (Ink's own render
timing needs it). See
[`../../docs/notes/2026-08-24-ink-findings.md`](../../docs/notes/2026-08-24-ink-findings.md)
for the investigation, including the exact versions and failure modes this
example works around.

```sh
npm install
./build-cjs.sh                                      # transpile to ./dist-cjs
../../cmake-build-release/bin/hermes-node dist-cjs/app.cjs   # run it, q to quit
./build-bundle.sh                                    # container + executable, kept in ./dist
./dist/ink                                           # the binary, on its own
./run.sh                                             # everything above, checked, all three ways
```

| file | what it is |
| --- | --- |
| `app.mjs` | the demo TUI: layout, a timer, `useInput`, `useApp().exit()` |
| `yoga-shim.mjs` | works around `yoga-layout`'s top-level await (see below) |
| `build.mjs` | the esbuild driver: bundles to CJS, drops Ink's own top-level await |
| `build-cjs.sh` | shell wrapper around `build.mjs`, writes `./dist-cjs/app.cjs` |
| `build-bundle.sh` | records and bakes the Wasm compile, builds the container and executable |
| `run.sh` | builds, then checks all three run modes under hermes-node (and node, as a control) |
| `../pty-run.py` | gives a program a real pty, so a TUI can be checked from a script |

## Version pin: `ink` is exactly `6.4.0`, not `^6.4.0`

**Do not widen this to a caret range.** `ink@6.5.0` bumped its `string-width`
dependency to `^8.1.x`, and `string-width@8` matches graphemes with the
ES2024 `v` regex flag. One of its patterns is `/^\p{RGI_Emoji}$/v` --
`RGI_Emoji` is a *property of strings*, expressible only with `v`, so unlike
every other regex in that bundle it has no `/u` equivalent to fall back to.
Hermes does not implement `v`, and because this is a parse error the whole
bundle fails to *compile*, before any of this example's code runs, with a
message naming neither Ink nor `string-width`:

```
SyntaxError: Invalid regular expression: Invalid flags
```

| ink | string-width | here |
| --- | --- | --- |
| 5.2.1 -- 6.4.0 | `^7.2.0` | runs |
| 6.5.0 and later (7.x included) | `^8.1.x` / `^8.2.0` | fails to parse |

`react` is pinned to `^19.3.0`: Ink 6 requires `react >=19.0.0` as a peer
dependency, and 5.x's React 18 stack is not what this example targets.

## The build: two top-level awaits, rewritten in memory

Ink is `"type": "module"`, and hermes-node has no ES module loader yet, so
this example needs a build step -- the same shape as
[`../ditz2/build-cjs.sh`](../ditz2/build-cjs.sh), but esbuild instead of
`tsc`, since there is no TypeScript here to begin with.

A plain `esbuild --bundle --format=cjs` of this dependency graph fails with
two "top-level await is currently not supported" errors, both confirmed by
running the build and reading esbuild's own errors rather than assumed from
the findings note above:

1. **`ink/build/reconciler.js`**: `await import('./devtools.js')`, inside
   `if (process.env['DEV'] === 'true')` -- dead code in production. `build.mjs`
   registers an esbuild `onLoad` plugin that patches this file **in memory**
   and drops just the `await`, leaving the dynamic `import()` itself alone.
   It is not a `sed` on `node_modules`: `npm install` wipes that, and the
   example would silently start failing the moment someone reinstalled. The
   plugin also asserts the exact string it expects is still there, so a
   future Ink version that moves or removes this line fails the build
   loudly instead of shipping something subtly wrong.
2. **`yoga-layout`**'s default entry (`dist/src/index.js`) is itself
   `const Yoga = wrapAssembly(await loadYoga());` -- CommonJS cannot express
   this at all, in memory or otherwise. `yoga-shim.mjs` (aliased in for the
   bare `yoga-layout` specifier via esbuild's `--alias`) reaches the same
   object through `yoga-layout/dist/src/load.js`'s async `loadYoga()`
   instead, imported by relative path rather than through the
   `'yoga-layout/load'` specifier -- aliasing a package name also rewrites
   that package's own subpaths, so importing the subpath specifier here
   would resolve back to the shim itself and recurse. The shim exposes a
   `Proxy` as the default export and an `initYoga()` function; `app.mjs`
   awaits `initYoga()` before calling `render()`. This works because Ink
   touches `Yoga` only inside function bodies -- event handlers, layout
   callbacks -- never at module scope, which was re-verified for 6.4.0
   rather than assumed from the note.

`--external:react-devtools-core` is also needed: it is only reachable from
the dead `DEV` branch above, is a devDependency of Ink's that this example
does not install, and esbuild would otherwise fail trying to bundle it.

The producer itself emits exactly three warnings when packaging
`dist-cjs/app.cjs` -- `bufferutil` and `utf-8-validate` (optional native
accelerators `ws` probes for with its own `try`/`catch`, from deep inside
Ink's dead `devtools.js` import) and `react-devtools-core` again (the
external above). All three are unresolvable, left to the run-time loader,
and correct: that is the designed behavior for an optional-dependency
probe, not something to silence. `run.sh` asserts there are exactly these
three and no others.

## The AOT container: baking the Wasm compile

`yoga-layout`'s layout engine loads as soon as the bundle's module graph
loads (see the shim section above), which means a bundled or executable
run compiles it on every launch unless that compile is baked into the
container ahead of time -- the feature designed in
[`2026-09-09-wasm-bundle-bake-design.md`](../../docs/superpowers/specs/2026-09-09-wasm-bundle-bake-design.md).
`build-bundle.sh` always uses it: it records a real run's Wasm compile with
`--record-wasm`, then bakes that recording into the container with
`--build-bundle --bake-wasm`, and builds the executable from the *baked*
container so it inherits the entry for free.

Measured on this exact container, both sides with `--no-compile-cache` --
the shipped-artifact case, where no warm disk cache exists to fall back
on, which is the case that actually matters: a produced executable lands
on a machine hermes-node has never run on before, so the baked figure is
the one a real user sees, not the disk-cache-warm figure a developer sees
on their second run:

```
plain --build-bundle                    ->  950,712 bytes
  run: 0.46s   trace: "wasm container miss", then "wasm store"

--record-wasm, then --build-bundle --bake-wasm:
  wasm: 31d7f3c29a1c86fb  257,200 bytes  ->  1,207,952 bytes
  run: 0.09s   trace: "wasm container hit", no store
```

A baked container is about 5x faster to start on a cold cache, at the cost
of roughly 257 KB. `run.sh` proves the hit rather than the size: it asserts
`wasm container hit` with no following `wasm store`, from
`HERMES_NODE_DEBUG_NATIVE=COMPILE_CACHE` tracing -- never from the timing
numbers above, which are context for this README and not a test condition,
matching this suite's own rule for the compile cache.

## What the demo does

A bordered box with a live tick counter and a grapheme-segmentation
exhibit: a ZWJ family emoji, a skin-tone-modified emoji, and a run of CJK
text, each in its own auto-sized inner box. Ink measures box width with
`string-width`, which walks *grapheme clusters* via `Intl.Segmenter` --
get that wrong (count UTF-16 code units, or code points instead of
clusters) and a multi-code-point emoji is measured as several characters
instead of one, so its box's border lands in the wrong place. `run.sh`
diffs this exhibit against the same program run under node; a segmentation
regression would show up there as a text difference, not merely a
subjective-looking misalignment.

`q` quits, cleanly, through `useApp().exit()`; nothing else does. `run.sh`
uses that specifically to distinguish a real keypress from a multi-character
paste -- see the next section.

## Checking a TUI from a script

Same tool as `tetris` and `gtop` next door: `../pty-run.py` gives the
program a real pty, since raw-mode input does not exist on a pipe. One
detail cost real time to learn: `--send KEYS` delivers its whole argument as
a **single** keypress, not one event per character. `--send 'q'` alone
triggers Ink's `input === 'q'` handler; `--send 'abcq'` does not, because the
string `"abcq"` is never equal to `"q"`. `run.sh` uses exactly that
distinction to prove the handler is character-exact rather than a substring
check, and to prove the process keeps re-rendering (the tick counter keeps
advancing) when nothing tells it to quit.

`run.sh`'s checks run as separate captures rather than one long one, and the
reason is a real, if not very interesting, timing hazard: Ink calls
`stdin.setRawMode(true)` from a `useEffect`, which only runs after the first
frame is already written -- so a keystroke sent the moment output starts can
race the terminal driver's own echo of it into the *middle* of that frame,
at whatever byte offset a write happened to be in flight. Under hermes-node
this can even land inside a multi-byte UTF-8 sequence, because hermes-node's
stdio writes to a TTY are queued rather than synchronous (see CLAUDE.md).
That is a genuine difference in how the two engines flush output, and it is
not what this example is trying to demonstrate, so the segmentation
comparison sends no keystroke at all -- removing the race -- and the
keypress checks only grep for a fixed marker (`EXITED CLEANLY`), which does
not care where in the stream a stray echoed key landed. Filed as
[`dz 01a08eac-0d4b`](../../dz/issues/01a08eac-0d4b-72ad-815a-46bfcb4e54a3.md),
which covers the general case -- anything drawing a frame to a raw-mode
terminal is exposed, not just Ink -- so the two records do not drift apart.

## Sizes

Measured on Linux x86_64, Release:

| | size |
| --- | --- |
| `node_modules` | 34 MB, ~40 packages |
| `dist-cjs/app.cjs` | 1.5 MB, one file |
| `dist/ink.hbb` (baked) | 1.2 MB |
| `dist/ink` (executable, baked) | 15.8 MB |

## What is not here

No `Intl.Segmenter` or `performance` polyfill, on purpose -- both are real
runtime features now, and adding a shim for either would be a regression to
notice, not a fix to make. No standalone `.wasm` data file and no
bytecode-only container entry either: `yoga-layout`'s module ends up in the
container twice (its source, inside compiled JavaScript, and its baked
bytecode) rather than once behind a token -- that third step is explicitly
out of scope for the bake feature this example exercises; see the design
doc linked above.
