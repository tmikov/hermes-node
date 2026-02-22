// Transform modern JavaScript to ES5 using @babel/core + @babel/preset-env.
//
// Usage:
//   hermes-node transform.js

'use strict';

var babel = require('@babel/core');

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
  presets: ['@babel/preset-env'],
  filename: 'input.js',
  cwd: __dirname,
});

console.log('=== Input ===');
console.log(modernCode);
console.log('\n=== Output ===');
console.log(result.code);
console.log('\nPASS');
