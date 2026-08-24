// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// RUN: %hermes-node %s | %FileCheck %s

// An exception escaping an I/O callback used to be taken and dropped on the
// floor -- mostly without even being printed. Two consequences, both wrong
// and both invisible: 'uncaughtException' listeners never ran, and the
// process exited 0, so a test runner whose assertion failed inside an fs
// callback looked green.
//
// Where the callback owned a live libuv handle it was worse than exiting 0.
// The handle stayed refed with nobody to service it, so the loop never
// drained and the program HUNG instead of failing -- net's connection,
// data, write and close callbacks, and dgram's message callback, all did
// this. The timers fix that preceded this one covered anything routed
// through a tick; these are the callbacks the native side invokes directly.
//
// Each case is checked against the status the same program produces under
// node v24.13.1. The handled half matters as much as the unhandled half: a
// listener that takes the error must leave the handle able to carry on, and
// a fix that exits correctly but stalls the socket afterwards would pass a
// test that only looked at exit codes.

var spawnSync = require('child_process').spawnSync;

function run(src) {
  var r = spawnSync(process.execPath, ['-e', src], {
    stdio: ['ignore', 'pipe', 'ignore'],
    timeout: 10000,
  });
  return {
    status: r.status,
    out: String(r.stdout || '').trim(),
    timedOut: r.signal === 'SIGTERM' || r.status === null,
  };
}

var LISTEN = 'process.on("uncaughtException",function(e){console.log("caught:"+e.message)});';
var failed = 0;

// --- unhandled: every one of these must exit 1, and none may hang --------
var unhandled = [
  ['fs.readFile', 'require("fs").readFile(__filename,function(){throw new Error("x")})'],
  ['fs.writeFile', 'require("fs").writeFile(require("os").tmpdir()+"/_hn_t","a",function(){throw new Error("x")})'],
  ['net connection', 'var n=require("net");var s=n.createServer(function(){throw new Error("x")});' +
   's.listen(0,function(){n.connect(s.address().port)})'],
  ['net data', 'var n=require("net");var s=n.createServer(function(c){c.end("hi")});' +
   's.listen(0,function(){n.connect(s.address().port).on("data",function(){throw new Error("x")})})'],
  ['net write cb', 'var n=require("net");var s=n.createServer(function(c){c.resume()});' +
   's.listen(0,function(){var c=n.connect(s.address().port,function(){c.write("x",function(){throw new Error("x")})})})'],
  ['net close', 'var n=require("net");var s=n.createServer(function(c){c.resume()});' +
   's.listen(0,function(){var c=n.connect(s.address().port,function(){c.end()});c.on("close",function(){throw new Error("x")})})'],
  ['dgram message', 'var d=require("dgram").createSocket("udp4");' +
   'd.on("message",function(){throw new Error("x")});' +
   'd.bind(0,function(){d.send("x",d.address().port,"127.0.0.1")})'],
  ['child exit', 'require("child_process").spawn("true").on("exit",function(){throw new Error("x")})'],
];

for (var i = 0; i < unhandled.length; i++) {
  var name = unhandled[i][0];
  var r = run(unhandled[i][1]);
  if (r.timedOut) {
    console.log('FAIL: ' + name + ' unhandled: hung (the handle was left refed)');
    failed++;
  } else if (r.status !== 1) {
    console.log('FAIL: ' + name + ' unhandled: expected exit 1, got ' + r.status);
    failed++;
  }
}

// --- handled: the listener runs, and the I/O carries on ------------------
var handled = [
  ['fs', LISTEN + 'var fs=require("fs");fs.readFile(__filename,function(){throw new Error("a")});' +
   'setTimeout(function(){fs.readFile(__filename,function(){console.log("second")})},20)', 'second'],
  ['net data', LISTEN + 'var n=require("net");var s=n.createServer(function(c){c.end("hi")});' +
   's.listen(0,function(){var c=n.connect(s.address().port);' +
   'c.on("data",function(){throw new Error("a")});' +
   'c.on("end",function(){console.log("second");s.close()})})', 'second'],
  ['net connection', LISTEN + 'var n=require("net");var k=0;' +
   'var s=n.createServer(function(c){k++;c.end();if(k===1)throw new Error("a");' +
   'console.log("second");s.close()});' +
   's.listen(0,function(){var p=s.address().port;' +
   'n.connect(p).on("close",function(){n.connect(p).resume()})})', 'second'],
  ['dgram message', LISTEN + 'var d=require("dgram").createSocket("udp4");var k=0;' +
   'd.on("message",function(){k++;if(k===1)throw new Error("a");' +
   'console.log("second");d.close()});' +
   'd.bind(0,function(){var p=d.address().port;' +
   'd.send("1",p,"127.0.0.1",function(){d.send("2",p,"127.0.0.1")})})', 'second'],
];

for (var j = 0; j < handled.length; j++) {
  var hname = handled[j][0];
  var hr = run(handled[j][1]);
  if (hr.timedOut) {
    console.log('FAIL: ' + hname + ' handled: hung');
    failed++;
  } else if (hr.status !== 0) {
    console.log('FAIL: ' + hname + ' handled: expected exit 0, got ' + hr.status);
    failed++;
  } else if (hr.out.indexOf('caught:a') === -1) {
    console.log('FAIL: ' + hname + ' handled: listener did not run: ' + hr.out);
    failed++;
  } else if (hr.out.indexOf(handled[j][2]) === -1) {
    console.log('FAIL: ' + hname + ' handled: I/O did not resume: ' + hr.out);
    failed++;
  }
}

if (failed === 0) {
  console.log('PASS');
}
// CHECK: PASS
