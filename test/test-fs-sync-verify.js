// Copyright (c) Tzvetan Mikov.
// RUN: %hermes-node %s | %FileCheck %s
// CHECK: PASS
// Step 31: Verify fs sync operations -- edge cases beyond test-fs-sync.js
'use strict';

var fs = require('fs');
var path = require('path');
var Buffer = require('buffer').Buffer;

var passed = 0;

function assert(cond, msg) {
  if (!cond) {
    throw new Error('Assertion failed: ' + (msg || ''));
  }
}

// --- accessSync with combined flags ---
fs.accessSync('/tmp', fs.constants.R_OK | fs.constants.W_OK);
passed++;

// --- symlink + readlink + lstat isSymbolicLink + stat follows symlink ---
var tmpDir = fs.mkdtempSync('/tmp/hermes-sym-');

fs.writeFileSync(tmpDir + '/target', 'data');
fs.symlinkSync(tmpDir + '/target', tmpDir + '/link');

// readlinkSync returns the symlink target
var linkTarget = fs.readlinkSync(tmpDir + '/link');
assert(linkTarget.endsWith('/target'), 'readlink ends with /target: ' + linkTarget);
passed++;

// lstatSync on the symlink sees it as a symlink
var lstat = fs.lstatSync(tmpDir + '/link');
assert(lstat.isSymbolicLink(), 'lstat isSymbolicLink');
assert(!lstat.isFile(), 'lstat on symlink is not isFile');
passed++;

// statSync on the symlink follows it and sees a file
var stat = fs.statSync(tmpDir + '/link');
assert(stat.isFile(), 'stat on symlink follows to isFile');
assert(!stat.isSymbolicLink(), 'stat on symlink is not isSymbolicLink');
passed++;

// --- rmSync cleans up the symlink directory ---
fs.rmSync(tmpDir, { recursive: true });
assert(!fs.existsSync(tmpDir), 'rmSync removes directory');
passed++;

// --- accessSync ENOENT error ---
var threw = false;
try {
  fs.accessSync('/nonexistent/path/surely');
} catch (e) {
  threw = true;
  assert(e.code === 'ENOENT', 'accessSync ENOENT error code');
  assert(e.syscall === 'access', 'accessSync ENOENT syscall');
}
assert(threw, 'accessSync throws for nonexistent path');
passed++;

// --- mkdirSync recursive returns first created path ---
tmpDir = fs.mkdtempSync('/tmp/hermes-mkdir-');
var deepDir = path.join(tmpDir, 'a', 'b', 'c');
var result = fs.mkdirSync(deepDir, { recursive: true });
assert(typeof result === 'string', 'mkdirSync recursive returns string');
assert(result.includes('/a'), 'mkdirSync recursive returns first created: ' + result);
assert(fs.statSync(deepDir).isDirectory(), 'deep dir exists');
passed++;

// --- mkdirSync recursive on existing dir returns undefined ---
result = fs.mkdirSync(deepDir, { recursive: true });
assert(result === undefined, 'mkdirSync recursive on existing returns undefined');
passed++;

// --- lchownSync ---
tmpDir = fs.mkdtempSync('/tmp/hermes-lchown-');
var file = path.join(tmpDir, 'test.txt');
fs.writeFileSync(file, 'data');
// lchown to same uid/gid should not throw
var fstat = fs.statSync(file);
fs.lchownSync(file, fstat.uid, fstat.gid);
passed++;

// --- readdir recursive (if supported) ---
var subDir1 = path.join(tmpDir, 'sub');
fs.mkdirSync(subDir1);
fs.writeFileSync(path.join(subDir1, 'nested.txt'), 'nested');
var entries = fs.readdirSync(tmpDir, { recursive: true });
assert(Array.isArray(entries), 'readdirSync recursive returns array');
// Should contain both 'test.txt' and 'sub/nested.txt' or similar
var hasNested = entries.some(function(e) {
  return typeof e === 'string' && e.indexOf('nested.txt') !== -1;
});
assert(hasNested, 'readdirSync recursive finds nested file: ' + JSON.stringify(entries));
passed++;

// --- writeFileSync + readFileSync with Buffer ---
var bufFile = path.join(tmpDir, 'buf.bin');
var testBuf = Buffer.from([0x00, 0x01, 0x02, 0xfe, 0xff]);
fs.writeFileSync(bufFile, testBuf);
var readBack = fs.readFileSync(bufFile);
assert(Buffer.isBuffer(readBack), 'readFileSync returns Buffer');
assert(readBack.length === 5, 'buffer length matches');
assert(readBack[0] === 0x00 && readBack[3] === 0xfe && readBack[4] === 0xff, 'buffer content matches');
passed++;

// --- statSync with bigint option ---
stat = fs.statSync(file, { bigint: true });
assert(typeof stat.size === 'bigint', 'bigint stat size is bigint: ' + typeof stat.size);
assert(stat.size === 4n, 'bigint stat size value');
assert(typeof stat.ino === 'bigint', 'bigint stat ino is bigint');
assert(typeof stat.mtimeNs === 'bigint', 'bigint stat mtimeNs is bigint');
passed++;

// --- readdirSync withFileTypes on symlinks ---
var symDir = fs.mkdtempSync('/tmp/hermes-symdir-');
fs.writeFileSync(path.join(symDir, 'real.txt'), 'real');
fs.symlinkSync(path.join(symDir, 'real.txt'), path.join(symDir, 'link.txt'));
var dirents = fs.readdirSync(symDir, { withFileTypes: true });
var linkDirent = dirents.find(function(d) { return d.name === 'link.txt'; });
assert(linkDirent, 'found link dirent');
assert(linkDirent.isSymbolicLink(), 'dirent isSymbolicLink');
var fileDirent = dirents.find(function(d) { return d.name === 'real.txt'; });
assert(fileDirent, 'found file dirent');
assert(fileDirent.isFile(), 'dirent isFile');
fs.rmSync(symDir, { recursive: true });
passed++;

// --- fdatasyncSync ---
var fdFile = path.join(tmpDir, 'fdsync.txt');
fs.writeFileSync(fdFile, 'data');
var fdFd = fs.openSync(fdFile, 'r+');
fs.fdatasyncSync(fdFd);
fs.closeSync(fdFd);
passed++;

// --- lutimesSync ---
var lutFile = path.join(tmpDir, 'lutimes-target.txt');
fs.writeFileSync(lutFile, 'data');
var lutLink = path.join(tmpDir, 'lutimes-link');
fs.symlinkSync(lutFile, lutLink);
var newTime = new Date('2020-01-01T00:00:00Z');
fs.lutimesSync(lutLink, newTime, newTime);
var lutStat = fs.lstatSync(lutLink);
// Verify the symlink's mtime was changed (not the target's)
assert(lutStat.mtime instanceof Date, 'lutimesSync sets mtime on symlink');
// The mtime should be close to 2020-01-01
assert(Math.abs(lutStat.mtime.getTime() - newTime.getTime()) < 2000,
  'lutimes mtime matches: ' + lutStat.mtime.toISOString());
passed++;

// --- Cleanup ---
fs.rmSync(tmpDir, { recursive: true });
assert(!fs.existsSync(tmpDir), 'final cleanup done');
passed++;

console.log('All ' + passed + ' fs sync verify tests passed');
console.log('PASS');
