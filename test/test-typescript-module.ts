// Copyright (c) Tzvetan Mikov.
// A TypeScript module used by test-typescript-require.js.

function greet(name: string): string {
  return 'Hello, ' + name + '!';
}

const VERSION: number = 1;

module.exports = { greet, VERSION };
