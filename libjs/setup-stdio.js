// Copyright (c) Tzvetan Mikov.
//
// Sets up process.stdin, process.stdout, process.stderr as proper streams.
// Adapted from Node's internal/bootstrap/switches/is_main_thread.js.
// Called during hermes-node bootstrap after module loader and timers are ready.

'use strict';

(function setupStdio() {
  var ObjectDefineProperty = globalThis.primordials.ObjectDefineProperty;
  var guessHandleType = require('internal/util').guessHandleType;

  var stdin;
  var stdout;
  var stderr;

  var stdoutDestroy;
  var stderrDestroy;

  function dummyDestroy(err, cb) {
    cb(err);
    // _undestroy restores the stream to a writable state after being destroyed,
    // which prevents stdout/stderr from becoming permanently broken.
    if (typeof this._undestroy === 'function') {
      this._undestroy();
    }
    // Emit 'close' for pipeline/finished support.
    if (this._writableState && !this._writableState.emitClose) {
      process.nextTick(() => {
        this.emit('close');
      });
    }
  }

  function createWritableStdioStream(fd) {
    var stream;
    switch (guessHandleType(fd)) {
      case 'TTY': {
        var tty = require('tty');
        stream = new tty.WriteStream(fd);
        stream._type = 'tty';
        break;
      }

      case 'FILE': {
        var SyncWriteStream = require('internal/fs/sync_write_stream');
        stream = new SyncWriteStream(fd, { autoClose: false });
        stream._type = 'fs';
        break;
      }

      case 'PIPE':
      case 'TCP': {
        var net = require('net');
        stream = new net.Socket({
          fd: fd,
          readable: false,
          writable: true,
        });
        stream._type = 'pipe';
        break;
      }

      default: {
        // Fallback: dummy writable stream.
        var Writable = require('stream').Writable;
        stream = new Writable({
          write: function(buf, enc, cb) { cb(); },
        });
      }
    }

    stream.fd = fd;
    stream._isStdio = true;
    return stream;
  }

  function getStdout() {
    if (stdout) return stdout;
    stdout = createWritableStdioStream(1);
    stdout.destroySoon = stdout.destroy;
    stdoutDestroy = stdout._destroy;
    stdout._destroy = dummyDestroy;
    return stdout;
  }

  function getStderr() {
    if (stderr) return stderr;
    stderr = createWritableStdioStream(2);
    stderr.destroySoon = stderr.destroy;
    stderrDestroy = stderr._destroy;
    stderr._destroy = dummyDestroy;
    return stderr;
  }

  function getStdin() {
    if (stdin) return stdin;
    var fd = 0;

    switch (guessHandleType(fd)) {
      case 'TTY': {
        var tty = require('tty');
        stdin = new tty.ReadStream(fd);
        break;
      }

      case 'FILE': {
        var fs = require('fs');
        stdin = new fs.ReadStream(null, { fd: fd, autoClose: false });
        break;
      }

      case 'PIPE':
      case 'TCP': {
        var net = require('net');
        stdin = new net.Socket({
          fd: fd,
          readable: true,
          writable: false,
          manualStart: true,
        });
        // Prevent stdin from being .end()-ed.
        stdin._writableState.ended = true;
        break;
      }

      default: {
        // Fallback: empty readable stream.
        var Readable = require('stream').Readable;
        stdin = new Readable({ read: function() {} });
        stdin.push(null);
      }
    }

    stdin.fd = fd;

    // stdin starts paused. Explicitly readStop() to put it in not-reading state.
    if (stdin._handle && stdin._handle.readStop) {
      stdin._handle.reading = false;
      stdin._readableState.reading = false;
      stdin._handle.readStop();
    }

    // When user calls stdin.pause(), stop reading at OS level so the
    // process can exit cleanly.
    stdin.on('pause', function() {
      process.nextTick(function onpause() {
        if (!stdin._handle)
          return;
        if (stdin._handle.reading && !stdin.readableFlowing) {
          stdin._readableState.reading = false;
          stdin._handle.reading = false;
          stdin._handle.readStop();
        }
      });
    });

    return stdin;
  }

  // Define lazy getters on the process object.
  ObjectDefineProperty(process, 'stdout', {
    configurable: true,
    enumerable: true,
    get: getStdout,
  });

  ObjectDefineProperty(process, 'stderr', {
    configurable: true,
    enumerable: true,
    get: getStderr,
  });

  ObjectDefineProperty(process, 'stdin', {
    configurable: true,
    enumerable: true,
    get: getStdin,
  });
})();
