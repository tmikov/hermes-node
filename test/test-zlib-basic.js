// RUN: %hermes-node %s | %FileCheck %s

'use strict';

const zlib = require('zlib');

// Test sync deflate/inflate
{
  const input = Buffer.from('Hello, World! This is a test of zlib compression.');
  const deflated = zlib.deflateSync(input);
  const inflated = zlib.inflateSync(deflated);
  console.log('deflate/inflate:', inflated.toString() === input.toString() ? 'PASS' : 'FAIL');
}
// CHECK: deflate/inflate: PASS

// Test sync gzip/gunzip
{
  const input = Buffer.from('Gzip test data with some content.');
  const gzipped = zlib.gzipSync(input);
  const gunzipped = zlib.gunzipSync(gzipped);
  console.log('gzip/gunzip:', gunzipped.toString() === input.toString() ? 'PASS' : 'FAIL');
}
// CHECK: gzip/gunzip: PASS

// Test sync deflateRaw/inflateRaw
{
  const input = Buffer.from('Raw deflate test data.');
  const deflated = zlib.deflateRawSync(input);
  const inflated = zlib.inflateRawSync(deflated);
  console.log('deflateRaw/inflateRaw:', inflated.toString() === input.toString() ? 'PASS' : 'FAIL');
}
// CHECK: deflateRaw/inflateRaw: PASS

// Test crc32
{
  const data = Buffer.from('hello');
  const crc = zlib.crc32(data);
  // Known CRC32 of 'hello' is 907060870 (0x3610A686)
  console.log('crc32:', typeof crc === 'number' && crc !== 0 ? 'PASS' : 'FAIL');
}
// CHECK: crc32: PASS

// Test crc32 with string
{
  const crc = zlib.crc32('hello');
  console.log('crc32 string:', typeof crc === 'number' && crc !== 0 ? 'PASS' : 'FAIL');
}
// CHECK: crc32 string: PASS

// Test crc32 with DataView
{
  const input = Buffer.from('dataview-crc32');
  const view = new DataView(input.buffer, input.byteOffset, input.byteLength);
  const crcBuf = zlib.crc32(input);
  const crcView = zlib.crc32(view);
  console.log('crc32 dataview:', crcBuf === crcView ? 'PASS' : 'FAIL');
}
// CHECK: crc32 dataview: PASS

// Test dictionary handling with non-Buffer ArrayBufferView.
{
  const dictBytes = Buffer.from('shared-dictionary-seed-data');
  const dictView = new DataView(
    dictBytes.buffer,
    dictBytes.byteOffset,
    dictBytes.byteLength,
  );
  const input = Buffer.from('shared-dictionary-seed-data::payload::shared-dictionary-seed-data');

  const outBufferDict = zlib.deflateRawSync(input, { dictionary: dictBytes });
  const outViewDict = zlib.deflateRawSync(input, { dictionary: dictView });

  console.log('dictionary dataview:', outBufferDict.equals(outViewDict) ? 'PASS' : 'FAIL');
}
// CHECK: dictionary dataview: PASS

// Test empty input
{
  const input = Buffer.alloc(0);
  const deflated = zlib.deflateSync(input);
  const inflated = zlib.inflateSync(deflated);
  console.log('empty:', inflated.length === 0 ? 'PASS' : 'FAIL');
}
// CHECK: empty: PASS

// Test constants are available
{
  const constants = zlib.constants;
  const hasBasicConsts =
    typeof constants.Z_NO_FLUSH === 'number' &&
    typeof constants.Z_FINISH === 'number' &&
    typeof constants.Z_DEFAULT_COMPRESSION === 'number' &&
    typeof constants.DEFLATE === 'number' &&
    typeof constants.BROTLI_OPERATION_PROCESS === 'number' &&
    typeof constants.ZSTD_e_continue === 'number';
  console.log('constants:', hasBasicConsts ? 'PASS' : 'FAIL');
}
// CHECK: constants: PASS

// Test ZLIB_VERSION is exported
{
  console.log('version:', typeof zlib.constants.ZLIB_VERNUM === 'number' ? 'PASS' : 'FAIL');
}
// CHECK: version: PASS

console.log('PASS');
// CHECK: PASS
