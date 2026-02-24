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
// CHECK: async deflate/inflate: PASS

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
// CHECK: async gzip/gunzip: PASS

// CHECK: PASS
