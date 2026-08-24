// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// RUN: %hermes-node %s | %FileCheck %s

// Two things a program is entitled to when it calls process.exit(), and
// neither held.
//
// The 'exit' event was never emitted at all: processExit() called _exit()
// straight away, so every listener was skipped. That is where a terminal UI
// puts the code that restores the screen -- blessed registers exactly such a
// handler -- so quitting one left the alternate screen buffer on and the
// user looking at whatever it had drawn.
//
// And output queued before exiting was thrown away. stdout on a TTY or a
// pipe is a libuv stream whose writes are queued rather than synchronous, and
// _exit() does not flush; eight console.log calls followed by process.exit()
// printed one line. The same loss hit anything an 'exit' handler wrote, on
// both exit paths, which is why restoring a screen from one only got a
// fraction of the escape sequence out.
//
// Each case below is checked against the same program under Node.

var spawnSync = require('child_process').spawnSync;

function run(code) {
  var r = spawnSync(process.execPath, ['-e', code], {encoding: 'utf8'});
  return {out: (r.stdout || '').trim(), status: r.status};
}

var cases = [
  // The event fires, once, with the code, for every listener.
  ['fires with the code',
   'process.on("exit",c=>console.log("H"+c));process.exit(3)', 'H3', 3],
  ['every listener runs',
   'process.on("exit",()=>console.log("A"));' +
   'process.on("exit",()=>console.log("B"));process.exit(0)', 'A\nB', 0],
  ['a handler can overrule the code',
   'process.on("exit",()=>{process.exitCode=7});process.exit(3)', '', 7],
  ['a nested exit does not re-run handlers',
   'let n=0;process.on("exit",()=>{n++;console.log("H"+n);' +
   'if(n<3)process.exit(9)});process.exit(1)', 'H1', 9],
  // Nothing queued is lost, on either exit path.
  ['queued output survives exit()',
   'for(let i=0;i<8;i++)console.log("L"+i);process.exit(0)',
   'L0\nL1\nL2\nL3\nL4\nL5\nL6\nL7', 0],
  ['a handler can print, after exit()',
   'process.on("exit",()=>{for(let i=0;i<5;i++)console.log("H"+i)});' +
   'process.exit(0)', 'H0\nH1\nH2\nH3\nH4', 0],
  ['a handler can print, on a natural exit',
   'process.on("exit",()=>{for(let i=0;i<5;i++)console.log("H"+i)});' +
   'console.log("main")', 'main\nH0\nH1\nH2\nH3\nH4', 0],
];

var failed = 0;
for (var i = 0; i < cases.length; i++) {
  var name = cases[i][0], code = cases[i][1];
  var wantOut = cases[i][2], wantStatus = cases[i][3];
  var got = run(code);
  if (got.out !== wantOut || got.status !== wantStatus) {
    console.log('FAIL: ' + name +
                '\n  expected ' + JSON.stringify(wantOut) + ' status ' + wantStatus +
                '\n  got      ' + JSON.stringify(got.out) + ' status ' + got.status);
    failed++;
  }
}

if (failed === 0) {
  console.log('PASS');
}
// CHECK: PASS
