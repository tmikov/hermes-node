// RUN: %hermes-node %s | %FileCheck %s

'use strict';

const zlib = require('zlib');

let pending = 0;

function done() {
  pending--;
  if (pending === 0) {
    console.log('PASS');
  }
}

// Async deflate/inflate
pending++;
{
  const input = Buffer.from('Hello async zlib world!');
  zlib.deflate(input, (err, deflated) => {
    if (err) { console.log('async deflate: FAIL', err.message); done(); return; }
    zlib.inflate(deflated, (err2, inflated) => {
      if (err2) { console.log('async inflate: FAIL', err2.message); done(); return; }
      console.log('async deflate/inflate:', inflated.toString() === input.toString() ? 'PASS' : 'FAIL');
      done();
    });
  });
}
// The two chains below are independent and run concurrently, so which
// finishes first is up to the thread pool rather than the program. Ordered
// CHECK lines encoded an order nothing guarantees: this passed on Linux and
// failed on the macOS release runner, where the second chain reported first.
// CHECK-DAG matches the pair in either order.
//
// This does not weaken the test. A chain that genuinely fails prints
// "async gzip: FAIL ..." instead of the line below, which no CHECK-DAG
// matches, so a real zlib defect still fails here -- only the ordering
// assumption is gone.
// CHECK-DAG: async deflate/inflate: PASS

// Async gzip/gunzip
pending++;
{
  const input = Buffer.from('Async gzip test data');
  zlib.gzip(input, (err, gzipped) => {
    if (err) { console.log('async gzip: FAIL', err.message); done(); return; }
    zlib.gunzip(gzipped, (err2, gunzipped) => {
      if (err2) { console.log('async gunzip: FAIL', err2.message); done(); return; }
      console.log('async gzip/gunzip:', gunzipped.toString() === input.toString() ? 'PASS' : 'FAIL');
      done();
    });
  });
}
// CHECK-DAG: async gzip/gunzip: PASS

// CHECK: PASS
