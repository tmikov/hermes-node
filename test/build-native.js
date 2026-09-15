// REQUIRES: linker-available, shermes-available
// RUN: rm -rf %t && mkdir -p %t/src/lib
//
// RUN: cp %s %t/src/app.js
// RUN: echo 'module.exports = { n: 41 };' > %t/src/lib/a.js
// RUN: echo '{"v": 1}' > %t/src/data.json
// RUN: echo 'function inner() { throw new Error("boom"); }' > %t/src/lib/thrower.js
// RUN: echo 'module.exports = inner;' >> %t/src/lib/thrower.js
// RUN: echo 'throw new Error("top level");' > %t/src/lib/boom.js
// RUN: echo 'module.exports = 1;' > %t/src/lib/c1.js
// RUN: echo 'module.exports = 2;' > %t/src/lib/c2.js
// RUN: echo 'module.exports = 3;' > %t/src/lib/c3.js
// RUN: echo 'module.exports = 4;' > %t/src/lib/c4.js
// RUN: echo 'module.exports = 5;' > %t/src/lib/c5.js
// RUN: echo 'module.exports = 6;' > %t/src/lib/c6.js
// RUN: echo 'module.exports = 7;' > %t/src/lib/c7.js
//
// RUN: %hermes-node build-native %t/src/app.js -o %t/app --kit=%kit_dir
// RUN: %t/app one two | %FileCheck %s
// RUN: %not %t/app --fail > /dev/null 2>&1
//
// The program must not need its source tree: that is the whole point of
// linking it in.
// RUN: mv %t/src %t/src-away
// RUN: %t/app one two | %FileCheck %s
//
// More than eight modules on purpose. SHUnit indices used to be capped at
// eight per process, and the twelve here would have aborted before the
// growable array landed.

const a = require('./lib/a.js');
const data = require('./data.json');
const thrower = require('./lib/thrower.js');

// Literal, one per line, NOT require('./lib/' + n + '.js'). A computed
// specifier is a scanner GAP (require_scanner.cpp records it and does not
// follow it), so a loop would package none of these and the test would
// silently stop covering more than eight units -- which is the thing it is
// here for.
require('./lib/c1.js');
require('./lib/c2.js');
require('./lib/c3.js');
require('./lib/c4.js');
require('./lib/c5.js');
require('./lib/c6.js');
require('./lib/c7.js');

// A module whose top level throws. Note what this does and does not
// cover: the CommonJS wrapper means the unit's global code only creates a
// closure, so the throw happens when the LOADER calls it, after
// hermes_init_sh_unit has returned. So this pins that a throwing module
// body propagates through require() -- the behaviour users see -- and NOT
// hermes_init_sh_unit's failure branch, which Task 1's hand-built
// throwing unit covers instead.
try {
  require('./lib/boom.js');
  console.log('TOPLEVEL no-throw');
} catch (e) {
  console.log('TOPLEVEL', e.message);
}
// CHECK: TOPLEVEL top level

console.log('SUM', a.n + data.v);
// CHECK: SUM 42

console.log('ARGV', process.argv.slice(2).join(','));
// CHECK: ARGV one,two

try {
  thrower();
} catch (e) {
  // -g2 puts identity:line:column into the plain stack string.
  console.log('STACK', e.stack.split('\n')[1].trim());
}
// CHECK: STACK at inner (lib/thrower.js:{{[0-9]+}}:{{[0-9]+}})

// The known phase-1 gap, asserted rather than skipped so that fixing it
// later fails here and is noticed. Structured CallSites carry no location
// for a native frame at any -g level, while the string above does.
Error.prepareStackTrace = (err, frames) =>
  frames.map((f) => f.getFileName() + ':' + f.getLineNumber()).join('|');
try {
  thrower();
} catch (e) {
  console.log('CALLSITE', e.stack.split('|')[0]);
}
// CHECK: CALLSITE null:null
Error.prepareStackTrace = undefined;

if (process.argv[2] === '--fail') process.exitCode = 1;
