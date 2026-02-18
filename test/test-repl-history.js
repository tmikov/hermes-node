// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// R21: Verify REPL history persistence.
// Tests that the REPL reads/writes history from/to a file via setupHistory().
// History only works with terminal: true (in non-terminal mode, [kLine] and
// thus addHistory are never called -- this matches Node's behavior).

// RUN: %hermes-node %s

var assert = require('assert');
var fs = require('fs');
var path = require('path');
var os = require('os');
var { Readable, Writable } = require('stream');
var repl = require('repl');

var tmpDir = fs.mkdtempSync(path.join(os.tmpdir(), 'hermes-repl-hist-'));
var testsRemaining = 5;
var testsPassed = 0;

function testDone(name) {
  testsPassed++;
  testsRemaining--;
  if (testsRemaining === 0) {
    // Cleanup temp directory.
    try {
      var files = fs.readdirSync(tmpDir);
      for (var i = 0; i < files.length; i++) {
        fs.unlinkSync(path.join(tmpDir, files[i]));
      }
      fs.rmdirSync(tmpDir);
    } catch (e) {
      // Ignore cleanup errors.
    }
    assert.strictEqual(testsPassed, 5, 'Expected all 5 tests to pass');
    console.log('PASS');
  }
}

// Helper: create a REPL with terminal: true (needed for history to work),
// call setupHistory with a file path, enter lines via write(), then close.
function runREPLWithHistory(histFile, inputLines, onReady, onExit) {
  var input = new Readable({ read() {} });
  var outputStream = new Writable({
    write(chunk, enc, cb) { cb(); }
  });

  var r = repl.start({
    input: input,
    output: outputStream,
    useGlobal: false,
    terminal: true,
    prompt: '> ',
  });

  r.setupHistory({
    filePath: histFile,
    size: 100,
    onHistoryFileLoaded: function(err, server) {
      if (err) {
        onExit(err);
        return;
      }
      onReady(r, input);
    }
  });

  r.on('exit', function() {
    onExit(null);
  });

  return r;
}

// Test 1: History is written to file.
(function test1() {
  var histFile = path.join(tmpDir, 'hist1.txt');
  runREPLWithHistory(histFile, null, function(r, input) {
    r.write('var x = 1\n');
    r.write('var y = 2\n');
    // Wait for debounced flush (15ms + async write).
    setTimeout(function() {
      var hist = fs.readFileSync(histFile, 'utf8');
      assert(hist.includes('var x = 1'), 'Test 1: History should contain "var x = 1", got: ' + JSON.stringify(hist));
      assert(hist.includes('var y = 2'), 'Test 1: History should contain "var y = 2", got: ' + JSON.stringify(hist));
      r.write('.exit\n');
    }, 200);
  }, function(err) {
    assert.ifError(err);
    testDone('history written');
  });
})();

// Test 2: History is loaded from existing file.
(function test2() {
  var histFile = path.join(tmpDir, 'hist2.txt');
  fs.writeFileSync(histFile, 'prev_cmd_1\nprev_cmd_2\n');

  runREPLWithHistory(histFile, null, function(r, input) {
    // After loading, the history array should contain previous entries.
    var history = r.history;
    assert(
      history.indexOf('prev_cmd_1') !== -1,
      'Test 2: History should contain "prev_cmd_1", got: ' + JSON.stringify(history)
    );
    assert(
      history.indexOf('prev_cmd_2') !== -1,
      'Test 2: History should contain "prev_cmd_2", got: ' + JSON.stringify(history)
    );
    r.write('.exit\n');
  }, function(err) {
    assert.ifError(err);
    testDone('history loaded');
  });
})();

// Test 3: New entries are appended to loaded history.
(function test3() {
  var histFile = path.join(tmpDir, 'hist3.txt');
  fs.writeFileSync(histFile, 'old_entry\n');

  runREPLWithHistory(histFile, null, function(r, input) {
    r.write('new_entry\n');
    setTimeout(function() {
      var hist = fs.readFileSync(histFile, 'utf8');
      // The history file should contain both old and new entries.
      assert(hist.includes('new_entry'), 'Test 3: History should contain "new_entry", got: ' + JSON.stringify(hist));
      assert(hist.includes('old_entry'), 'Test 3: History should contain "old_entry", got: ' + JSON.stringify(hist));
      r.write('.exit\n');
    }, 200);
  }, function(err) {
    assert.ifError(err);
    testDone('history appended');
  });
})();

// Test 4: Empty filePath disables history persistence.
(function test4() {
  var histFile = path.join(tmpDir, 'hist4.txt');
  var input = new Readable({ read() {} });
  var outputStream = new Writable({
    write(chunk, enc, cb) { cb(); }
  });

  var r = repl.start({
    input: input,
    output: outputStream,
    useGlobal: false,
    terminal: true,
    prompt: '> ',
  });

  r.setupHistory({
    filePath: '',
    size: 100,
    onHistoryFileLoaded: function(err, server) {
      assert.ifError(err);
      r.write('should_not_persist\n');
      setTimeout(function() {
        // File should not have been created.
        assert(!fs.existsSync(histFile), 'Test 4: History file should not exist when filePath is empty');
        r.write('.exit\n');
      }, 200);
    }
  });

  r.on('exit', function() {
    testDone('history disabled');
  });
})();

// Test 5: createInternalRepl uses NODE_REPL_HISTORY env var.
(function test5() {
  var histFile = path.join(tmpDir, 'hist5.txt');
  var cliRepl = require('internal/repl');
  var input = new Readable({ read() {} });
  var outputStream = new Writable({
    write(chunk, enc, cb) { cb(); }
  });

  var testEnv = { NODE_REPL_HISTORY: histFile };

  cliRepl.createInternalRepl(testEnv, {
    input: input,
    output: outputStream,
    terminal: true,
  }, function(err, r) {
    assert.ifError(err);
    r.write('env_test_cmd\n');
    setTimeout(function() {
      var hist = fs.readFileSync(histFile, 'utf8');
      assert(hist.includes('env_test_cmd'), 'Test 5: History should contain "env_test_cmd", got: ' + JSON.stringify(hist));
      r.on('exit', function() {
        testDone('createInternalRepl history');
      });
      r.write('.exit\n');
    }, 200);
  });
})();
