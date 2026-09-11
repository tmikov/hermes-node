// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.
//
// Shim for yoga-layout's top-level await.
//
// yoga-layout@3.2.1's "." entry (dist/src/index.js) is:
//
//   const Yoga = wrapAssembly(await loadYoga());
//   export default Yoga;
//
// which CommonJS cannot express, and esbuild refuses to emit for a "cjs"
// output format. The package also exposes "./load" (dist/src/load.js),
// the same object behind an async function instead of a top-level await:
//
//   export async function loadYoga() { return wrapAssembly(await loadYogaImpl()); }
//
// build.mjs aliases the bare 'yoga-layout' specifier to this file, so every
// `import Yoga from 'yoga-layout'` in Ink's own sources resolves here
// instead. This works only because Ink touches Yoga exclusively inside
// function bodies (event handlers, layout callbacks) and never at module
// scope -- verified for ink@6.4.0 by grepping its build output for `Yoga.`
// outside a function. That is what makes a *lazy* proxy viable: the module
// graph can finish loading before the real Yoga object exists, as long as
// nothing calls into it before initYoga() below has resolved.
//
// This file imports the underlying dist/src/load.js by relative path
// rather than the 'yoga-layout/load' specifier. esbuild's --alias for a
// package name also rewrites that package's subpaths, so importing
// 'yoga-layout/load' here would resolve back to this very file and recurse.
import {loadYoga} from './node_modules/yoga-layout/dist/src/load.js';

let real = null;
const pending = loadYoga().then((yoga) => {
  real = yoga;
  return yoga;
});

// The app must await this before rendering (see app.mjs). Once it
// resolves, every property read on the default export below is forwarded
// to the real Yoga module.
export function initYoga() {
  return pending;
}

const proxy = new Proxy(
  {},
  {
    get(_target, prop) {
      if (real === null) {
        throw new Error(
          "yoga-layout accessed before initYoga() resolved -- await it before rendering"
        );
      }
      return real[prop];
    },
  }
);

export default proxy;
