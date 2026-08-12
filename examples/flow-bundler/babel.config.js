/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 *
 * @format
 */

// Copied from the Hermes flow-bundler. Unchanged except for this comment:
// __dirname is now this example directory, so NODE_MODULES resolves to the
// node_modules created by `npm install` here.
const path = require('path');
const NODE_MODULES = path.resolve(__dirname, 'node_modules');

module.exports = {
  presets: [
    [
      path.join(NODE_MODULES, '@babel/preset-env'),
      {targets: {node: 'current'}},
    ],
    path.join(NODE_MODULES, '@babel/preset-flow'),
  ],
  plugins: [path.join(NODE_MODULES, 'babel-plugin-syntax-hermes-parser')],
  ignore: [/\/node_modules\//],
};
