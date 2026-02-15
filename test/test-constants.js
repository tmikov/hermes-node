// Copyright (c) Tzvetan Mikov.
// Test the constants binding.
'use strict';

var c = internalBinding('constants');

// --- os.signals ---
if (typeof c.os.signals.SIGINT !== 'number')
  throw new Error('SIGINT should be a number');
if (c.os.signals.SIGINT !== 2)
  throw new Error('SIGINT should be 2');
if (typeof c.os.signals.SIGTERM !== 'number')
  throw new Error('SIGTERM should be a number');
if (c.os.signals.SIGTERM !== 15)
  throw new Error('SIGTERM should be 15');
if (typeof c.os.signals.SIGKILL !== 'number')
  throw new Error('SIGKILL should be a number');

// --- os.errno ---
if (typeof c.os.errno.ENOENT !== 'number')
  throw new Error('ENOENT should be a number');
if (typeof c.os.errno.EACCES !== 'number')
  throw new Error('EACCES should be a number');
if (typeof c.os.errno.EEXIST !== 'number')
  throw new Error('EEXIST should be a number');
if (typeof c.os.errno.EINVAL !== 'number')
  throw new Error('EINVAL should be a number');

// --- os.priority ---
if (typeof c.os.priority !== 'object')
  throw new Error('os.priority should be an object');

// --- os.dlopen ---
if (typeof c.os.dlopen !== 'object')
  throw new Error('os.dlopen should be an object');

// --- fs ---
if (typeof c.fs.O_RDONLY !== 'number')
  throw new Error('O_RDONLY should be a number');
if (c.fs.O_RDONLY !== 0)
  throw new Error('O_RDONLY should be 0');
if (typeof c.fs.O_WRONLY !== 'number')
  throw new Error('O_WRONLY should be a number');
if (typeof c.fs.O_RDWR !== 'number')
  throw new Error('O_RDWR should be a number');
if (typeof c.fs.S_IFREG !== 'number')
  throw new Error('S_IFREG should be a number');
if (typeof c.fs.S_IFDIR !== 'number')
  throw new Error('S_IFDIR should be a number');
if (typeof c.fs.S_IFMT !== 'number')
  throw new Error('S_IFMT should be a number');

// Access flags.
if (typeof c.fs.F_OK !== 'number')
  throw new Error('F_OK should be a number');
if (typeof c.fs.R_OK !== 'number')
  throw new Error('R_OK should be a number');
if (typeof c.fs.W_OK !== 'number')
  throw new Error('W_OK should be a number');
if (typeof c.fs.X_OK !== 'number')
  throw new Error('X_OK should be a number');

// UV dirent types.
if (typeof c.fs.UV_DIRENT_FILE !== 'number')
  throw new Error('UV_DIRENT_FILE should be a number');
if (typeof c.fs.UV_DIRENT_DIR !== 'number')
  throw new Error('UV_DIRENT_DIR should be a number');

// --- crypto (stubbed, should be empty object) ---
if (typeof c.crypto !== 'object')
  throw new Error('crypto should be an object');

// --- zlib (stubbed, should be empty object) ---
if (typeof c.zlib !== 'object')
  throw new Error('zlib should be an object');

// --- trace (stubbed, should be empty object) ---
if (typeof c.trace !== 'object')
  throw new Error('trace should be an object');

console.log('PASS');
