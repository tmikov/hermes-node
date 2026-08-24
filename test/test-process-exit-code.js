// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// RUN: %hermes-node %s | %FileCheck %s

// process.exitCode is the status a program sets when it wants to fail but
// still wants its output flushed and its pending work finished -- which is
// what every test runner and linter does. It used to be ignored entirely:
// the runtime kept a native exit code that nothing in JavaScript could
// reach, so `process.exitCode = 1` exited 0 and a failing run looked green.
//
// Each case below is checked against the status the same program produces
// under Node. The interesting ones are the 'exit' handler cases: Node keeps
// one variable, so an uncaught exception assigns process.exitCode = 1 and a
// handler can then overwrite it, which is why a handler's assignment wins
// even over a native failure.

var spawnSync = require('child_process').spawnSync;

var cases = [
  ['nothing set', 'console.log("x")', 0],
  ['plain assignment', 'process.exitCode = 3', 3],
  ['assignment from an async callback', 'setTimeout(function(){process.exitCode=5},10)', 5],
  ['bare exit() uses the property', 'process.exitCode=3;process.exit()', 3],
  ['an explicit exit(0) beats it', 'process.exitCode=3;process.exit(0)', 0],
  ['a throw beats a zero property', 'process.exitCode=0;throw new Error("boom")', 1],
  ['a throw beats a nonzero property', 'process.exitCode=5;throw new Error("boom")', 1],
  ['handler overrides the property',
   'process.on("exit",function(){process.exitCode=7});process.exitCode=3', 7],
  ['handler sets one from nothing',
   'process.on("exit",function(){process.exitCode=7})', 7],
  ['handler clears the property',
   'process.on("exit",function(){process.exitCode=0});process.exitCode=3', 0],
  ['handler rescues a native failure',
   'process.on("exit",function(){process.exitCode=0});throw new Error("boom")', 0],
  ['handler that touches nothing leaves a failure alone',
   'process.on("exit",function(){});throw new Error("boom")', 1],
  ['the last handler wins',
   'process.on("exit",function(){process.exitCode=1});' +
   'process.on("exit",function(){process.exitCode=2})', 2],
  ['the handler receives the settled code',
   'process.on("exit",function(c){if(c!==3)process.exitCode=99});process.exitCode=3', 3],
];

var failed = 0;
for (var i = 0; i < cases.length; i++) {
  var name = cases[i][0], code = cases[i][1], want = cases[i][2];
  var r = spawnSync(process.execPath, ['-e', code], {stdio: 'ignore'});
  if (r.status !== want) {
    console.log('FAIL: ' + name + ': expected ' + want + ', got ' + r.status);
    failed++;
  }
}

if (failed === 0) {
  console.log('PASS');
}
// CHECK: PASS
