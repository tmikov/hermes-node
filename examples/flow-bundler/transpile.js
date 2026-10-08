// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.
//
// Transpiles bundler/ to CommonJS ahead of time, for build-bundle.sh.
//
// Usage: hermes-node transpile.js <out-dir>
//
// This does what babel-register.js does on the way in, but up front and to
// disk: the same presets and plugins, with configFile, babelrc and root
// pinned the same way. A bundle cannot use the require hook -- see "Bundling
// it" in README.md -- so the artifacts are built from this output instead.
//
// Run it with FLOW_BUNDLER_PARSER=wasm to parse with the parser the
// artifacts carry; the output is byte-identical either way.

'use strict';

// Must come first, for the same reason as in babel-register.js: everything
// below may pull the parser in.
require('./parser-select');

const fs = require('fs');
const path = require('path');
const babel = require('@babel/core');
const config = require('./babel.config');

const outDir = process.argv[2];
if (!outDir) {
  console.error('usage: hermes-node transpile.js <out-dir>');
  process.exit(2);
}

let count = 0;

function walk(srcDir, destDir) {
  for (const entry of fs.readdirSync(srcDir, {withFileTypes: true})) {
    const src = path.join(srcDir, entry.name);
    const dest = path.join(destDir, entry.name);
    if (entry.isDirectory()) {
      walk(src, dest);
    } else if (entry.name.endsWith('.js')) {
      const {code} = babel.transformFileSync(src, {
        presets: config.presets,
        plugins: config.plugins,
        configFile: false,
        babelrc: false,
        root: __dirname,
      });
      fs.mkdirSync(destDir, {recursive: true});
      fs.writeFileSync(dest, code);
      count++;
    }
  }
}

walk(path.join(__dirname, 'bundler'), outDir);
console.log(`transpiled ${count} files to ${outDir}`);
