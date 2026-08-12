// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.
//
// Mirrors benchmarks/MiniReact/no-objects/build.config.js from the Hermes
// tree, with plugins resolved from this example's node_modules.

const path = require('path');

const HERE = __dirname;

function plugin(name) {
  return require.resolve(name, {paths: [HERE]});
}

function createConfig(benchmarkName) {
  return {
    root: path.join(HERE, 'fixture', 'src'),
    outDir: path.join(HERE, 'out'),
    entrypoints: [`./app/${benchmarkName}/index.js`],
    simpleJsxTransform: true,
    out: {
      [`${benchmarkName}.js`]: null,
      [`${benchmarkName}-stripped.js`]: {
        babelConfig: {
          plugins: [plugin('@babel/plugin-transform-flow-strip-types')],
        },
      },
      [`${benchmarkName}-lowered.js`]: {
        babelConfig: {
          plugins: [
            [plugin('@babel/plugin-transform-class-properties'), {enableBabelRuntime: false}],
            plugin('@babel/plugin-transform-flow-strip-types'),
            [plugin('@babel/plugin-transform-classes'), {enableBabelRuntime: false}],
          ],
          assumptions: {
            constantSuper: true,
            noClassCalls: true,
            setClassMethods: true,
            setPublicClassFields: true,
            superIsCallableConstructor: true,
          },
        },
      },
    },
  };
}

module.exports = {builds: [createConfig('simple'), createConfig('music')]};
