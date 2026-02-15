'use strict';

// Test fs_dir binding: opendirSync, Dir.readSync, Dir.closeSync, async opendir

var assert = globalThis.assert;
if (!assert) {
  assert = function assert(cond, msg) {
    if (!cond) throw new Error('Assertion failed: ' + (msg || ''));
  };
}
assert.strictEqual = function(a, b, msg) {
  if (a !== b)
    throw new Error(
      'strictEqual failed: ' + JSON.stringify(a) + ' !== ' + JSON.stringify(b) +
      (msg ? ' -- ' + msg : ''));
};

var fs = require('fs');
var path = require('path');

var passed = 0;

// ---------------------------------------------------------------------------
// Test 1: opendirSync + readSync + closeSync
// ---------------------------------------------------------------------------
(function testOpendirSyncBasic() {
  var tmpDir = fs.mkdtempSync('/tmp/hermes-dir-');
  fs.writeFileSync(tmpDir + '/a.txt', 'a');
  fs.writeFileSync(tmpDir + '/b.txt', 'b');
  fs.mkdirSync(tmpDir + '/sub');

  var dir = fs.opendirSync(tmpDir);
  var names = [];
  var dirent;
  while ((dirent = dir.readSync()) !== null) {
    names.push(dirent.name);
    if (dirent.name === 'sub') {
      assert(dirent.isDirectory(), 'sub should be a directory');
    } else {
      assert(dirent.isFile(), dirent.name + ' should be a file');
    }
  }
  dir.closeSync();

  names.sort();
  assert.strictEqual(names.join(','), 'a.txt,b.txt,sub', 'all entries found');

  fs.rmSync(tmpDir, { recursive: true });
  passed++;
  console.log('  test 1: opendirSync + readSync + closeSync -- ok');
})();

// ---------------------------------------------------------------------------
// Test 2: empty directory
// ---------------------------------------------------------------------------
(function testEmptyDir() {
  var tmpDir = fs.mkdtempSync('/tmp/hermes-dir-');

  var dir = fs.opendirSync(tmpDir);
  var result = dir.readSync();
  assert.strictEqual(result, null, 'empty dir returns null');
  dir.closeSync();

  fs.rmSync(tmpDir, { recursive: true });
  passed++;
  console.log('  test 2: empty directory -- ok');
})();

// ---------------------------------------------------------------------------
// Test 3: dirent types
// ---------------------------------------------------------------------------
(function testDirentTypes() {
  var tmpDir = fs.mkdtempSync('/tmp/hermes-dir-');
  fs.writeFileSync(tmpDir + '/file.txt', 'data');
  fs.mkdirSync(tmpDir + '/subdir');
  fs.symlinkSync(tmpDir + '/file.txt', tmpDir + '/link');

  var dir = fs.opendirSync(tmpDir);
  var entries = {};
  var dirent;
  while ((dirent = dir.readSync()) !== null) {
    entries[dirent.name] = dirent;
  }
  dir.closeSync();

  assert(entries['file.txt'].isFile(), 'file.txt is a file');
  assert(!entries['file.txt'].isDirectory(), 'file.txt is not a directory');
  assert(!entries['file.txt'].isSymbolicLink(), 'file.txt is not a symlink');

  assert(entries['subdir'].isDirectory(), 'subdir is a directory');
  assert(!entries['subdir'].isFile(), 'subdir is not a file');

  assert(entries['link'].isSymbolicLink(), 'link is a symlink');
  assert(!entries['link'].isFile(), 'link is not a file (it is a symlink)');
  assert(!entries['link'].isDirectory(), 'link is not a directory');

  fs.rmSync(tmpDir, { recursive: true });
  passed++;
  console.log('  test 3: dirent types -- ok');
})();

// ---------------------------------------------------------------------------
// Test 4: opendirSync with non-existent directory throws
// ---------------------------------------------------------------------------
(function testOpendirNonexistent() {
  var threw = false;
  try {
    fs.opendirSync('/tmp/hermes-nonexistent-dir-' + Date.now());
  } catch (e) {
    threw = true;
    assert.strictEqual(e.code, 'ENOENT', 'error code should be ENOENT');
    assert(e.syscall === 'opendir', 'syscall should be opendir');
  }
  assert(threw, 'should throw for non-existent directory');
  passed++;
  console.log('  test 4: opendirSync non-existent throws ENOENT -- ok');
})();

// ---------------------------------------------------------------------------
// Test 5: async opendir + read + close with callbacks
// ---------------------------------------------------------------------------
(function testAsyncOpendir() {
  var tmpDir = fs.mkdtempSync('/tmp/hermes-dir-');
  fs.writeFileSync(tmpDir + '/x.txt', 'x');
  fs.writeFileSync(tmpDir + '/y.txt', 'y');

  fs.opendir(tmpDir, function(err, dir) {
    assert(!err, 'opendir should not error: ' + (err && err.message));

    var names = [];
    function readNext() {
      dir.read(function(err2, dirent) {
        assert(!err2, 'read should not error');
        if (dirent === null) {
          // Done reading.
          dir.close(function(err3) {
            assert(!err3, 'close should not error');
            names.sort();
            assert.strictEqual(names.join(','), 'x.txt,y.txt', 'async entries');
            fs.rmSync(tmpDir, { recursive: true });
            passed++;
            console.log('  test 5: async opendir + read + close -- ok');
            checkDone();
          });
        } else {
          names.push(dirent.name);
          readNext();
        }
      });
    }
    readNext();
  });
})();

// ---------------------------------------------------------------------------
// Test 6: async opendir with non-existent directory
// ---------------------------------------------------------------------------
(function testAsyncOpendirError() {
  fs.opendir('/tmp/hermes-nonexistent-dir-' + Date.now(), function(err, dir) {
    assert(err, 'should error for non-existent directory');
    assert.strictEqual(err.code, 'ENOENT', 'error code should be ENOENT');
    assert.strictEqual(dir, undefined, 'dir should be undefined on error');
    passed++;
    console.log('  test 6: async opendir non-existent errors -- ok');
    checkDone();
  });
})();

// ---------------------------------------------------------------------------
// Test 7: bufferSize option
// ---------------------------------------------------------------------------
(function testBufferSize() {
  var tmpDir = fs.mkdtempSync('/tmp/hermes-dir-');
  // Create enough files to test with bufferSize=2.
  for (var i = 0; i < 5; i++) {
    fs.writeFileSync(tmpDir + '/f' + i + '.txt', 'data');
  }

  var dir = fs.opendirSync(tmpDir, { bufferSize: 2 });
  var names = [];
  var dirent;
  while ((dirent = dir.readSync()) !== null) {
    names.push(dirent.name);
  }
  dir.closeSync();

  assert.strictEqual(names.length, 5, 'should read all 5 entries');
  names.sort();
  assert.strictEqual(names.join(','), 'f0.txt,f1.txt,f2.txt,f3.txt,f4.txt',
    'all entries with bufferSize=2');

  fs.rmSync(tmpDir, { recursive: true });
  passed++;
  console.log('  test 7: bufferSize option -- ok');
})();

// ---------------------------------------------------------------------------
// Test 8: double close is safe
// ---------------------------------------------------------------------------
(function testDoubleClose() {
  var tmpDir = fs.mkdtempSync('/tmp/hermes-dir-');

  var dir = fs.opendirSync(tmpDir);
  dir.closeSync();

  // Second close should throw ERR_DIR_CLOSED.
  var threw = false;
  try {
    dir.closeSync();
  } catch (e) {
    threw = true;
    assert(e.code === 'ERR_DIR_CLOSED', 'should throw ERR_DIR_CLOSED');
  }
  assert(threw, 'should throw on double close');

  fs.rmSync(tmpDir, { recursive: true });
  passed++;
  console.log('  test 8: double close throws -- ok');
})();

// ---------------------------------------------------------------------------
// Test 9: read after close throws
// ---------------------------------------------------------------------------
(function testReadAfterClose() {
  var tmpDir = fs.mkdtempSync('/tmp/hermes-dir-');
  fs.writeFileSync(tmpDir + '/a.txt', 'a');

  var dir = fs.opendirSync(tmpDir);
  dir.closeSync();

  var threw = false;
  try {
    dir.readSync();
  } catch (e) {
    threw = true;
    assert(e.code === 'ERR_DIR_CLOSED', 'should throw ERR_DIR_CLOSED');
  }
  assert(threw, 'should throw on read after close');

  fs.rmSync(tmpDir, { recursive: true });
  passed++;
  console.log('  test 9: read after close throws -- ok');
})();

// ---------------------------------------------------------------------------
// Done
// ---------------------------------------------------------------------------

// For async tests: track total expected.
var asyncExpected = 2; // tests 5 and 6

function checkDone() {
  // This is called from async test completions.
  // We check if all async tests are done by counting.
  asyncExpected--;
  if (asyncExpected === 0) {
    console.log('All ' + passed + ' fs_dir tests passed');
    console.log('PASS');
  }
}

// Check sync tests (tests 1-4, 7-9 = 7 tests).
if (passed >= 7 && asyncExpected <= 0) {
  console.log('All ' + passed + ' fs_dir tests passed');
  console.log('PASS');
}
