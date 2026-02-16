// RUN: %hermes-node %s | %FileCheck %s
'use strict';

// Tests all 12 Readable stream operators from Node's internal/streams/operators.js.
//
// Stream-returning operators (map, filter, flatMap, drop, take) use async
// generators internally, exercising:
//   - async function* support (-Xasync-generators)
//   - for await...of inside async function* (flatMap, drop, take)
//   - ES6 block scoping (-Xes6-block-scoping) -- stream.js wires operators
//     via function declarations inside for-let loops that capture loop vars
//   - Promise.withResolvers (used by duplexify.js's fromAsyncGen)
//
// Promise-returning operators (reduce, toArray, some, every, find, forEach)
// consume streams via for-await and also depend on correct block scoping
// for proper dispatch from Readable.prototype.

const { Readable } = require('stream');

async function testMap() {
  const result = [];
  for await (const val of Readable.from([1, 2, 3, 4, 5]).map((x) => x * 2)) {
    result.push(val);
  }
  if (result.join() !== '2,4,6,8,10') throw new Error('map: ' + result);
  console.log('map ok');
  // CHECK: map ok
}

async function testFilter() {
  const result = [];
  for await (const val of Readable.from([1, 2, 3, 4, 5, 6]).filter((x) => x % 2 === 0)) {
    result.push(val);
  }
  if (result.join() !== '2,4,6') throw new Error('filter: ' + result);
  console.log('filter ok');
  // CHECK-NEXT: filter ok
}

async function testFlatMap() {
  const result = [];
  for await (const val of Readable.from([1, 2, 3]).flatMap((x) => [x, x * 10])) {
    result.push(val);
  }
  if (result.join() !== '1,10,2,20,3,30') throw new Error('flatMap: ' + result);
  console.log('flatMap ok');
  // CHECK-NEXT: flatMap ok
}

async function testDrop() {
  const result = [];
  for await (const val of Readable.from([1, 2, 3, 4, 5]).drop(2)) {
    result.push(val);
  }
  if (result.join() !== '3,4,5') throw new Error('drop: ' + result);
  console.log('drop ok');
  // CHECK-NEXT: drop ok
}

async function testTake() {
  const result = [];
  for await (const val of Readable.from([1, 2, 3, 4, 5]).take(3)) {
    result.push(val);
  }
  if (result.join() !== '1,2,3') throw new Error('take: ' + result);
  console.log('take ok');
  // CHECK-NEXT: take ok
}

async function testReduce() {
  const sum = await Readable.from([1, 2, 3, 4, 5]).reduce((acc, val) => acc + val, 0);
  if (sum !== 15) throw new Error('reduce: ' + sum);
  console.log('reduce ok');
  // CHECK-NEXT: reduce ok
}

async function testToArray() {
  const arr = await Readable.from([10, 20, 30]).toArray();
  if (arr.join() !== '10,20,30') throw new Error('toArray: ' + arr);
  console.log('toArray ok');
  // CHECK-NEXT: toArray ok
}

async function testSome() {
  const has2 = await Readable.from([1, 2, 3]).some((x) => x === 2);
  if (has2 !== true) throw new Error('some(2): ' + has2);
  const has9 = await Readable.from([1, 2, 3]).some((x) => x === 9);
  if (has9 !== false) throw new Error('some(9): ' + has9);
  console.log('some ok');
  // CHECK-NEXT: some ok
}

async function testEvery() {
  const allEven = await Readable.from([2, 4, 6]).every((x) => x % 2 === 0);
  if (allEven !== true) throw new Error('every(even): ' + allEven);
  const allEven2 = await Readable.from([2, 3, 6]).every((x) => x % 2 === 0);
  if (allEven2 !== false) throw new Error('every(mixed): ' + allEven2);
  console.log('every ok');
  // CHECK-NEXT: every ok
}

async function testFind() {
  const found = await Readable.from([1, 2, 3, 4, 5]).find((x) => x > 3);
  if (found !== 4) throw new Error('find: ' + found);
  console.log('find ok');
  // CHECK-NEXT: find ok
}

async function testForEach() {
  const result = [];
  await Readable.from([10, 20, 30]).forEach((x) => result.push(x));
  if (result.join() !== '10,20,30') throw new Error('forEach: ' + result);
  console.log('forEach ok');
  // CHECK-NEXT: forEach ok
}

async function testChained() {
  const result = await Readable.from([1, 2, 3, 4, 5, 6, 7, 8, 9, 10])
    .filter((x) => x % 2 === 0)
    .map((x) => x * 3)
    .toArray();
  if (result.join() !== '6,12,18,24,30') throw new Error('chained: ' + result);
  console.log('chained ok');
  // CHECK-NEXT: chained ok
}

async function main() {
  await testMap();
  await testFilter();
  await testFlatMap();
  await testDrop();
  await testTake();
  await testReduce();
  await testToArray();
  await testSome();
  await testEvery();
  await testFind();
  await testForEach();
  await testChained();
  console.log('PASS');
  // CHECK-NEXT: PASS
}

main();
