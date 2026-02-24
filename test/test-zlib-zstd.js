// RUN: %hermes-node %s | %FileCheck %s

'use strict';

const zlib = require('zlib');

// Test sync zstd compress/decompress
{
  const input = Buffer.from('Zstandard compression test data. Hello World!');
  const compressed = zlib.zstdCompressSync(input);
  const decompressed = zlib.zstdDecompressSync(compressed);
  console.log('zstd sync:', decompressed.toString() === input.toString() ? 'PASS' : 'FAIL');
}
// CHECK: zstd sync: PASS

// Test zstd dictionary with DataView.
{
  const dictBytes = Buffer.from('zstd-dict-bytes');
  const dictView = new DataView(
    dictBytes.buffer,
    dictBytes.byteOffset,
    dictBytes.byteLength,
  );
  const input = Buffer.from('zstd-dict-bytes + payload + zstd-dict-bytes');

  const outBufferDict = zlib.zstdCompressSync(input, { dictionary: dictBytes });
  const outViewDict = zlib.zstdCompressSync(input, { dictionary: dictView });
  console.log('zstd dictionary dataview:', outBufferDict.equals(outViewDict) ? 'PASS' : 'FAIL');
}
// CHECK: zstd dictionary dataview: PASS

// Test async zstd compress/decompress
{
  const input = Buffer.from('Async zstd test data.');
  zlib.zstdCompress(input, (err, compressed) => {
    if (err) { console.log('zstd async: FAIL', err.message); return; }
    zlib.zstdDecompress(compressed, (err2, decompressed) => {
      if (err2) { console.log('zstd async: FAIL', err2.message); return; }
      console.log('zstd async:', decompressed.toString() === input.toString() ? 'PASS' : 'FAIL');
    });
  });
}
// CHECK: zstd async: PASS
