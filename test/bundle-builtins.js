// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// A builtin must be requirable from inside a bundle exactly as it is from a
// script -- no more and no less. Three lists have to agree for that: the
// modules actually compiled in (lib/embedded-modules/embedded-modules.txt),
// the run-time classifier (builtinIds in
// libjs/shims/internal/bootstrap/realm.js, behind
// BuiltinModule.normalizeRequirableId), and the producer's mirror of it
// (builtinIds() in lib/bundle/bundle_resolve.cpp).
//
// Nothing forced them to agree, and they did not: `zlib` was compiled in and
// requirable from a script but absent from the other two. A bundle routes a
// request by classification, so an unclassified builtin fell through to the
// closed world and threw MODULE_NOT_FOUND -- visible only once a program was
// bundled, and only if it used zlib.
//
// The assertion is a comparison rather than a list, so it needs no
// maintenance and cannot rot: the same file runs plain and bundled, and the
// two outputs must be identical. A name that is requirable one way and not
// the other fails here rather than in somebody's bundled program. Names that
// are requirable neither way -- the classifier lists several that are not
// compiled in -- are equally fine by this test, because they are equally
// broken in both modes and so are not a bundling defect.

// RUN: %hermes-node %s > %t.plain
// RUN: %hermes-node --build-bundle=%t.hbb %s && %hermes-node --bundle=%t.hbb > %t.bundled
// RUN: diff %t.plain %t.bundled
// RUN: %FileCheck %s < %t.bundled

var names = [
  'assert', 'assert/strict', 'async_hooks', 'buffer', 'child_process',
  'cluster', 'console', 'constants', 'crypto', 'dgram',
  'diagnostics_channel', 'dns', 'dns/promises', 'domain', 'events',
  'fs', 'fs/promises', 'http', 'https', 'module', 'net', 'os',
  'path', 'path/posix', 'path/win32', 'process', 'querystring',
  'readline', 'readline/promises', 'repl', 'stream', 'stream/consumers',
  'stream/promises', 'stream/web', 'string_decoder', 'timers',
  'timers/promises', 'tls', 'tty', 'url', 'util', 'util/types', 'vm',
  'zlib',
];

var ok = [];
for (var i = 0; i < names.length; i++) {
  var n = names[i];
  var bare = false, scheme = false;
  try { bare = !!require(n); } catch (e) {}
  try { scheme = !!require('node:' + n); } catch (e) {}
  if (bare && scheme) ok.push(n);
  else if (bare !== scheme) ok.push(n + '(BARE/SCHEME MISMATCH)');
}
console.log('requirable: ' + ok.join(' '));

// zlib is named explicitly because it is the regression this test exists
// for: a comparison alone would still pass if zlib broke in *both* modes.
// CHECK: requirable:
// CHECK-SAME: zlib
