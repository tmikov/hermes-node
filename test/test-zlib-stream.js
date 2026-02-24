// RUN: %hermes-node %s | %FileCheck %s

'use strict';

const zlib = require('zlib');
const { Writable } = require('stream');

// Test pipe-based streaming compression
{
  const input = Buffer.from('Streaming compression test. This is some data that will be piped through a gzip stream.');

  const gzip = zlib.createGzip();
  const gunzip = zlib.createGunzip();

  const chunks = [];

  const sink = new Writable({
    write(chunk, encoding, cb) {
      chunks.push(chunk);
      cb();
    }
  });

  sink.on('finish', () => {
    const result = Buffer.concat(chunks);
    console.log('stream pipe:', result.toString() === input.toString() ? 'PASS' : 'FAIL');
  });

  gzip.pipe(gunzip).pipe(sink);
  gzip.end(input);
}
// CHECK: stream pipe: PASS
