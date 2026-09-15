// REQUIRES: linker-available, shermes-available
//
// The forcing function for the compile-flag parity table in
// docs/superpowers/specs/2026-09-13-native-compilation-design.md. Every row
// of that table is SILENT when it is wrong: shermes defaults ES6 block
// scoping and async generators off, while the bytecode compiler enables
// both, and block scoping off changes what a let-in-loop closure captures
// with no error anywhere. So the check is to run one program two ways and
// diff, not to inspect anything.
//
// RUN: rm -rf %t && mkdir -p %t/src
// RUN: cp %s %t/src/app.js
// An interface plus module.exports, NOT `export const`: every module is
// compiled inside the CommonJS wrapper, and an export declaration is not
// top-level there. test/bundle-build.js:117 uses this same form. The
// interface is load-bearing, not decoration: a bare `identifier: Type`
// annotation on a `const` parses under plain Hermes with no TypeScript
// flag at all (confirmed directly against shermes), so a fixture built
// from that shape alone would build and run identically with
// -transform-ts removed and prove nothing. `interface Typed { ... }`
// actually exercises this row: it requires TypeScript parsing, and it was
// this exact construct that surfaced a real native_compile.cpp bug (see
// lib/build-native/native_compile.cpp) where -transform-ts alone left
// Flow's parser handling the declaration instead of TypeScript's, so
// -parse-ts is now pushed alongside it.
// RUN: echo 'interface Typed { n: number }' > %t/src/typed.ts
// RUN: echo 'const typed: Typed = { n: 7 };' >> %t/src/typed.ts
// RUN: echo 'module.exports = typed;' >> %t/src/typed.ts
//
// RUN: %hermes-node %t/src/app.js > %t/plain.txt 2>&1
// RUN: %hermes-node build-native %t/src/app.js -o %t/app --kit=%kit_dir
// RUN: %t/app > %t/native.txt 2>&1
// RUN: diff -u %t/plain.txt %t/native.txt
// RUN: %FileCheck %s < %t/native.txt

// --- block scoping: the loop-closure case, measured at 3,3,3 without the
// --- flag and 0,1,2 with it.
const fns = [];
for (let i = 0; i < 3; i++) fns.push(() => i);
console.log('LET', fns.map((f) => f()).join(','));
// CHECK: LET 0,1,2

// const in a loop body, the other half of per-iteration binding
const consts = [];
for (const v of ['a', 'b']) consts.push(() => v);
console.log('CONST', consts.map((f) => f()).join(','));
// CHECK: CONST a,b

// --- async generators: a PARSE error without the flag, so its absence
// --- fails the build rather than the output. Here for completeness of the
// --- table, and because the scanner used to reject this file outright.
async function* ag() {
  yield 1;
  yield 2;
}

// --- plain generators: on in both compilers, no flag. A row in the table
// --- so that a future change turning them off is caught.
function* g() {
  yield 3;
}
console.log('GEN', [...g()].join(','));
// CHECK: GEN 3

// --- TypeScript, per .ts extension
const typed = require('./typed.ts');
console.log('TS', typed.n);
// CHECK: TS 7

(async () => {
  const seen = [];
  for await (const v of ag()) seen.push(v);
  console.log('ASYNCGEN', seen.join(','));
  // CHECK: ASYNCGEN 1,2
})();
