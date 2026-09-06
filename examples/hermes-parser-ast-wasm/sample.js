// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// A small file to parse. It exists so the AST assertion in run.sh can be
// specific to this input -- a class with a private field, and an optional
// chain -- rather than "some JSON appeared".

class Counter {
  #count = 0;

  increment() {
    this.#count += 1;
    return this.#count;
  }
}

function describe(counter) {
  return counter?.increment?.() ?? 0;
}

const c = new Counter();
describe(c);
