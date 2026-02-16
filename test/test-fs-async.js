// RUN: %hermes-node %s | %FileCheck %s
// CHECK: PASS
// Test async fs operations via the fs module (callback and promise APIs).
'use strict';

var fs = require('fs');
var path = require('path');
var Buffer = require('buffer').Buffer;

var passed = 0;
var expected = 0;

function assert(cond, msg) {
  if (!cond) {
    throw new Error('Assertion failed: ' + (msg || ''));
  }
}

function done(name) {
  passed++;
  if (passed === expected) {
    console.log('All ' + passed + ' fs async tests passed');
    console.log('PASS');
  }
}

// Create a temp dir synchronously for test setup.
var tmpDir = fs.mkdtempSync('/tmp/hermes-fs-async-test-');

// --- Callback API tests ---

// 1. fs.writeFile + fs.readFile (callback)
expected++;
var testFile = path.join(tmpDir, 'callback-test.txt');
fs.writeFile(testFile, 'hello async', function(err) {
  assert(!err, 'writeFile callback no error: ' + (err && err.message));
  fs.readFile(testFile, 'utf8', function(err2, data) {
    assert(!err2, 'readFile callback no error');
    assert(data === 'hello async', 'readFile returns correct content: ' + data);
    done('writeFile+readFile callback');
  });
});

// 2. fs.stat (callback)
expected++;
fs.stat(tmpDir, function(err, stats) {
  assert(!err, 'stat callback no error');
  assert(stats.isDirectory(), 'stat callback isDirectory');
  done('stat callback');
});

// 3. fs.mkdir + fs.readdir (callback)
expected++;
var asyncSubDir = path.join(tmpDir, 'async-subdir');
fs.mkdir(asyncSubDir, function(err) {
  assert(!err, 'mkdir callback no error: ' + (err && err.message));
  fs.readdir(tmpDir, function(err2, entries) {
    assert(!err2, 'readdir callback no error');
    assert(Array.isArray(entries), 'readdir returns array');
    assert(entries.indexOf('async-subdir') !== -1, 'readdir includes async-subdir');
    done('mkdir+readdir callback');
  });
});

// 4. fs.open + fs.write + fs.read + fs.close (callback, low-level)
expected++;
var lowFile = path.join(tmpDir, 'low-level.txt');
fs.open(lowFile, 'w', 0o666, function(err, fd) {
  assert(!err, 'open for write no error: ' + (err && err.message));
  assert(typeof fd === 'number', 'open returns fd');
  var buf = Buffer.from('low level data');
  fs.write(fd, buf, 0, buf.length, 0, function(err2, bytesWritten) {
    assert(!err2, 'write callback no error');
    assert(bytesWritten === 14, 'write wrote 14 bytes');
    fs.close(fd, function(err3) {
      assert(!err3, 'close after write no error');
      // Now read it back.
      fs.open(lowFile, 'r', function(err4, fd2) {
        assert(!err4, 'open for read no error');
        var readBuf = Buffer.alloc(14);
        fs.read(fd2, readBuf, 0, 14, 0, function(err5, bytesRead) {
          assert(!err5, 'read callback no error');
          assert(bytesRead === 14, 'read 14 bytes');
          assert(readBuf.toString('utf8') === 'low level data', 'read content matches');
          fs.close(fd2, function(err6) {
            assert(!err6, 'close after read no error');
            done('open+write+read+close callback');
          });
        });
      });
    });
  });
});

// 5. Error handling: ENOENT (callback)
expected++;
fs.readFile('/nonexistent/path/file.txt', function(err, data) {
  assert(err, 'readFile nonexistent has error');
  assert(err.code === 'ENOENT', 'readFile nonexistent error code is ENOENT');
  done('ENOENT callback');
});

// 6. fs.rename (callback)
expected++;
var renSrc = path.join(tmpDir, 'rename-src.txt');
var renDst = path.join(tmpDir, 'rename-dst.txt');
fs.writeFileSync(renSrc, 'rename me');
fs.rename(renSrc, renDst, function(err) {
  assert(!err, 'rename callback no error');
  assert(!fs.existsSync(renSrc), 'rename src gone');
  assert(fs.readFileSync(renDst, 'utf8') === 'rename me', 'rename dst has content');
  done('rename callback');
});

// 7. fs.unlink (callback)
expected++;
var unlinkFile = path.join(tmpDir, 'to-unlink.txt');
fs.writeFileSync(unlinkFile, 'delete me');
fs.unlink(unlinkFile, function(err) {
  assert(!err, 'unlink callback no error');
  assert(!fs.existsSync(unlinkFile), 'unlink removed file');
  done('unlink callback');
});

// Note: fs.promises API tests are not included because Hermes does not
// support async generators, which fs/promises requires internally.

// 8. fs.mkdtemp (callback)
expected++;
fs.mkdtemp(path.join(tmpDir, 'mkdtemp-'), function(err, dir) {
  assert(!err, 'mkdtemp callback no error: ' + (err && err.message));
  assert(typeof dir === 'string', 'mkdtemp returns string');
  assert(dir.startsWith(path.join(tmpDir, 'mkdtemp-')), 'mkdtemp has prefix');
  assert(fs.statSync(dir).isDirectory(), 'mkdtemp created directory');
  done('mkdtemp callback');
});

// 14. fs.copyFile (callback)
expected++;
var copySrc = path.join(tmpDir, 'copy-src.txt');
var copyDst = path.join(tmpDir, 'copy-dst.txt');
fs.writeFileSync(copySrc, 'copy me');
fs.copyFile(copySrc, copyDst, function(err) {
  assert(!err, 'copyFile callback no error');
  assert(fs.readFileSync(copyDst, 'utf8') === 'copy me', 'copyFile copied content');
  done('copyFile callback');
});
