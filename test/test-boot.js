// Copyright (c) Tzvetan Mikov.
// Minimal bootstrap test -- verifies that the runtime, console, and process
// are properly wired up.
'use strict';

console.log('platform:', process.platform);
console.log('pid:', process.pid);
console.log('cwd:', process.cwd());

// Basic sanity checks.
if (typeof process.platform !== 'string' || process.platform.length === 0) {
  throw new Error('process.platform is not a non-empty string');
}
if (typeof process.pid !== 'number' || process.pid <= 0) {
  throw new Error('process.pid is not a positive number');
}
if (typeof process.cwd() !== 'string' || process.cwd().length === 0) {
  throw new Error('process.cwd() is not a non-empty string');
}

// Verify process.argv exists and has the expected structure.
if (!Array.isArray(process.argv) || process.argv.length < 2) {
  throw new Error('process.argv should have at least 2 entries');
}

// Verify process.version is set.
if (typeof process.version !== 'string') {
  throw new Error('process.version is not a string');
}

// Verify console.error writes to stderr (won't crash at least).
console.error('stderr test');

console.log('PASS');
