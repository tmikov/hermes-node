// Copyright (c) Tzvetan Mikov.
// RUN: %hermes-node %s | %FileCheck %s
// CHECK: PASS
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

// --- os UV_UDP constants ---
if (typeof c.os.UV_UDP_REUSEADDR !== 'number')
  throw new Error('UV_UDP_REUSEADDR should be a number');
if (c.os.UV_UDP_REUSEADDR !== 4)
  throw new Error('UV_UDP_REUSEADDR should be 4');
if (typeof c.os.UV_UDP_IPV6ONLY !== 'number')
  throw new Error('UV_UDP_IPV6ONLY should be a number');
if (c.os.UV_UDP_IPV6ONLY !== 1)
  throw new Error('UV_UDP_IPV6ONLY should be 1');
if (typeof c.os.UV_UDP_PARTIAL !== 'number')
  throw new Error('UV_UDP_PARTIAL should be a number');
if (c.os.UV_UDP_PARTIAL !== 2)
  throw new Error('UV_UDP_PARTIAL should be 2');
// UV_UDP_REUSEPORT may not be defined on all platforms.
if (c.os.UV_UDP_REUSEPORT !== undefined &&
    typeof c.os.UV_UDP_REUSEPORT !== 'number')
  throw new Error('UV_UDP_REUSEPORT should be a number if defined');

// --- os socket type constants ---
if (typeof c.os.SOCK_STREAM !== 'number')
  throw new Error('SOCK_STREAM should be a number');
if (typeof c.os.SOCK_DGRAM !== 'number')
  throw new Error('SOCK_DGRAM should be a number');
if (typeof c.os.SOCK_RAW !== 'number')
  throw new Error('SOCK_RAW should be a number');

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

// --- Verify os module exposes constants ---
var os = require('os');
if (typeof os.constants !== 'object')
  throw new Error('os.constants should be an object');
if (typeof os.constants.signals !== 'object')
  throw new Error('os.constants.signals should be an object');
if (typeof os.constants.errno !== 'object')
  throw new Error('os.constants.errno should be an object');
if (typeof os.constants.UV_UDP_REUSEADDR !== 'number')
  throw new Error('os.constants.UV_UDP_REUSEADDR should be a number');
if (typeof os.constants.SOCK_STREAM !== 'number')
  throw new Error('os.constants.SOCK_STREAM should be a number');
if (typeof os.constants.SOCK_DGRAM !== 'number')
  throw new Error('os.constants.SOCK_DGRAM should be a number');

console.log('PASS');
