// Copyright (c) Tzvetan Mikov.
// A TypeScript module used by test-typescript-require.js.

interface Greeting {
  message: string;
}

function greet(name: string): Greeting {
  return { message: 'Hello, ' + name + '!' };
}

const VERSION: number = 1;

module.exports = { greet, VERSION };
