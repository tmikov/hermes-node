// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.
//
// Preloaded with `hermes-node -r`. Installs a require hook that compiles
// the bundler's Flow + ESM sources to CommonJS as they load, parsing them
// with hermes-parser by way of babel-plugin-syntax-hermes-parser.
//
// configFile and root are explicit: Babel otherwise looks for configuration
// relative to each file it compiles, and `only` keeps the hook off anything
// outside this example.

const path = require('path');

require('@babel/register')({
  ...require('./babel.config'),
  configFile: false,
  babelrc: false,
  root: __dirname,
  only: [path.join(__dirname, 'bundler'), path.join(__dirname, 'build.config.js')],
  extensions: ['.js'],
});
