/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * @format
 */

// Copied from the Hermes flow-bundler, header included and verbatim (it is
// shorter than the usual MIT header upstream uses; the file is MIT-licensed
// along with the rest of that repository). Unchanged except for this comment:
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
