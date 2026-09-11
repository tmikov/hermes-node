# unicode-segmenter (grapheme-only subset)

## What this is

A trimmed vendored copy of [`unicode-segmenter`](https://github.com/cometkim/unicode-segmenter),
used to implement `Intl.Segmenter` at grapheme granularity. See the top-level
`README.md`'s Intl section for why this exists: Hermes is built here with
`HERMES_ENABLE_INTL=OFF`, so `Intl` does not exist natively, and this package
backs the one member of it this repository installs.

## Provenance

- **Upstream:** https://github.com/cometkim/unicode-segmenter
- **Package:** `unicode-segmenter`
- **Version:** `0.17.3`
- **License:** MIT (`LICENSE` in this directory, copied unmodified)
- **Copyright:** Hyeseong Kim

## Only four files, and why

The full package also implements word and sentence segmentation (`general.*`,
`utils.*`) and extended-pictographic emoji sequence detection used by those
(`emoji.*`, `_emoji_data.*`, `_general_data.*`). `Intl.Segmenter` here only
ever constructs the grapheme segmenter -- `word` and `sentence` granularity
throw `TypeError` by design (see the top-level README) -- so none of that is
reachable and vendoring it would roughly double the size for nothing.

Following the actual `require()` graph from the adapter this repository
calls into:

```
intl-adapter  -->  grapheme  -->  core
                            \\--> _grapheme_data
```

gives exactly four files: `intl-adapter`, `grapheme`, `core`,
`_grapheme_data`.

## The extension rewrite

Upstream ships every module twice: `<name>.js` (ESM, `require`-free, using
`import`/`export`) and `<name>.cjs` (CommonJS, using `require`/`exports`).
This repository's vendoring mechanism
(`lib/embedded-modules/CMakeLists.txt`) globs a vendored package for `*.js`
files and CJS-wraps each one with the standard `(function(exports, require,
module, __filename, __dirname) {...})` wrapper -- so taking the upstream
`.js` files verbatim would embed ESM source inside a CommonJS wrapper and
fail to compile.

The fix taken here: the four **`.cjs`** files were copied and renamed to
`.js`, and their internal `require("./x.cjs")` specifiers were rewritten to
`require("./x.js")` to match (the module loader resolves by the renamed
extension, not the original one). That is the only change made to the
source text -- diffing a vendored file here against the matching `.cjs` file
in an upstream checkout shows nothing else different. Compare:

- `intl-adapter.js` vs. upstream `intl-adapter.cjs`:
  `require("./grapheme.cjs")` -> `require("./grapheme.js")`
- `grapheme.js` vs. upstream `grapheme.cjs`:
  `require("./core.cjs")` -> `require("./core.js")`,
  `require("./_grapheme_data.cjs")` -> `require("./_grapheme_data.js")`
- `core.js` vs. upstream `core.cjs`: byte-identical (no internal requires)
- `_grapheme_data.js` vs. upstream `_grapheme_data.cjs`: byte-identical (no
  internal requires)

`package.json` here is hand-written, not copied from upstream: it keeps only
what the vendoring mechanism reads (`main`, pointing at `intl-adapter.js` so
a generic `require('unicode-segmenter')` gets the same `{ Segmenter }` shape
`globalThis.Intl.Segmenter` is installed from) plus a few descriptive fields
for documentation. It is not the full upstream manifest.

## How to re-sync

To pick up an upstream version bump:

1. `npm pack unicode-segmenter@<version>` (or fetch the four files directly
   from the tagged commit) and get `intl-adapter.cjs`, `grapheme.cjs`,
   `core.cjs`, `_grapheme_data.cjs`.
2. Re-check the require graph is still exactly these four files -- if
   upstream restructures `grapheme.cjs` or `intl-adapter.cjs` to pull in
   `utils.cjs`, `emoji.cjs` or the data files behind them, those need
   vendoring too.
3. Copy each `.cjs` file to the matching `.js` name here and rewrite its
   internal `require("./x.cjs")` specifiers to `require("./x.js")`, exactly
   as described above. Nothing else in the file should change.
4. Update `LICENSE` if upstream's changed, and bump `version` in
   `package.json`.
5. Reconfigure (`cmake -B cmake-build-asan`) so the CMake glob (which runs at
   configure time) picks up any file additions or removals, then rebuild and
   run `test/test-intl-segmenter.js`.
6. Update the version and this file, and commit the result.
