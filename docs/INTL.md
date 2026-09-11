# Intl Support

This page documents what `Intl` actually is in `hermes-node` today. It is
current documentation, not a design record -- see the note at the top of
`docs/README.md` about the rest of `docs/`; this page is one of the
exceptions, alongside `DESIGN.md` and `DEBUGGER.md`, because it describes
present behavior a program depends on rather than a plan for how something
was going to be built.

## The short version

`typeof Intl === 'object'` is true. `Intl.Segmenter` exists and works, at
grapheme granularity. Everything else on `Intl` is `undefined` --
`Intl.NumberFormat`, `Intl.DateTimeFormat`, `Intl.Collator`,
`Intl.PluralRules`, `Intl.RelativeTimeFormat`, `Intl.ListFormat`,
`Intl.DisplayNames`, `Intl.Locale`, `Intl.getCanonicalLocales`,
`Intl.supportedValuesOf` -- none of it is implemented, and none of it is
stubbed to look implemented.

## Why Intl is missing at all

Hermes is built here with `HERMES_ENABLE_INTL=OFF`. Turning it on is not an
option on Linux: Intl support in Hermes is not solid enough there to ship.
Without the flag, a stock Hermes build has no `Intl` global whatsoever.

## Why `Segmenter` and nothing else

`Intl.Segmenter` was added as a targeted exception because of what sits on
top of it, not because it was the easiest piece to implement. `string-width`
-- which most of the terminal/CLI ecosystem depends on transitively --
constructs `new Intl.Segmenter()` at **module scope**, to walk grapheme
clusters. `@alcalzone/ansi-tokenize` calls it directly too. A missing
`Intl.Segmenter` does not fail where the code tries to use it; it fails the
moment the module is `require`'d, before any of the module's own code runs.
That made it categorically different from a formatting API a caller can work
around: there was no call site to patch, no feature-detection branch to add,
just an import that throws. This was the last engine-shaped gap standing
between `hermes-node` and running Ink (React for terminal UIs).

Nothing else in `Intl` currently has that shape of dependency, so nothing
else is implemented.

## What `Intl.Segmenter` actually does

Only grapheme-cluster segmentation:

```js
const segments = [...new Intl.Segmenter().segment(str)];
```

It is exact against native ICU segmentation on every case that separates a
correct implementation from a plausible one, verified by hand against a
Node build with real `Intl` (see `test/test-intl-segmenter.js` for the
pinned assertions):

- A four-person ZWJ family emoji (`\u{1F468}` + ZWJ + `\u{1F469}` + ZWJ +
  `\u{1F467}` + ZWJ + `\u{1F466}`) is **one** grapheme cluster, not four
  emoji stitched together by joiner codepoints.
- A regional-indicator flag pair (e.g. the US flag,
  `\u{1F1FA}\u{1F1F8}`) is **one** cluster.
- An emoji plus a skin-tone modifier (e.g. `\u{1F44B}\u{1F3FD}`) is **one**
  cluster.
- Latin combining marks attach to their base letter: `"e" + U+0301` and
  `"o" + U+0308` segment as `["é", "ö"]` -- two clusters, not four
  codepoints.
- The Devanagari conjunct "अनुच्छेद" segments into
  its four akshara clusters (`["अ", "नु", "च्छे", "द"]`), which is the case
  that trips up the two most commonly reached-for pure-JS grapheme
  splitters, `grapheme-splitter` and `graphemer` -- both get it wrong.

A codepoint-splitting implementation (`for (const ch of str)`) fails every
one of those cases, which is the point of pinning them: they discriminate a
real implementation from one that merely returns *something*.

Measured end to end against `string-width@7.2.0` (unmodified, from npm),
bundled to CommonJS with esbuild so it can run here (it ships ESM-only) and
compared against the same bundle under a real Node build:

| input | Node (`Intl` native) | hermes-node (this Segmenter) | codepoint-splitting stub |
|---|---|---|---|
| 4-person ZWJ family emoji | 2 | 2 | 8 |
| US flag (regional-indicator pair) | 2 | 2 | 4 |
| "अनुच्छेद" | 4 | 4 | 8 |

The "codepoint-splitting stub" column is what a hand-written
`for...of`-based `Intl.Segmenter` stand-in reports for the same three
inputs -- the shape of shim this replaces, kept here to show what silently
wrong output looks like next to the real numbers.

`word` and `sentence` granularity are not implemented, and asking for them
does not silently fall back to grapheme segmentation -- that would produce
a plausible-looking but wrong answer, which is worse than an error:

```js
new Intl.Segmenter('en', {granularity: 'word'});     // throws TypeError
new Intl.Segmenter('en', {granularity: 'sentence'}); // throws TypeError
new Intl.Segmenter('en', {granularity: 'bogus'});     // throws RangeError
```

## The partial-namespace trap

Before this change, `typeof Intl === 'undefined'` was a reliable signal that
*none* of Intl existed, and code that checked only that (rather than the
specific API it needed) got a clear, early failure. That is no longer true:
`typeof Intl !== 'undefined'` now holds, so a check written as
`if (typeof Intl !== 'undefined') { ... use Intl.NumberFormat ... }` will
pass the outer check and then hit `Intl.NumberFormat is not a constructor`
somewhere less obvious.

Per-API feature detection continues to work correctly and is what to use:

```js
typeof Intl.NumberFormat === 'function'  // false here -- absent, not stubbed
typeof Intl.Segmenter === 'function'     // true
```

`toLocaleString`, `toLocaleDateString`, `localeCompare` and similar methods
on `Number`, `Date` and `String` still exist (they come from the engine, not
from `Intl`), but none of them are locale-aware here -- they behave as their
locale-agnostic equivalents regardless of the locale argument passed in.
That is an easy thing to miss, since nothing about calling
`date.toLocaleDateString('ja-JP')` looks like it goes through `Intl` at all.

## Provenance

The grapheme implementation is a trimmed vendored copy of
[`unicode-segmenter`](https://github.com/cometkim/unicode-segmenter)
(MIT license), version 0.17.3, by Hyeseong Kim. Only the four files needed
for the grapheme path are vendored (`intl-adapter`, `grapheme`, `core`,
`_grapheme_data`) -- the word/sentence/emoji-sequence code that would have
come along with the rest of the package is left out entirely, since nothing
here can reach it. See `vendored/unicode-segmenter/README.md` for exactly
what was taken, the one modification made to it (an extension rewrite, to
fit this repository's CJS-only vendoring mechanism), and how to re-sync it
to a newer upstream version. The license text is in
`THIRD_PARTY_LICENSES.md`.

## Where this is going

A full `Intl`, backed by [ICU4X](https://github.com/unicode-org/icu4x), is
the intended eventual solution -- not this. What is here is a deliberate,
narrow stopgap for the one `Intl` API the terminal-UI ecosystem cannot start
without, shipped because waiting for a complete ICU4X-backed `Intl` would
have meant Ink-based tools staying entirely unreachable in the meantime. It
is not a preview of the destination's shape or scope.
