/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 *
 * @format
 */

'use strict';

const fs = require('fs');
const path = require('path');

// Path edited from upstream: there this script sits in
// tools/hermes-parser/js/scripts and the package is a sibling named
// hermes-parser; here scripts/ and package/ are siblings.
const OUTPUT_FILE = path.resolve(
  __dirname,
  '../package/dist/HermesParserWASM.js',
);

const HEADER = `/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

'use strict';
`;

// Add header and sign file before writing back to disk
const wasmParserContents = fs.readFileSync(process.argv[2]).toString();
const fileContents = HEADER + wasmParserContents;

fs.writeFileSync(OUTPUT_FILE, fileContents);
