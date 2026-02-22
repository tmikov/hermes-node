// Parse JavaScript source code using @babel/parser and print the AST.
//
// Usage:
//   hermes-node parse.js

'use strict';

var parser = require('@babel/parser');

var code = [
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
  'async function fetchData(url) {',
  '  const response = await fetch(url);',
  '  return response.json();',
  '}',
].join('\n');

var ast = parser.parse(code, {
  sourceType: 'module',
  plugins: ['classProperties'],
});

console.log('Parsed ' + ast.program.body.length + ' top-level statements:');
ast.program.body.forEach(function(node, i) {
  console.log('  ' + (i + 1) + '. ' + node.type + ' (' + node.loc.start.line + ':' + node.loc.start.column + ')');
});

// Dig into the class to show AST detail
var classNode = ast.program.body[1];
console.log('\nClass "' + classNode.id.name + '" has ' + classNode.body.body.length + ' members:');
classNode.body.body.forEach(function(member) {
  console.log('  - ' + member.type + ': ' + (member.key.name || member.kind));
});

console.log('\nPASS');
