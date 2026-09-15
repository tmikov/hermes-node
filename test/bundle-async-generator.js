// RUN: rm -rf %t && mkdir -p %t
// RUN: cp %s %t/app.js
// RUN: echo 'async function* g() { yield 1; }' > %t/gen.js
// RUN: echo 'module.exports = g;' >> %t/gen.js
// RUN: %hermes-node %t/app.js | %FileCheck --check-prefix=PLAIN %s
// RUN: %hermes-node --build-bundle=%t/app.hbb %t/app.js 2>&1 | %FileCheck --check-prefix=BUILD %s
// RUN: %hermes-node --bundle=%t/app.hbb | %FileCheck --check-prefix=BUNDLED %s

// An async generator is a parse error unless the compiler is told to allow
// them. The bundle producer's scanner and its compiler must agree about
// that, or the scanner stubs a module the compiler would have compiled --
// which is what it used to do. See dz 01a09e14-0a0e.

const g = require('./gen.js');
(async () => {
  for await (const v of g()) console.log('PASS', v);
})();

// A block-scoped shadow of require. sema::resolveAST gives every `{ }` its
// own scope regardless of the ES6-block-scoping flag (that flag only
// affects IRGen loop-capture codegen, which the scanner never reaches), so
// this passes with the flag either way. It is here to pin the shadowing
// behavior itself: the scanner must not attribute this call to the
// module's require parameter and must not warn about an unresolvable
// specifier.
{
  const require = (x) => ({ shadowed: x });
  require('./not-a-real-file.js');
}

// PLAIN: PASS 1
// BUILD-NOT: cannot parse
// BUILD-NOT: not-a-real-file
// BUNDLED: PASS 1
