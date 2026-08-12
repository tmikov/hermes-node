// Copyright (c) Tzvetan Mikov.
// RUN: mkdir -p %t/eval-require/node_modules/hello-addon
// RUN: cp %hello_addon %t/eval-require/node_modules/hello-addon/hello_addon.node
// RUN: echo '{"name":"hello-addon","main":"hello_addon.node"}' > %t/eval-require/node_modules/hello-addon/package.json
// RUN: echo '{"answer":42}' > %t/eval-require/data.json
// RUN: cd %t/eval-require && %hermes-node %s | %FileCheck %s
// CHECK: PASS
//
// Test: `require` inside -e behaves like Node's, resolving by extension and
// through node_modules, rather than assuming every path is JavaScript source.
//
// Eval code used to get the bootstrap module loader, which knows only embedded
// module ids and otherwise reads the path off disk and compiles it as JS. A
// .node addon therefore died on the first byte of the ELF magic
// ("unrecognized Unicode character \u7f") and a .json file on its first colon.

'use strict';

var assert = require('assert');
var child_process = require('child_process');
var path = require('path');

var hermesNode = process.execPath;

// The RUN lines cd into the fixture directory, so relative resolution and
// node_modules lookup are exercised from the current working directory, the
// way Node roots -e require.
function evalIn(code) {
  return child_process.spawnSync(hermesNode, ['-e', code], {
    encoding: 'utf8',
    cwd: process.cwd(),
  });
}

function check(label, code, expected) {
  var r = evalIn(code);
  assert.strictEqual(
    r.status,
    0,
    label + ': exit ' + r.status + ', stderr: ' + r.stderr,
  );
  assert.strictEqual(r.stdout.trim(), expected, label + ': stdout mismatch');
}

// 1. A native .node addon, by relative path.
check(
  'relative .node',
  "console.log(require('./node_modules/hello-addon/hello_addon.node').hello())",
  'world',
);

// 2. The same addon resolved as a package through node_modules.
check(
  'node_modules addon',
  "console.log(require('hello-addon').add(2, 3))",
  '5',
);

// 3. A .json file, which must be parsed rather than compiled.
check('relative .json', "console.log(require('./data.json').answer)", '42');

// 4. An absolute path still works.
check(
  'absolute .json',
  'console.log(require(' +
    JSON.stringify(path.join(process.cwd(), 'data.json')) +
    ').answer)',
  '42',
);

// 5. Embedded builtins keep working from -e.
check('builtin', "console.log(typeof require('fs').readFileSync)", 'function');

// 6. So do internal module ids, which only the bootstrap loader can resolve.
check(
  'internal id',
  "console.log(typeof require('internal/errors').codes)",
  'object',
);

// 7. Eval code runs at global scope, as Node's does: a top-level `var` lands
// on globalThis. Wrapping the code in a module function would break this.
check('global scope', "var x = 1; console.log(globalThis.x)", '1');

// 8. The module identity Node exposes for -e: a bare "[eval]" id and
// __filename, "." for __dirname, an absolute module.filename, and node_modules
// resolution rooted at the cwd.
var idProbe = evalIn(
  'console.log(JSON.stringify({f: __filename, d: __dirname, id: module.id,' +
    ' mf: module.filename, p0: module.paths[0]}))',
);
assert.strictEqual(idProbe.status, 0, 'identity probe: ' + idProbe.stderr);
var ids = JSON.parse(idProbe.stdout);
assert.strictEqual(ids.f, '[eval]', '__filename');
assert.strictEqual(ids.d, '.', '__dirname');
assert.strictEqual(ids.id, '[eval]', 'module.id');
assert.strictEqual(
  ids.mf,
  path.join(process.cwd(), '[eval]'),
  'module.filename is absolute',
);
assert.strictEqual(
  ids.p0,
  path.join(process.cwd(), 'node_modules'),
  'module.paths starts at the cwd',
);

console.log('PASS');
