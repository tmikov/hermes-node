"use strict";

Object.defineProperty(exports, "__esModule", {
  value: true
});
exports.ParserOptionsKeys = void 0;

/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 *
 *  strict
 * @noformat
 */
const ParserOptionsKeys
/*: ReadonlySet<keyof ParserOptions> */
= new Set(['allowReturnOutsideFunction', 'babel', 'flow', 'enableExperimentalComponentSyntax', 'enableExperimentalFlowMatchSyntax', 'enableExperimentalFlowRecordSyntax', 'reactRuntimeTarget', 'sourceFilename', 'sourceType', 'tokens', 'transformOptions']);
exports.ParserOptionsKeys = ParserOptionsKeys;