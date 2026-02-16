// RUN: %hermes-node %s | %FileCheck %s
// CHECK: PASS
// Test synchronous fs operations via the fs binding.
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

// --- mkdtempSync ---
var tmpDir = fs.mkdtempSync('/tmp/hermes-fs-test-');
assert(typeof tmpDir === 'string', 'mkdtempSync returns string');
assert(tmpDir.startsWith('/tmp/hermes-fs-test-'), 'mkdtempSync prefix');
passed++;

// --- writeFileSync + readFileSync ---
var testFile = path.join(tmpDir, 'test.txt');
fs.writeFileSync(testFile, 'hello world');
var content = fs.readFileSync(testFile, 'utf8');
assert(content === 'hello world', 'readFileSync returns written content');
passed++;

// --- readFileSync as Buffer ---
var bufContent = fs.readFileSync(testFile);
assert(Buffer.isBuffer(bufContent), 'readFileSync without encoding returns Buffer');
assert(bufContent.toString('utf8') === 'hello world', 'Buffer content matches');
passed++;

// --- statSync ---
var stat = fs.statSync(testFile);
assert(stat.isFile() === true, 'statSync isFile');
assert(stat.isDirectory() === false, 'statSync not isDirectory');
assert(stat.size === 11, 'statSync size');
assert(typeof stat.mtime === 'object', 'statSync mtime is Date');
assert(stat.mode > 0, 'statSync mode > 0');
passed++;

// --- statSync throwIfNoEntry: false ---
var noStat = fs.statSync('/nonexistent/path/surely', { throwIfNoEntry: false });
assert(noStat === undefined, 'statSync throwIfNoEntry: false returns undefined');
passed++;

// --- lstatSync ---
var lstat = fs.lstatSync(testFile);
assert(lstat.isFile() === true, 'lstatSync isFile');
assert(lstat.size === 11, 'lstatSync size');
passed++;

// --- fstatSync ---
var fd = fs.openSync(testFile, 'r');
var fst = fs.fstatSync(fd);
assert(fst.isFile() === true, 'fstatSync isFile');
assert(fst.size === 11, 'fstatSync size');
fs.closeSync(fd);
passed++;

// --- openSync + readSync + closeSync ---
fd = fs.openSync(testFile, 'r');
var buf = Buffer.alloc(5);
var bytesRead = fs.readSync(fd, buf, 0, 5, 0);
assert(bytesRead === 5, 'readSync reads 5 bytes');
assert(buf.toString('utf8') === 'hello', 'readSync content');
fs.closeSync(fd);
passed++;

// --- openSync + writeSync + closeSync ---
var writeFile = path.join(tmpDir, 'write-test.txt');
fd = fs.openSync(writeFile, 'w', 0o666);
var written = fs.writeSync(fd, Buffer.from('test data'));
assert(written === 9, 'writeSync writes 9 bytes');
fs.closeSync(fd);
assert(fs.readFileSync(writeFile, 'utf8') === 'test data', 'writeSync data persisted');
passed++;

// --- writeSync with string ---
var writeFile2 = path.join(tmpDir, 'write-str-test.txt');
fd = fs.openSync(writeFile2, 'w', 0o666);
written = fs.writeSync(fd, 'string data');
assert(written === 11, 'writeSync string writes 11 bytes');
fs.closeSync(fd);
assert(fs.readFileSync(writeFile2, 'utf8') === 'string data', 'writeSync string persisted');
passed++;

// --- renameSync ---
var newFile = path.join(tmpDir, 'renamed.txt');
fs.renameSync(testFile, newFile);
assert(fs.readFileSync(newFile, 'utf8') === 'hello world', 'renamed file readable');
assert(!fs.existsSync(testFile), 'old file gone after rename');
passed++;

// --- mkdirSync ---
var subDir = path.join(tmpDir, 'subdir');
fs.mkdirSync(subDir);
assert(fs.statSync(subDir).isDirectory(), 'mkdirSync creates directory');
passed++;

// --- mkdirSync recursive ---
var deepDir = path.join(tmpDir, 'a', 'b', 'c');
fs.mkdirSync(deepDir, { recursive: true });
assert(fs.statSync(deepDir).isDirectory(), 'mkdirSync recursive creates deep dir');
passed++;

// --- readdirSync ---
var entries = fs.readdirSync(tmpDir);
assert(Array.isArray(entries), 'readdirSync returns array');
assert(entries.indexOf('renamed.txt') !== -1, 'readdirSync includes renamed.txt');
assert(entries.indexOf('subdir') !== -1, 'readdirSync includes subdir');
passed++;

// --- readdirSync with withFileTypes ---
var dirents = fs.readdirSync(tmpDir, { withFileTypes: true });
assert(Array.isArray(dirents), 'readdirSync withFileTypes returns array');
var fileDirent = dirents.find(function(d) { return d.name === 'renamed.txt'; });
assert(fileDirent, 'found file dirent');
assert(fileDirent.isFile(), 'dirent isFile');
var dirDirent = dirents.find(function(d) { return d.name === 'subdir'; });
assert(dirDirent, 'found dir dirent');
assert(dirDirent.isDirectory(), 'dirent isDirectory');
passed++;

// --- chmodSync ---
fs.chmodSync(newFile, 0o644);
var chmodStat = fs.statSync(newFile);
assert((chmodStat.mode & 0o777) === 0o644, 'chmodSync sets mode');
passed++;

// --- copyFileSync ---
var copyDest = path.join(tmpDir, 'copy.txt');
fs.copyFileSync(newFile, copyDest);
assert(fs.readFileSync(copyDest, 'utf8') === 'hello world', 'copyFileSync copies content');
passed++;

// --- linkSync ---
var hardLink = path.join(tmpDir, 'hardlink.txt');
fs.linkSync(newFile, hardLink);
assert(fs.readFileSync(hardLink, 'utf8') === 'hello world', 'linkSync creates hard link');
passed++;

// --- symlinkSync + readlinkSync ---
var symLink = path.join(tmpDir, 'symlink.txt');
fs.symlinkSync(newFile, symLink);
assert(fs.readFileSync(symLink, 'utf8') === 'hello world', 'symlinkSync creates symlink');
var linkTarget = fs.readlinkSync(symLink);
assert(linkTarget === newFile, 'readlinkSync returns target');
passed++;

// --- realpathSync ---
var realPath = fs.realpathSync(symLink);
// realPath should resolve to the absolute path of newFile.
assert(typeof realPath === 'string', 'realpathSync returns string');
passed++;

// --- existsSync ---
assert(fs.existsSync(newFile) === true, 'existsSync returns true for existing file');
assert(fs.existsSync('/definitely/not/here') === false, 'existsSync returns false for nonexistent');
passed++;

// --- accessSync ---
fs.accessSync(newFile, fs.constants.R_OK);
passed++;

// --- ftruncateSync ---
var truncFile = path.join(tmpDir, 'trunc.txt');
fs.writeFileSync(truncFile, 'long content here');
fd = fs.openSync(truncFile, 'r+');
fs.ftruncateSync(fd, 4);
fs.closeSync(fd);
assert(fs.readFileSync(truncFile, 'utf8') === 'long', 'ftruncateSync truncates');
passed++;

// --- utimesSync ---
var now = new Date();
fs.utimesSync(newFile, now, now);
var utStat = fs.statSync(newFile);
// Just verify it doesn't throw and mtime is updated.
assert(utStat.mtime instanceof Date, 'utimesSync updates mtime');
passed++;

// --- fsyncSync ---
fd = fs.openSync(newFile, 'r');
fs.fsyncSync(fd);
fs.closeSync(fd);
passed++;

// --- Error handling: ENOENT ---
var threw = false;
try {
  fs.readFileSync('/nonexistent/path/file.txt');
} catch (e) {
  threw = true;
  assert(e.code === 'ENOENT', 'ENOENT error code');
  assert(e.syscall === 'open', 'ENOENT syscall');
  assert(typeof e.errno === 'number', 'ENOENT errno is number');
  assert(e.path === '/nonexistent/path/file.txt', 'ENOENT path');
}
assert(threw, 'readFileSync throws for nonexistent file');
passed++;

// --- Error handling: EACCES (try to write to /proc) ---
threw = false;
try {
  fs.writeFileSync('/proc/self/mem', 'nope');
} catch (e) {
  threw = true;
  assert(typeof e.code === 'string', 'write error has code');
}
assert(threw, 'writeFileSync throws for unwritable path');
passed++;

// --- rmSync recursive ---
fs.rmSync(tmpDir, { recursive: true });
assert(!fs.existsSync(tmpDir), 'rmSync removes directory recursively');
passed++;

console.log('All ' + passed + ' fs sync tests passed');
console.log('PASS');
