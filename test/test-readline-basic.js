// Copyright (c) Tzvetan Mikov.
// Test that readline module loads and basic line reading works.
// RUN: %hermes-node %s | %FileCheck %s

'use strict';

const readline = require('readline');
const { Readable } = require('stream');

// Verify exports exist.
console.log(typeof readline.createInterface);
// CHECK: function
console.log(typeof readline.clearLine);
// CHECK: function
console.log(typeof readline.cursorTo);
// CHECK: function
console.log(typeof readline.moveCursor);
// CHECK: function

// Create an interface with a programmatic input stream.
const input = new Readable({ read() {} });
const rl = readline.createInterface({ input, terminal: false });

let lineCount = 0;
rl.on('line', (line) => {
  lineCount++;
  console.log('GOT: ' + line);
});

rl.on('close', () => {
  console.log('LINES: ' + lineCount);
  console.log('PASS');
});

// Push lines and close.
input.push('hello\n');
input.push('world\n');
input.push(null);

// CHECK: GOT: hello
// CHECK: GOT: world
// CHECK: LINES: 2
// CHECK: PASS
