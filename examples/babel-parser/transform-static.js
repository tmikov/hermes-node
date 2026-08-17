// transform.js with one change: the preset is required, not named.
//
// `presets: ['@babel/preset-env']` makes Babel resolve the preset by name at
// run time, which no static bundler can follow -- not hermes-node's
// --build-bundle and not rollup. The bundle builds without complaint, runs
// while node_modules is still there, and dies with "Cannot find module
// '@babel/preset-env'" once it is not.
//
// Requiring the preset makes it an ordinary static dependency, and the
// bundle becomes self-contained. Nothing else here differs from
// transform.js; the two files are meant to be diffed, so the duplication is
// the point and not an oversight.
//
// Usage:
//   hermes-node transform-static.js
//   hermes-node --build-bundle=app.hbb transform-static.js && hermes-node --bundle=app.hbb

'use strict';

var babel = require('@babel/core');
var presetEnv = require('@babel/preset-env');

var modernCode = [
  'const greet = (name) => `Hello, ${name}!`;',
  '',
  'class Animal {',
  '  constructor(name) {',
  '    this.name = name;',
  '  }',
  '  speak() {',
  '    return `${this.name} makes a noise.`;',
  '  }',
  '}',
  '',
  'const [first, ...rest] = [1, 2, 3, 4, 5];',
  'const merged = { a: 1, ...{ b: 2, c: 3 } };',
].join('\n');

var result = babel.transformSync(modernCode, {
  presets: [presetEnv],
  filename: 'input.js',
  cwd: __dirname,
});

console.log('=== Input ===');
console.log(modernCode);
console.log('\n=== Output ===');
console.log(result.code);
console.log('\nPASS');
