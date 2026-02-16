// RUN: %hermes-node %s | %FileCheck %s
// CHECK: PASS
// Test fs.cpSync() functionality.
'use strict';

var fs = require('fs');
var path = require('path');

var passed = 0;

function assert(cond, msg) {
  if (!cond) {
    throw new Error('Assertion failed: ' + (msg || ''));
  }
}

function assertThrows(fn, checkFn, msg) {
  var threw = false;
  try {
    fn();
  } catch (e) {
    threw = true;
    if (checkFn) checkFn(e);
  }
  assert(threw, msg || 'expected function to throw');
}

// Create a temp directory for all tests.
var tmpDir = fs.mkdtempSync('/tmp/hermes-cp-test-');

// --- Test 1: Copy single file ---
var srcFile = path.join(tmpDir, 'src.txt');
fs.writeFileSync(srcFile, 'hello cpSync');
var destFile = path.join(tmpDir, 'dest.txt');
fs.cpSync(srcFile, destFile);
assert(fs.readFileSync(destFile, 'utf8') === 'hello cpSync', 'single file copy');
passed++;

// --- Test 2: Copy file with overwrite (force: true) ---
fs.writeFileSync(destFile, 'old content');
fs.cpSync(srcFile, destFile, { force: true });
assert(fs.readFileSync(destFile, 'utf8') === 'hello cpSync', 'overwrite with force');
passed++;

// --- Test 3: Copy nested directory recursively ---
var srcDir = path.join(tmpDir, 'srcdir');
fs.mkdirSync(srcDir);
fs.writeFileSync(path.join(srcDir, 'a.txt'), 'file a');
var subDir = path.join(srcDir, 'sub');
fs.mkdirSync(subDir);
fs.writeFileSync(path.join(subDir, 'b.txt'), 'file b');

var destDir = path.join(tmpDir, 'destdir');
fs.cpSync(srcDir, destDir, { recursive: true });
assert(fs.readFileSync(path.join(destDir, 'a.txt'), 'utf8') === 'file a', 'recursive copy file a');
assert(fs.readFileSync(path.join(destDir, 'sub', 'b.txt'), 'utf8') === 'file b', 'recursive copy file b');
passed++;

// --- Test 4: Preserve timestamps ---
var tsSrc = path.join(tmpDir, 'ts-src.txt');
fs.writeFileSync(tsSrc, 'timestamp test');
// Set a known time in the past.
var pastTime = new Date('2020-01-01T00:00:00Z');
fs.utimesSync(tsSrc, pastTime, pastTime);
var tsDest = path.join(tmpDir, 'ts-dest.txt');
fs.cpSync(tsSrc, tsDest, { preserveTimestamps: true });
var destStat = fs.statSync(tsDest);
// mtime should be close to pastTime (within 2 seconds).
var mtimeDiff = Math.abs(destStat.mtime.getTime() - pastTime.getTime());
assert(mtimeDiff < 2000, 'preserveTimestamps mtime close to source: diff=' + mtimeDiff);
passed++;

// --- Test 5: Error: directory without recursive flag ---
var dirSrc = path.join(tmpDir, 'dir-no-rec');
fs.mkdirSync(dirSrc);
fs.writeFileSync(path.join(dirSrc, 'x.txt'), 'x');
assertThrows(
  function() { fs.cpSync(dirSrc, path.join(tmpDir, 'dir-no-rec-dest')); },
  function(e) {
    assert(e.code === 'ERR_FS_EISDIR', 'error code for dir without recursive: ' + e.code);
  },
  'should throw for directory without recursive'
);
passed++;

// --- Test 6: Error: src and dest are same path ---
var sameSrc = path.join(tmpDir, 'same.txt');
fs.writeFileSync(sameSrc, 'same');
assertThrows(
  function() { fs.cpSync(sameSrc, sameSrc); },
  function(e) {
    assert(e.code === 'ERR_FS_CP_EINVAL', 'error code for same path: ' + e.code);
  },
  'should throw for same src and dest'
);
passed++;

// --- Test 7: Error: copy directory to subdirectory of self ---
var selfDir = path.join(tmpDir, 'selfdir');
fs.mkdirSync(selfDir);
fs.writeFileSync(path.join(selfDir, 'f.txt'), 'f');
var selfSub = path.join(selfDir, 'child');
fs.mkdirSync(selfSub);
assertThrows(
  function() { fs.cpSync(selfDir, path.join(selfSub, 'nested'), { recursive: true }); },
  function(e) {
    assert(e.code === 'ERR_FS_CP_EINVAL', 'error code for subdirectory of self: ' + e.code);
  },
  'should throw for copy to subdirectory of self'
);
passed++;

// --- Test 8: Symlinks with verbatimSymlinks: true ---
var symSrc = path.join(tmpDir, 'sym-src');
fs.mkdirSync(symSrc);
var symTarget = path.join(symSrc, 'target.txt');
fs.writeFileSync(symTarget, 'symlink target');
fs.symlinkSync('target.txt', path.join(symSrc, 'link.txt'));

var symDest = path.join(tmpDir, 'sym-dest');
fs.cpSync(symSrc, symDest, { recursive: true, verbatimSymlinks: true });
// The symlink should be preserved as a symlink.
var linkStat = fs.lstatSync(path.join(symDest, 'link.txt'));
assert(linkStat.isSymbolicLink(), 'verbatimSymlinks preserves symlink');
var linkTarget = fs.readlinkSync(path.join(symDest, 'link.txt'));
assert(linkTarget === 'target.txt', 'verbatimSymlinks preserves relative target');
passed++;

// --- Test 9: errorOnExist: true ---
var eoeSrc = path.join(tmpDir, 'eoe-src.txt');
fs.writeFileSync(eoeSrc, 'eoe data');
var eoeDest = path.join(tmpDir, 'eoe-dest.txt');
fs.writeFileSync(eoeDest, 'existing');
assertThrows(
  function() { fs.cpSync(eoeSrc, eoeDest, { force: false, errorOnExist: true }); },
  function(e) {
    assert(
      e.code === 'ERR_FS_CP_EEXIST' || e.code === 'EEXIST',
      'error code for errorOnExist: ' + e.code
    );
  },
  'should throw for errorOnExist'
);
passed++;

// --- Test 10: Skip existing with force: false, errorOnExist: false ---
var skipSrc = path.join(tmpDir, 'skip-src.txt');
fs.writeFileSync(skipSrc, 'new content');
var skipDest = path.join(tmpDir, 'skip-dest.txt');
fs.writeFileSync(skipDest, 'old content');
// Should not throw and should not overwrite.
fs.cpSync(skipSrc, skipDest, { force: false, errorOnExist: false });
assert(fs.readFileSync(skipDest, 'utf8') === 'old content', 'skip existing does not overwrite');
passed++;

// --- Cleanup ---
fs.rmSync(tmpDir, { recursive: true });

console.log('All ' + passed + ' fs.cpSync tests passed');
console.log('PASS');
