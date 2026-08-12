/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 *
 * @flow strict-local
 * @format
 */

'use strict';

import type {HermesNode} from './HermesAST';
import type {ParserOptions} from './ParserOptions';

import HermesParserDeserializer from './HermesParserDeserializer';
import EXPECTED_KIND_HASH from './HermesParserKindHash';

const loadAddon = require('./HermesParserAddon');

const CONTAINER_MAGIC = 0x484d5052;
const CONTAINER_VERSION = 1;

let addon = null;

function getAddon() {
  if (addon == null) {
    addon = loadAddon();
  }
  return addon;
}

export function parse(source: string, options: ParserOptions): HermesNode {
  const result = getAddon().parse(source, {
    detectFlow: options.flow === 'detect',
    enableExperimentalComponentSyntax:
      options.enableExperimentalComponentSyntax === true,
    enableExperimentalFlowMatchSyntax:
      options.enableExperimentalFlowMatchSyntax === true,
    enableExperimentalFlowRecordSyntax:
      options.enableExperimentalFlowRecordSyntax === true,
    tokens: options.tokens === true,
    allowReturnOutsideFunction: options.allowReturnOutsideFunction === true,
  });

  if (result.error != null) {
    // Node-API cannot construct a SyntaxError, so the addon returns a
    // descriptor and we build the error here. This keeps the thrown value
    // identical in shape to what the WASM parser throws.
    const syntaxError = new SyntaxError(result.error);
    // $FlowExpectedError[prop-missing]
    syntaxError.loc = {line: result.line, column: result.column};
    throw syntaxError;
  }

  const header = new Uint32Array(result.buffer, 0, 12);

  if (header[0] !== CONTAINER_MAGIC || header[1] !== CONTAINER_VERSION) {
    throw new Error(
      'hermes-parser-native: unrecognized parse container ' +
        `(magic ${header[0]}, version ${header[1]})`,
    );
  }

  if (header[2] !== EXPECTED_KIND_HASH) {
    // Hex, because that is how the value is read on the C++ side (it is a
    // raw uint32 word in the container header) and how a hex dump of the
    // buffer shows it.
    throw new Error(
      'hermes-parser-native: node-kind table mismatch. The native addon ' +
        `reports hash 0x${header[2].toString(16)} but this JavaScript ` +
        `package was generated for 0x${EXPECTED_KIND_HASH.toString(16)}. ` +
        'The addon and the JavaScript package were built from different ' +
        'versions of ESTree.def.',
    );
  }

  const deserializer = new HermesParserDeserializer(
    result.buffer,
    header,
    options,
  );
  return deserializer.deserialize();
}
