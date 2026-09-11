// Copyright (c) Tzvetan Mikov.
// RUN: %hermes-node %s | %FileCheck %s
// CHECK: PASS
//
// Intl.Segmenter is the only member of the Intl namespace this runtime
// installs -- see docs/INTL.md. It is backed by the vendored
// unicode-segmenter package (vendored/unicode-segmenter/README.md) at
// grapheme granularity only.

'use strict';

var assert = function(cond, msg) {
  if (!cond) throw new Error('Assertion failed: ' + (msg || ''));
};

function deepEqualArrays(a, b, msg) {
  assert(a.length === b.length, (msg || '') + ' length: ' + a.length + ' vs ' + b.length);
  for (var i = 0; i < a.length; ++i) {
    assert(a[i] === b[i], (msg || '') + ' [' + i + ']: ' + JSON.stringify(a[i]) + ' vs ' + JSON.stringify(b[i]));
  }
}

function graphemeClusters(str) {
  var segmenter = new Intl.Segmenter();
  var out = [];
  for (var entry of segmenter.segment(str)) {
    out.push(entry.segment);
  }
  return out;
}

// ---- Honest absence: Intl exists, but only Segmenter does. ----

assert(typeof Intl === 'object', 'typeof Intl');
assert(typeof Intl.Segmenter === 'function', 'typeof Intl.Segmenter');
assert(Intl.NumberFormat === undefined, 'Intl.NumberFormat must be absent, not stubbed');

// ---- Property attributes. ----

var intlDesc = Object.getOwnPropertyDescriptor(globalThis, 'Intl');
assert(intlDesc.writable === true, 'globalThis.Intl writable');
assert(intlDesc.configurable === true, 'globalThis.Intl configurable');

var segmenterDesc = Object.getOwnPropertyDescriptor(Intl, 'Segmenter');
assert(segmenterDesc.writable === true, 'Intl.Segmenter writable');
assert(segmenterDesc.configurable === true, 'Intl.Segmenter configurable');
assert(segmenterDesc.enumerable === false, 'Intl.Segmenter must be non-enumerable');

// ---- Grapheme segmentation: the cases that separate a correct
// implementation from a plausible one, checked against native ICU as
// ground truth (see docs/INTL.md). A codepoint-splitting implementation
// fails every one of these. ----

// A 4-person ZWJ family emoji is one grapheme cluster, not four emoji
// joined by ZWJ codepoints.
var family = '\u{1F468}‍\u{1F469}‍\u{1F467}‍\u{1F466}';
deepEqualArrays(graphemeClusters(family), [family], 'ZWJ family emoji');

// A regional-indicator flag pair (US flag) is one cluster.
var flag = '\u{1F1FA}\u{1F1F8}';
deepEqualArrays(graphemeClusters(flag), [flag], 'regional-indicator flag');

// An emoji with a skin-tone modifier is one cluster.
var wave = '\u{1F44B}\u{1F3FD}';
deepEqualArrays(graphemeClusters(wave), [wave], 'emoji + skin-tone modifier');

// Latin combining marks attach to their base letter: "e" + COMBINING ACUTE
// ACCENT and "o" + COMBINING DIAERESIS are two clusters, not four
// codepoints.
var combining = 'éö';
deepEqualArrays(graphemeClusters(combining), ['é', 'ö'], 'combining marks');

// The Devanagari conjunct "अनुच्छेद"
// (aksharas "a", "nu", "cche", "da") is four grapheme clusters, the case
// that grapheme-splitter and graphemer both get wrong (see docs/INTL.md).
var deva = 'अनुच्छेद';
deepEqualArrays(
    graphemeClusters(deva),
    ['अ', 'नु', 'च्छे', 'द'],
    'Devanagari conjunct');

// ---- word/sentence granularity refuse rather than silently segmenting by
// grapheme; an invalid granularity is a RangeError. ----

var threw;

threw = undefined;
try {
  new Intl.Segmenter('en', {granularity: 'word'});
} catch (e) {
  threw = e;
}
assert(threw instanceof TypeError, 'word granularity must throw TypeError');

threw = undefined;
try {
  new Intl.Segmenter('en', {granularity: 'sentence'});
} catch (e) {
  threw = e;
}
assert(threw instanceof TypeError, 'sentence granularity must throw TypeError');

threw = undefined;
try {
  new Intl.Segmenter('en', {granularity: 'bogus'});
} catch (e) {
  threw = e;
}
assert(threw instanceof RangeError, 'invalid granularity must throw RangeError');

console.log('PASS');
