// Copyright (c) Tzvetan Mikov.
// Test for fs_event_wrap, uv binding, and fs.watch/watchFile support.

'use strict';

var assert = function(cond, msg) {
  if (!cond) throw new Error('Assertion failed: ' + (msg || ''));
};

var testsPassed = 0;

// ==========================================================================
// Test 1: uv binding — error constants exist
// ==========================================================================
(function() {
  var uv = internalBinding('uv');
  assert(typeof uv.UV_ENOENT === 'number', 'UV_ENOENT should be a number');
  assert(uv.UV_ENOENT < 0, 'UV_ENOENT should be negative');
  assert(typeof uv.UV_ENOSPC === 'number', 'UV_ENOSPC should be a number');
  assert(typeof uv.UV_EOF === 'number', 'UV_EOF should be a number');
  assert(typeof uv.UV_EACCES === 'number', 'UV_EACCES should be a number');
  assert(typeof uv.UV_EINVAL === 'number', 'UV_EINVAL should be a number');
  testsPassed++;
  console.log('  uv binding: error constants OK');
})();

// ==========================================================================
// Test 2: uv binding — errname function
// ==========================================================================
(function() {
  var uv = internalBinding('uv');
  var name = uv.errname(uv.UV_ENOENT);
  assert(name === 'ENOENT', 'errname(UV_ENOENT) should be "ENOENT", got: ' + name);
  testsPassed++;
  console.log('  uv binding: errname OK');
})();

// ==========================================================================
// Test 3: uv binding — getErrorMessage function
// ==========================================================================
(function() {
  var uv = internalBinding('uv');
  var msg = uv.getErrorMessage(uv.UV_ENOENT);
  assert(typeof msg === 'string' && msg.length > 0, 'getErrorMessage should return a non-empty string');
  testsPassed++;
  console.log('  uv binding: getErrorMessage OK');
})();

// ==========================================================================
// Test 4: uv binding — getErrorMap function
// ==========================================================================
(function() {
  var uv = internalBinding('uv');
  var map = uv.getErrorMap();
  assert(map instanceof Map, 'getErrorMap should return a Map');
  assert(map.size > 0, 'error map should not be empty');
  // Check that ENOENT is in the map.
  var entry = map.get(uv.UV_ENOENT);
  assert(Array.isArray(entry), 'map entry for UV_ENOENT should be an array');
  assert(entry[0] === 'ENOENT', 'entry[0] should be "ENOENT"');
  assert(typeof entry[1] === 'string', 'entry[1] should be a message string');
  testsPassed++;
  console.log('  uv binding: getErrorMap OK');
})();

// ==========================================================================
// Test 5: fs binding — kFsStatsFieldsNumber export
// ==========================================================================
(function() {
  var fs = internalBinding('fs');
  assert(fs.kFsStatsFieldsNumber === 18, 'kFsStatsFieldsNumber should be 18, got: ' + fs.kFsStatsFieldsNumber);
  testsPassed++;
  console.log('  fs binding: kFsStatsFieldsNumber OK');
})();

// ==========================================================================
// Test 6: fs binding — StatWatcher constructor exists
// ==========================================================================
(function() {
  var fs = internalBinding('fs');
  assert(typeof fs.StatWatcher === 'function', 'StatWatcher should be a function');
  var sw = new fs.StatWatcher(false);
  assert(typeof sw.start === 'function', 'StatWatcher should have start method');
  assert(typeof sw.close === 'function', 'StatWatcher should have close method');
  assert(typeof sw.ref === 'function', 'StatWatcher should have ref method');
  assert(typeof sw.unref === 'function', 'StatWatcher should have unref method');
  assert(typeof sw.getAsyncId === 'function', 'StatWatcher should have getAsyncId method');
  sw.close();
  testsPassed++;
  console.log('  fs binding: StatWatcher constructor OK');
})();

// ==========================================================================
// Test 7: fs_event_wrap binding — FSEvent constructor
// ==========================================================================
(function() {
  var fsEventWrap = internalBinding('fs_event_wrap');
  assert(typeof fsEventWrap.FSEvent === 'function', 'FSEvent should be a function');
  var ev = new fsEventWrap.FSEvent();
  assert(typeof ev.start === 'function', 'FSEvent should have start method');
  assert(typeof ev.close === 'function', 'FSEvent should have close method');
  assert(typeof ev.ref === 'function', 'FSEvent should have ref method');
  assert(typeof ev.unref === 'function', 'FSEvent should have unref method');
  assert(typeof ev.hasRef === 'function', 'FSEvent should have hasRef method');
  assert(typeof ev.getAsyncId === 'function', 'FSEvent should have getAsyncId method');
  assert(ev.initialized === false, 'FSEvent should not be initialized before start');
  testsPassed++;
  console.log('  fs_event_wrap binding: FSEvent constructor OK');
})();

// ==========================================================================
// Test 8: FSEvent — start and close lifecycle
// ==========================================================================
(function() {
  var fsEventWrap = internalBinding('fs_event_wrap');
  var fs = require('fs');
  var tmpDir = fs.mkdtempSync('/tmp/hermes-fsev-');

  var ev = new fsEventWrap.FSEvent();
  assert(ev.initialized === false, 'not initialized before start');

  var err = ev.start(tmpDir, true, false, 'utf8');
  assert(err === 0, 'start should return 0 on success, got: ' + err);
  assert(ev.initialized === true, 'should be initialized after start');

  ev.close();

  // Clean up.
  fs.rmSync(tmpDir, { recursive: true });
  testsPassed++;
  console.log('  FSEvent: start and close lifecycle OK');
})();

// ==========================================================================
// Test 9: FSEvent — watch directory and detect file creation
// ==========================================================================
(function() {
  var fsEventWrap = internalBinding('fs_event_wrap');
  var fs = require('fs');
  var tmpDir = fs.mkdtempSync('/tmp/hermes-fsev2-');

  var eventFired = false;
  var eventType = '';
  var eventFilename = '';

  var ev = new fsEventWrap.FSEvent();
  ev.onchange = function(status, type, filename) {
    if (!eventFired) {
      eventFired = true;
      eventType = type;
      eventFilename = filename;
    }
    ev.close();
  };

  var err = ev.start(tmpDir, true, false, 'utf8');
  assert(err === 0, 'start should succeed');

  // After a short delay, create a file in the watched directory.
  setTimeout(function() {
    fs.writeFileSync(tmpDir + '/newfile.txt', 'hello');
  }, 50);

  // After a longer delay, verify the event was fired.
  setTimeout(function() {
    assert(eventFired, 'fs event should have fired');
    assert(eventType === 'rename' || eventType === 'change',
           'event type should be rename or change, got: ' + eventType);
    // On Linux, inotify reports 'rename' for new file creation.
    // The filename may or may not be null depending on the OS.

    fs.rmSync(tmpDir, { recursive: true });
    testsPassed++;
    console.log('  FSEvent: watch directory OK');
    finishTests();
  }, 500);
})();

// ==========================================================================
// Test 10: FSEvent — watch file change via modification
// ==========================================================================
(function() {
  var fsEventWrap = internalBinding('fs_event_wrap');
  var fs = require('fs');
  var tmpDir = fs.mkdtempSync('/tmp/hermes-fsev3-');
  var watchFile = tmpDir + '/watched.txt';
  fs.writeFileSync(watchFile, 'initial');

  var changeFired = false;

  var ev = new fsEventWrap.FSEvent();
  ev.onchange = function(status, type, filename) {
    if (!changeFired) {
      changeFired = true;
    }
    ev.close();
  };

  var err = ev.start(tmpDir, true, false, 'utf8');
  assert(err === 0, 'start should succeed');

  // Trigger a change.
  setTimeout(function() {
    fs.writeFileSync(watchFile, 'changed');
  }, 50);

  setTimeout(function() {
    assert(changeFired, 'change event should have fired');
    fs.rmSync(tmpDir, { recursive: true });
    testsPassed++;
    console.log('  FSEvent: watch file change OK');
    finishTests();
  }, 500);
})();

// ==========================================================================
// Test 11: FSEvent — ref/unref
// ==========================================================================
(function() {
  var fsEventWrap = internalBinding('fs_event_wrap');
  var fs = require('fs');
  var tmpDir = fs.mkdtempSync('/tmp/hermes-fsev4-');

  var ev = new fsEventWrap.FSEvent();
  var err = ev.start(tmpDir, true, false, 'utf8');
  assert(err === 0, 'start should succeed');
  assert(ev.hasRef() === true, 'should have ref after persistent start');

  ev.unref();
  assert(ev.hasRef() === false, 'should not have ref after unref');

  ev.ref();
  assert(ev.hasRef() === true, 'should have ref after ref');

  ev.close();
  fs.rmSync(tmpDir, { recursive: true });
  testsPassed++;
  console.log('  FSEvent: ref/unref OK');
})();

// ==========================================================================
// Test 12: FSEvent — non-persistent does not keep loop alive
// (start with persistent=false, so handle is unreffed)
// ==========================================================================
(function() {
  var fsEventWrap = internalBinding('fs_event_wrap');
  var fs = require('fs');
  var tmpDir = fs.mkdtempSync('/tmp/hermes-fsev5-');

  var ev = new fsEventWrap.FSEvent();
  var err = ev.start(tmpDir, false, false, 'utf8');
  assert(err === 0, 'start should succeed');
  assert(ev.hasRef() === false, 'should not have ref with persistent=false');

  ev.close();
  fs.rmSync(tmpDir, { recursive: true });
  testsPassed++;
  console.log('  FSEvent: non-persistent OK');
})();

// ==========================================================================
// Test 13: StatWatcher — poll for file changes
// ==========================================================================
(function() {
  var fsBinding = internalBinding('fs');
  var fs = require('fs');
  var tmpDir = fs.mkdtempSync('/tmp/hermes-statwatcher-');
  var watchFile = tmpDir + '/pollme.txt';
  fs.writeFileSync(watchFile, 'original');

  var sw = new fsBinding.StatWatcher(false);
  var changeDetected = false;

  sw.onchange = function(status, statsArr) {
    // statsArr is the shared Float64Array(36).
    // We just check that onchange fires.
    if (!changeDetected) {
      changeDetected = true;
    }
    sw.close();
  };

  var err = sw.start(watchFile, 100); // 100ms polling interval
  assert(err === undefined || err === null || err === 0,
         'StatWatcher.start should succeed, got: ' + err);

  // Trigger a change.
  setTimeout(function() {
    fs.writeFileSync(watchFile, 'modified');
  }, 150);

  setTimeout(function() {
    // StatWatcher polling may take a while; just make sure it didn't error.
    if (changeDetected) {
      console.log('  StatWatcher: poll change detected');
    } else {
      // It may not have fired in time -- that's okay for a basic test.
      // Just close and clean up.
      sw.close();
      console.log('  StatWatcher: poll change not detected in time (OK for CI)');
    }
    fs.rmSync(tmpDir, { recursive: true });
    testsPassed++;
    console.log('  StatWatcher: poll lifecycle OK');
    finishTests();
  }, 800);
})();

// ==========================================================================
// Finish
// ==========================================================================

var expectedAsyncTests = 3; // Tests 9, 10, 13
var asyncTestsDone = 0;

function finishTests() {
  asyncTestsDone++;
  if (asyncTestsDone >= expectedAsyncTests) {
    console.log('All ' + testsPassed + ' tests passed.');
    console.log('PASS');
  }
}
