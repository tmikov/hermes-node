// Copyright (c) Tzvetan Mikov.
// RUN: %hermes-node %s

// fs.rmSync()'s `force` option, which means "a path that is not there is not
// an error".
//
// This is the whole of the atomic-write idiom -- write a temporary file,
// rename it over the target, then remove the temporary in a `finally` -- and
// the remove is expected to be a no-op, because the rename already consumed
// the file. Without `force` honoured, every such writer throws ENOENT on the
// success path.
//
// Each expectation below was checked against node v24.13.1 first. The two
// rows that matter are the `force` + missing-path ones; the rest are here
// because a fix that special-cases a missing path can easily break them --
// in particular the dangling symlink, which is missing to stat() and present
// to lstat(), and which Node removes.

'use strict';

var assert = require('assert');
var fs = require('fs');
var os = require('os');
var path = require('path');

var root = fs.mkdtempSync(path.join(os.tmpdir(), 'hermes-rm-force-'));
var counter = 0;

function freshDir() {
  var d = path.join(root, 'case-' + counter++);
  fs.mkdirSync(d);
  return d;
}

function lstatExists(p) {
  try {
    fs.lstatSync(p);
    return true;
  } catch (e) {
    return false;
  }
}

function throwsCode(fn, code, what) {
  var threw = false;
  try {
    fn();
  } catch (e) {
    threw = true;
    assert.strictEqual(e.code, code, what + ' has code ' + code);
  }
  assert(threw, what + ' throws');
}

// -- a missing path with force is silently accepted --

var d = freshDir();
var missing = path.join(d, 'not-there');

fs.rmSync(missing, { force: true });
fs.rmSync(missing, { force: true, recursive: true });
assert(!lstatExists(missing), 'nothing was created by removing a missing path');

// -- a missing path without force still throws, as it always did --

throwsCode(
  function () { fs.rmSync(missing, {}); },
  'ENOENT',
  'missing path, no force',
);
throwsCode(
  function () { fs.rmSync(missing, { recursive: true }); },
  'ENOENT',
  'missing path, recursive without force',
);
throwsCode(
  function () { fs.rmSync(missing); },
  'ENOENT',
  'missing path, no options',
);

// -- force does not stop a file that IS there from being removed --

d = freshDir();
var file = path.join(d, 'f');
fs.writeFileSync(file, 'x');
fs.rmSync(file, { force: true });
assert(!lstatExists(file), 'force still removes a file that exists');

// -- a directory without recursive is EISDIR, force or not --

d = freshDir();
var sub = path.join(d, 'sub');
fs.mkdirSync(sub);
throwsCode(
  function () { fs.rmSync(sub, { force: true }); },
  'ERR_FS_EISDIR',
  'directory with force but not recursive',
);
throwsCode(
  function () { fs.rmSync(sub, {}); },
  'ERR_FS_EISDIR',
  'directory without recursive',
);
assert(lstatExists(sub), 'the directory survived both attempts');

// -- recursive still empties a populated tree --

d = freshDir();
sub = path.join(d, 'sub');
fs.mkdirSync(sub);
fs.writeFileSync(path.join(sub, 'child'), 'x');
fs.mkdirSync(path.join(sub, 'nested'));
fs.writeFileSync(path.join(sub, 'nested', 'deep'), 'x');
fs.rmSync(sub, { recursive: true, force: true });
assert(!lstatExists(sub), 'recursive force removed a populated tree');

// -- a dangling symlink is removed, not skipped --
//
// stat() cannot see it and lstat() can. A missing-path check written against
// stat() would silently leave this behind; node v24.13.1 removes it.

d = freshDir();
var dangling = path.join(d, 'dangling');
fs.symlinkSync(path.join(d, 'no-such-target'), dangling);
assert(lstatExists(dangling), 'the dangling symlink was created');
fs.rmSync(dangling, { force: true });
assert(!lstatExists(dangling), 'the dangling symlink was removed');

// -- a symlink to a real file removes the link and spares the target --

d = freshDir();
var target = path.join(d, 'target');
fs.writeFileSync(target, 'x');
var link = path.join(d, 'link');
fs.symlinkSync(target, link);
fs.rmSync(link, { force: true });
assert(!lstatExists(link), 'the symlink was removed');
assert(lstatExists(target), 'the symlink target survived');

// -- the idiom this all exists for --
//
// A write that renames its temporary into place and then removes the
// temporary unconditionally. The remove must not throw.

d = freshDir();
var dest = path.join(d, 'config.yaml');
var tmp = dest + '.tmp-' + process.pid + '-0';
try {
  fs.writeFileSync(tmp, 'name: demo\n');
  fs.renameSync(tmp, dest);
} finally {
  fs.rmSync(tmp, { force: true });
}
assert.strictEqual(fs.readFileSync(dest, 'utf8'), 'name: demo\n', 'atomic write landed');
assert(!lstatExists(tmp), 'no temporary left behind');

fs.rmSync(root, { recursive: true, force: true });
assert(!lstatExists(root), 'the test root cleaned itself up');

console.log('PASS');
