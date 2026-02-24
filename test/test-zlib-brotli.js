// RUN: %hermes-node %s | %FileCheck %s

'use strict';

const zlib = require('zlib');

// Test sync brotli compress/decompress
{
  const input = Buffer.from('Brotli compression test data. Hello World!');
  const compressed = zlib.brotliCompressSync(input);
  const decompressed = zlib.brotliDecompressSync(compressed);
  console.log('brotli sync:', decompressed.toString() === input.toString() ? 'PASS' : 'FAIL');
}
// CHECK: brotli sync: PASS

// Test async brotli compress/decompress
{
  const input = Buffer.from('Async brotli test data.');
  zlib.brotliCompress(input, (err, compressed) => {
    if (err) { console.log('brotli async: FAIL', err.message); return; }
    zlib.brotliDecompress(compressed, (err2, decompressed) => {
      if (err2) { console.log('brotli async: FAIL', err2.message); return; }
      console.log('brotli async:', decompressed.toString() === input.toString() ? 'PASS' : 'FAIL');
    });
  });
}
// CHECK: brotli async: PASS
