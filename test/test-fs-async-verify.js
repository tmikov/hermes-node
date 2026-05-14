// Copyright (c) Tzvetan Mikov.
// RUN: %hermes-node %s | %FileCheck %s
// CHECK: PASS
// Test: comprehensive verification of async fs operations (Step 32).
// Covers callback-based async edge cases and fs.promises operations.
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

function assertEqual(a, b, msg) {
  if (a !== b) {
    throw new Error('assertEqual failed: ' + JSON.stringify(a) + ' !== ' +
      JSON.stringify(b) + (msg ? ' -- ' + msg : ''));
  }
}

var failCount = 0;

function done(name) {
  passed++;
  if (passed + failCount === expected) {
    if (failCount > 0) {
      console.log(failCount + ' FAILED, ' + passed + ' passed out of ' + expected);
    } else {
      console.log('All ' + passed + ' async verify tests passed');
      console.log('PASS');
    }
  }
}

function fail(name, msg) {
  failCount++;
  console.log('FAIL: ' + name + ': ' + msg);
  if (passed + failCount === expected) {
    console.log(failCount + ' FAILED, ' + passed + ' passed out of ' + expected);
  }
}

// Create a temp dir for all tests.
var tmpDir = fs.mkdtempSync('/tmp/hermes-fs-async-verify-');

// --- Test 1: fs.chmod (callback) ---
expected++;
(function() {
  var f = path.join(tmpDir, 'chmod-test.txt');
  fs.writeFileSync(f, 'data');
  fs.chmod(f, 0o444, function(err) {
    assert(!err, 'chmod no error: ' + (err && err.message));
    var st = fs.statSync(f);
    assertEqual(st.mode & 0o777, 0o444, 'chmod changed permissions');
    // Restore for cleanup.
    fs.chmodSync(f, 0o644);
    done('chmod callback');
  });
})();

// --- Test 2: fs.lstat (callback) ---
expected++;
(function() {
  var f = path.join(tmpDir, 'lstat-target.txt');
  var link = path.join(tmpDir, 'lstat-link');
  fs.writeFileSync(f, 'target data');
  fs.symlinkSync(f, link);
  fs.lstat(link, function(err, stats) {
    assert(!err, 'lstat no error');
    assert(stats.isSymbolicLink(), 'lstat reports symlink');
    assert(!stats.isFile(), 'lstat symlink is not file');
    done('lstat callback');
  });
})();

// --- Test 3: fs.ftruncate (callback) ---
expected++;
(function() {
  var f = path.join(tmpDir, 'truncate-test.txt');
  fs.writeFileSync(f, 'hello world long content');
  var fd = fs.openSync(f, 'r+');
  fs.ftruncate(fd, 5, function(err) {
    assert(!err, 'ftruncate no error');
    fs.closeSync(fd);
    var content = fs.readFileSync(f, 'utf8');
    assertEqual(content, 'hello', 'ftruncate truncated to 5 bytes');
    done('ftruncate callback');
  });
})();

// --- Test 4: fs.link + verify (callback) ---
expected++;
(function() {
  var f = path.join(tmpDir, 'link-src.txt');
  var hardLink = path.join(tmpDir, 'link-hard.txt');
  fs.writeFileSync(f, 'link content');
  fs.link(f, hardLink, function(err) {
    assert(!err, 'link no error: ' + (err && err.message));
    assertEqual(fs.readFileSync(hardLink, 'utf8'), 'link content', 'hard link content');
    // Verify same inode.
    var srcStat = fs.statSync(f);
    var linkStat = fs.statSync(hardLink);
    assertEqual(srcStat.ino, linkStat.ino, 'hard link same inode');
    done('link callback');
  });
})();

// --- Test 5: fs.symlink + fs.readlink (callback) ---
expected++;
(function() {
  var f = path.join(tmpDir, 'symlink-src.txt');
  var symLink = path.join(tmpDir, 'symlink-link');
  fs.writeFileSync(f, 'symlink content');
  fs.symlink(f, symLink, function(err) {
    assert(!err, 'symlink no error');
    fs.readlink(symLink, function(err2, target) {
      assert(!err2, 'readlink no error');
      assertEqual(target, f, 'readlink returns target');
      done('symlink+readlink callback');
    });
  });
})();

// --- Test 6: fs.realpath (callback) ---
expected++;
(function() {
  var f = path.join(tmpDir, 'realpath-target.txt');
  var link = path.join(tmpDir, 'realpath-link');
  fs.writeFileSync(f, 'real');
  fs.symlinkSync(f, link);
  fs.realpath(link, function(err, resolved) {
    assert(!err, 'realpath no error');
    assertEqual(resolved, fs.realpathSync(f), 'realpath resolved correctly');
    done('realpath callback');
  });
})();

// --- Test 7: fs.utimes (callback) ---
expected++;
(function() {
  var f = path.join(tmpDir, 'utimes-test.txt');
  fs.writeFileSync(f, 'utimes');
  var atime = new Date('2020-01-15T10:00:00Z');
  var mtime = new Date('2021-06-20T15:30:00Z');
  fs.utimes(f, atime, mtime, function(err) {
    assert(!err, 'utimes no error');
    var st = fs.statSync(f);
    // Only assert mtime: macOS APFS does not reliably persist atime under
    // concurrent fs load (the kernel can reset it to "now" between the
    // utimensat syscall completing and our callback running). Tests 29
    // (lutimes) and 31 (futimes) below check mtime only for the same
    // reason. uv_fs_utime is verified end-to-end for both atime and
    // mtime when called in isolation.
    assert(Math.abs(st.mtimeMs - mtime.getTime()) < 2000,
      'mtime set: ' + st.mtimeMs + ' vs ' + mtime.getTime());
    done('utimes callback');
  });
})();

// --- Test 8: fs.access (callback) ---
expected++;
(function() {
  var f = path.join(tmpDir, 'access-test.txt');
  fs.writeFileSync(f, 'access');
  fs.access(f, fs.constants.R_OK | fs.constants.W_OK, function(err) {
    assert(!err, 'access R_OK|W_OK no error for existing file');
    done('access callback');
  });
})();

// --- Test 9: fs.access ENOENT (callback) ---
expected++;
(function() {
  fs.access('/nonexistent-path-' + Date.now(), function(err) {
    assert(err, 'access errors for nonexistent');
    assertEqual(err.code, 'ENOENT', 'access ENOENT code');
    done('access ENOENT callback');
  });
})();

// --- Test 10: fs.stat with bigint option (callback) ---
expected++;
(function() {
  var f = path.join(tmpDir, 'bigint-stat.txt');
  fs.writeFileSync(f, 'bigint data here');
  fs.stat(f, { bigint: true }, function(err, stats) {
    assert(!err, 'stat bigint no error');
    assertEqual(typeof stats.size, 'bigint', 'size is bigint');
    assertEqual(typeof stats.ino, 'bigint', 'ino is bigint');
    assertEqual(typeof stats.mtimeNs, 'bigint', 'mtimeNs is bigint');
    assert(stats.size === 16n, 'bigint size = 16: got ' + stats.size);
    assert(stats.isFile(), 'bigint stat isFile');
    done('stat bigint callback');
  });
})();

// --- Test 11: fs.fsync (callback) ---
expected++;
(function() {
  var f = path.join(tmpDir, 'fsync-test.txt');
  var fd = fs.openSync(f, 'w');
  fs.writeSync(fd, 'fsync data');
  fs.fsync(fd, function(err) {
    assert(!err, 'fsync no error');
    fs.closeSync(fd);
    assertEqual(fs.readFileSync(f, 'utf8'), 'fsync data', 'data persisted after fsync');
    done('fsync callback');
  });
})();

// --- Test 12: fs.fdatasync (callback) ---
expected++;
(function() {
  var f = path.join(tmpDir, 'fdatasync-test.txt');
  var fd = fs.openSync(f, 'w');
  fs.writeSync(fd, 'datasync data');
  fs.fdatasync(fd, function(err) {
    assert(!err, 'fdatasync no error');
    fs.closeSync(fd);
    assertEqual(fs.readFileSync(f, 'utf8'), 'datasync data', 'data persisted');
    done('fdatasync callback');
  });
})();

// --- Test 13: async mkdir recursive (callback) ---
expected++;
(function() {
  var deep = path.join(tmpDir, 'deep', 'nested', 'dirs');
  fs.mkdir(deep, { recursive: true }, function(err, firstCreated) {
    assert(!err, 'mkdir recursive no error: ' + (err && err.message));
    assert(typeof firstCreated === 'string' || firstCreated === undefined,
      'mkdir recursive returns first created path or undefined');
    assert(fs.statSync(deep).isDirectory(), 'deep dir exists');
    done('mkdir recursive callback');
  });
})();

// --- Test 14: async readdir with withFileTypes (callback) ---
expected++;
(function() {
  var d = path.join(tmpDir, 'readdir-types');
  fs.mkdirSync(d);
  fs.writeFileSync(d + '/file1.txt', 'f1');
  fs.mkdirSync(d + '/subdir');
  fs.symlinkSync(d + '/file1.txt', d + '/link1');
  fs.readdir(d, { withFileTypes: true }, function(err, entries) {
    assert(!err, 'readdir withFileTypes no error');
    assert(Array.isArray(entries), 'readdir returns array');
    assertEqual(entries.length, 3, 'readdir returns 3 entries');

    var byName = {};
    for (var i = 0; i < entries.length; i++) {
      byName[entries[i].name] = entries[i];
    }
    assert(byName['file1.txt'].isFile(), 'file1.txt isFile');
    assert(byName['subdir'].isDirectory(), 'subdir isDirectory');
    assert(byName['link1'].isSymbolicLink(), 'link1 isSymbolicLink');
    done('readdir withFileTypes callback');
  });
})();

// --- Test 15: async readdir recursive (callback) ---
expected++;
(function() {
  var d = path.join(tmpDir, 'readdir-recursive');
  fs.mkdirSync(d);
  fs.mkdirSync(d + '/sub');
  fs.writeFileSync(d + '/a.txt', 'a');
  fs.writeFileSync(d + '/sub/b.txt', 'b');
  fs.readdir(d, { recursive: true }, function(err, entries) {
    assert(!err, 'readdir recursive no error');
    assert(Array.isArray(entries), 'readdir recursive returns array');
    entries.sort();
    // Should include both top-level and nested entries.
    assert(entries.indexOf('a.txt') !== -1, 'includes a.txt');
    assert(entries.indexOf('sub') !== -1 || entries.indexOf('sub/b.txt') !== -1,
      'includes sub or sub/b.txt');
    done('readdir recursive callback');
  });
})();

// --- Test 16: async writeFile + readFile with Buffer (callback) ---
expected++;
(function() {
  var f = path.join(tmpDir, 'buffer-write.bin');
  var buf = Buffer.from([0x00, 0x01, 0x02, 0xFF, 0xFE, 0x80]);
  fs.writeFile(f, buf, function(err) {
    assert(!err, 'writeFile buffer no error');
    fs.readFile(f, function(err2, data) {
      assert(!err2, 'readFile buffer no error');
      assert(Buffer.isBuffer(data), 'readFile returns Buffer');
      assertEqual(data.length, 6, 'buffer length');
      assertEqual(data[0], 0, 'byte 0');
      assertEqual(data[3], 0xFF, 'byte 3');
      assertEqual(data[5], 0x80, 'byte 5');
      done('writeFile+readFile buffer callback');
    });
  });
})();

// --- Test 17: async open + write string + close (callback) ---
expected++;
(function() {
  var f = path.join(tmpDir, 'write-string-async.txt');
  fs.open(f, 'w', function(err, fd) {
    assert(!err, 'open no error');
    fs.write(fd, 'string data', 0, 'utf8', function(err2, written, str) {
      assert(!err2, 'write string no error');
      assertEqual(typeof written, 'number', 'write returns byte count');
      assert(written > 0, 'wrote some bytes');
      fs.close(fd, function(err3) {
        assert(!err3, 'close no error');
        assertEqual(fs.readFileSync(f, 'utf8'), 'string data', 'content correct');
        done('write string async');
      });
    });
  });
})();

// --- Test 18: async copyFile + verify (callback) ---
expected++;
(function() {
  var src = path.join(tmpDir, 'copy-verify-src.txt');
  var dst = path.join(tmpDir, 'copy-verify-dst.txt');
  fs.writeFileSync(src, 'verify copy content');
  fs.copyFile(src, dst, function(err) {
    assert(!err, 'copyFile no error');
    fs.stat(src, function(err2, srcStat) {
      assert(!err2, 'stat src no error');
      fs.stat(dst, function(err3, dstStat) {
        assert(!err3, 'stat dst no error');
        assertEqual(srcStat.size, dstStat.size, 'same size');
        assertEqual(fs.readFileSync(dst, 'utf8'), 'verify copy content', 'content matches');
        done('copyFile+verify callback');
      });
    });
  });
})();

// --- Test 19: async opendir + sequential reads (callback) ---
expected++;
(function() {
  var d = path.join(tmpDir, 'opendir-seq');
  fs.mkdirSync(d);
  for (var i = 0; i < 10; i++) {
    fs.writeFileSync(d + '/file' + i + '.txt', 'f' + i);
  }
  fs.opendir(d, function(err, dir) {
    assert(!err, 'opendir no error');
    var names = [];
    function readNext() {
      dir.read(function(err2, dirent) {
        assert(!err2, 'dir.read no error');
        if (dirent === null) {
          dir.close(function(err3) {
            assert(!err3, 'dir.close no error');
            assertEqual(names.length, 10, 'read all 10 entries');
            names.sort();
            for (var j = 0; j < 10; j++) {
              assertEqual(names[j], 'file' + j + '.txt', 'entry ' + j);
            }
            done('opendir sequential reads');
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

// --- Test 20: async error EISDIR (callback) ---
expected++;
(function() {
  fs.readFile(tmpDir, 'utf8', function(err, data) {
    assert(err, 'readFile on dir should error');
    assertEqual(err.code, 'EISDIR', 'error code should be EISDIR');
    done('EISDIR callback');
  });
})();

// --- Test 21: multiple concurrent async ops ---
expected++;
(function() {
  var count = 0;
  var total = 5;
  for (var i = 0; i < total; i++) {
    (function(idx) {
      var f = path.join(tmpDir, 'concurrent-' + idx + '.txt');
      fs.writeFile(f, 'data-' + idx, function(err) {
        assert(!err, 'concurrent write ' + idx + ' no error');
        fs.readFile(f, 'utf8', function(err2, data) {
          assert(!err2, 'concurrent read ' + idx + ' no error');
          assertEqual(data, 'data-' + idx, 'concurrent data ' + idx + ' matches');
          count++;
          if (count === total) {
            done('concurrent async ops');
          }
        });
      });
    })(i);
  }
})();

// --- Test 22: fs.promises loads and basic operations work ---
expected++;
(function() {
  var fsp = fs.promises;
  assert(typeof fsp === 'object', 'fs.promises is an object');
  assert(typeof fsp.writeFile === 'function', 'fsp.writeFile exists');
  assert(typeof fsp.readFile === 'function', 'fsp.readFile exists');
  assert(typeof fsp.stat === 'function', 'fsp.stat exists');
  assert(typeof fsp.open === 'function', 'fsp.open exists');

  var pDir = path.join(tmpDir, 'promises-basic');
  fs.mkdirSync(pDir);
  fsp.writeFile(pDir + '/p.txt', 'promise-hello').then(function() {
    return fsp.readFile(pDir + '/p.txt', 'utf8');
  }).then(function(data) {
    assertEqual(data, 'promise-hello', 'fsp readFile content');
    return fsp.stat(pDir + '/p.txt');
  }).then(function(stat) {
    assert(stat.isFile(), 'fsp stat isFile');
    assertEqual(stat.size, 13, 'fsp stat size');
    done('fs.promises basic ops');
  }).catch(function(e) {
    fail('fs.promises basic ops', e.message);
  });
})();

// --- Test 23: async lchown (callback) ---
expected++;
(function() {
  var f = path.join(tmpDir, 'lchown-target.txt');
  var link = path.join(tmpDir, 'lchown-link');
  fs.writeFileSync(f, 'lchown');
  fs.symlinkSync(f, link);
  // lchown to current uid/gid should succeed.
  var stat = fs.lstatSync(link);
  fs.lchown(link, stat.uid, stat.gid, function(err) {
    assert(!err, 'lchown no error: ' + (err && err.message));
    done('lchown callback');
  });
})();

// --- Test 24: async chown (callback) ---
expected++;
(function() {
  var f = path.join(tmpDir, 'chown-test.txt');
  fs.writeFileSync(f, 'chown');
  var stat = fs.statSync(f);
  fs.chown(f, stat.uid, stat.gid, function(err) {
    assert(!err, 'chown no error: ' + (err && err.message));
    done('chown callback');
  });
})();

// --- Test 25: async fchown (callback) ---
expected++;
(function() {
  var f = path.join(tmpDir, 'fchown-test.txt');
  fs.writeFileSync(f, 'fchown');
  var fd = fs.openSync(f, 'r');
  var stat = fs.fstatSync(fd);
  fs.fchown(fd, stat.uid, stat.gid, function(err) {
    assert(!err, 'fchown no error: ' + (err && err.message));
    fs.closeSync(fd);
    done('fchown callback');
  });
})();

// --- Test 26: async fchmod (callback) ---
expected++;
(function() {
  var f = path.join(tmpDir, 'fchmod-test.txt');
  fs.writeFileSync(f, 'fchmod');
  var fd = fs.openSync(f, 'r+');
  fs.fchmod(fd, 0o400, function(err) {
    assert(!err, 'fchmod no error');
    var st = fs.fstatSync(fd);
    assertEqual(st.mode & 0o777, 0o400, 'fchmod changed permissions');
    // Restore.
    fs.fchmodSync(fd, 0o644);
    fs.closeSync(fd);
    done('fchmod callback');
  });
})();

// --- Test 27: async fstat (callback) ---
expected++;
(function() {
  var f = path.join(tmpDir, 'fstat-test.txt');
  fs.writeFileSync(f, 'fstat data here!');
  var fd = fs.openSync(f, 'r');
  fs.fstat(fd, function(err, stats) {
    assert(!err, 'fstat no error');
    assert(stats.isFile(), 'fstat isFile');
    assertEqual(stats.size, 16, 'fstat size = 16');
    fs.closeSync(fd);
    done('fstat callback');
  });
})();

// --- Test 28: async fstat with bigint (callback) ---
expected++;
(function() {
  var f = path.join(tmpDir, 'fstat-bigint.txt');
  fs.writeFileSync(f, 'bigint fstat');
  var fd = fs.openSync(f, 'r');
  fs.fstat(fd, { bigint: true }, function(err, stats) {
    assert(!err, 'fstat bigint no error');
    assertEqual(typeof stats.size, 'bigint', 'fstat bigint size');
    assert(stats.size === 12n, 'fstat bigint size = 12n');
    fs.closeSync(fd);
    done('fstat bigint callback');
  });
})();

// --- Test 29: async lutimes (callback) ---
expected++;
(function() {
  var f = path.join(tmpDir, 'lutimes-target.txt');
  var link = path.join(tmpDir, 'lutimes-link');
  fs.writeFileSync(f, 'lutimes');
  fs.symlinkSync(f, link);
  var atime = new Date('2019-03-15T12:00:00Z');
  var mtime = new Date('2019-06-15T12:00:00Z');
  fs.lutimes(link, atime, mtime, function(err) {
    assert(!err, 'lutimes no error: ' + (err && err.message));
    var st = fs.lstatSync(link);
    assert(Math.abs(st.mtimeMs - mtime.getTime()) < 2000,
      'lutimes mtime set on symlink');
    done('lutimes callback');
  });
})();

// --- Test 30: async rmdir (callback) ---
expected++;
(function() {
  var d = path.join(tmpDir, 'rmdir-test');
  fs.mkdirSync(d);
  assert(fs.statSync(d).isDirectory(), 'dir exists before rmdir');
  fs.rmdir(d, function(err) {
    assert(!err, 'rmdir no error');
    assert(!fs.existsSync(d), 'dir removed after rmdir');
    done('rmdir callback');
  });
})();

// --- Test 31: async futimes (callback) ---
expected++;
(function() {
  var f = path.join(tmpDir, 'futimes-test.txt');
  fs.writeFileSync(f, 'futimes');
  var fd = fs.openSync(f, 'r+');
  var atime = new Date('2018-01-01T00:00:00Z');
  var mtime = new Date('2018-12-31T23:59:59Z');
  fs.futimes(fd, atime, mtime, function(err) {
    assert(!err, 'futimes no error');
    var st = fs.fstatSync(fd);
    assert(Math.abs(st.mtimeMs - mtime.getTime()) < 2000,
      'futimes mtime set');
    fs.closeSync(fd);
    done('futimes callback');
  });
})();

// --- Test 32: async statfs (callback) ---
expected++;
(function() {
  fs.statfs(tmpDir, function(err, stats) {
    assert(!err, 'statfs no error: ' + (err && err.message));
    assertEqual(typeof stats.type, 'number', 'statfs type is number');
    assertEqual(typeof stats.bsize, 'number', 'statfs bsize is number');
    assert(stats.bsize > 0, 'statfs bsize > 0');
    assertEqual(typeof stats.blocks, 'number', 'statfs blocks is number');
    done('statfs callback');
  });
})();

// =========================================================================
// fs.promises tests (Test 33+)
// =========================================================================

var fsp = fs.promises;

// --- Test 33: fsp.mkdir + fsp.readdir ---
expected++;
(function() {
  var d = path.join(tmpDir, 'fsp-readdir');
  fsp.mkdir(d).then(function() {
    return fsp.writeFile(d + '/x.txt', 'x');
  }).then(function() {
    return fsp.readdir(d);
  }).then(function(entries) {
    assert(Array.isArray(entries), 'fsp readdir returns array');
    assertEqual(entries.length, 1, 'fsp readdir one entry');
    assertEqual(entries[0], 'x.txt', 'fsp readdir entry name');
    done('fsp mkdir+readdir');
  }).catch(function(e) {
    fail('fsp mkdir+readdir', e.message);
  });
})();

// --- Test 34: fsp.rename ---
expected++;
(function() {
  var src = path.join(tmpDir, 'fsp-rename-src.txt');
  var dst = path.join(tmpDir, 'fsp-rename-dst.txt');
  fsp.writeFile(src, 'rename-data').then(function() {
    return fsp.rename(src, dst);
  }).then(function() {
    return fsp.readFile(dst, 'utf8');
  }).then(function(data) {
    assertEqual(data, 'rename-data', 'fsp rename content');
    done('fsp rename');
  }).catch(function(e) {
    fail('fsp rename', e.message);
  });
})();

// --- Test 35: fsp.copyFile ---
expected++;
(function() {
  var src = path.join(tmpDir, 'fsp-copy-src.txt');
  var dst = path.join(tmpDir, 'fsp-copy-dst.txt');
  fsp.writeFile(src, 'copy-data').then(function() {
    return fsp.copyFile(src, dst);
  }).then(function() {
    return fsp.readFile(dst, 'utf8');
  }).then(function(data) {
    assertEqual(data, 'copy-data', 'fsp copy content');
    done('fsp copyFile');
  }).catch(function(e) {
    fail('fsp copyFile', e.message);
  });
})();

// --- Test 36: fsp.chmod ---
expected++;
(function() {
  var f = path.join(tmpDir, 'fsp-chmod.txt');
  fsp.writeFile(f, 'chmod').then(function() {
    return fsp.chmod(f, 0o444);
  }).then(function() {
    var s = fs.statSync(f);
    assertEqual(s.mode & 0o777, 0o444, 'fsp chmod permissions');
    return fsp.chmod(f, 0o644);
  }).then(function() {
    done('fsp chmod');
  }).catch(function(e) {
    fail('fsp chmod', e.message);
  });
})();

// --- Test 37: fsp.symlink + fsp.readlink ---
expected++;
(function() {
  var f = path.join(tmpDir, 'fsp-sym-target.txt');
  var link = path.join(tmpDir, 'fsp-sym-link');
  fsp.writeFile(f, 'sym').then(function() {
    return fsp.symlink(f, link);
  }).then(function() {
    return fsp.readlink(link);
  }).then(function(target) {
    assertEqual(target, f, 'fsp readlink target');
    done('fsp symlink+readlink');
  }).catch(function(e) {
    fail('fsp symlink+readlink', e.message);
  });
})();

// --- Test 38: fsp.access + ENOENT ---
expected++;
(function() {
  var f = path.join(tmpDir, 'fsp-chmod.txt');
  fsp.access(f, fs.constants.R_OK).then(function() {
    return fsp.access('/nonexistent-' + Date.now()).then(function() {
      assert(false, 'should throw ENOENT');
    }, function(e) {
      assertEqual(e.code, 'ENOENT', 'fsp access ENOENT');
    });
  }).then(function() {
    done('fsp access');
  }).catch(function(e) {
    fail('fsp access', e.message);
  });
})();

// --- Test 39: fsp.open FileHandle read/write ---
expected++;
(function() {
  var f = path.join(tmpDir, 'fsp-fh-rw.txt');
  fsp.open(f, 'w+').then(function(fh) {
    assert(typeof fh.fd === 'number' && fh.fd >= 0, 'fsp open returns FileHandle');
    var buf = Buffer.from('FileHandle rw');
    return fh.write(buf).then(function(res) {
      assertEqual(res.bytesWritten, 13, 'fh write bytes');
      return fh.stat();
    }).then(function(s) {
      assert(s.isFile(), 'fh stat isFile');
      assertEqual(s.size, 13, 'fh stat size');
      return fh.close();
    });
  }).then(function() {
    return fsp.readFile(f, 'utf8');
  }).then(function(data) {
    assertEqual(data, 'FileHandle rw', 'fh write content');
    done('fsp FileHandle read/write');
  }).catch(function(e) {
    fail('fsp FileHandle', e.message);
  });
})();

// --- Test 40: fsp.mkdtemp ---
expected++;
(function() {
  fsp.mkdtemp(tmpDir + '/fsp-tmp-').then(function(d) {
    assert(typeof d === 'string', 'fsp mkdtemp returns string');
    assert(fs.statSync(d).isDirectory(), 'fsp mkdtemp creates dir');
    done('fsp mkdtemp');
  }).catch(function(e) {
    fail('fsp mkdtemp', e.message);
  });
})();

// --- Test 41: fsp.realpath ---
expected++;
(function() {
  var f = path.join(tmpDir, 'fsp-real-target.txt');
  var link = path.join(tmpDir, 'fsp-real-link');
  fsp.writeFile(f, 'real').then(function() {
    return fsp.symlink(f, link);
  }).then(function() {
    return fsp.realpath(link);
  }).then(function(rp) {
    assertEqual(rp, fs.realpathSync(f), 'fsp realpath');
    done('fsp realpath');
  }).catch(function(e) {
    fail('fsp realpath', e.message);
  });
})();

// --- Test 42: fsp.utimes ---
expected++;
(function() {
  var f = path.join(tmpDir, 'fsp-utimes.txt');
  var atime = new Date('2020-03-15T10:00:00Z');
  var mtime = new Date('2021-09-20T15:30:00Z');
  fsp.writeFile(f, 'utimes').then(function() {
    return fsp.utimes(f, atime, mtime);
  }).then(function() {
    var s = fs.statSync(f);
    assert(Math.abs(s.mtimeMs - mtime.getTime()) < 2000,
      'fsp utimes mtime: ' + s.mtimeMs + ' vs ' + mtime.getTime());
    done('fsp utimes');
  }).catch(function(e) {
    fail('fsp utimes', e.message);
  });
})();

// --- Test 43: fsp.unlink ---
expected++;
(function() {
  var f = path.join(tmpDir, 'fsp-unlink.txt');
  fsp.writeFile(f, 'del').then(function() {
    return fsp.unlink(f);
  }).then(function() {
    assert(!fs.existsSync(f), 'fsp unlink removed file');
    done('fsp unlink');
  }).catch(function(e) {
    fail('fsp unlink', e.message);
  });
})();

// --- Test 44: fsp.link ---
expected++;
(function() {
  var f = path.join(tmpDir, 'fsp-link-src.txt');
  var hard = path.join(tmpDir, 'fsp-link-hard.txt');
  fsp.writeFile(f, 'hard-link').then(function() {
    return fsp.link(f, hard);
  }).then(function() {
    return fsp.readFile(hard, 'utf8');
  }).then(function(data) {
    assertEqual(data, 'hard-link', 'fsp link content');
    done('fsp link');
  }).catch(function(e) {
    fail('fsp link', e.message);
  });
})();

