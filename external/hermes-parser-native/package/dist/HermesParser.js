'use strict';

Object.defineProperty(exports, "__esModule", {
  value: true
});
exports.parse = parse;

var _HermesParserDeserializer = _interopRequireDefault(require("./HermesParserDeserializer"));

var _HermesParserKindHash = _interopRequireDefault(require("./HermesParserKindHash"));

function _interopRequireDefault(obj) { return obj && obj.__esModule ? obj : { default: obj }; }

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

function parse(source, options) {
  const result = getAddon().parse(source, {
    detectFlow: options.flow === 'detect',
    enableExperimentalComponentSyntax: options.enableExperimentalComponentSyntax === true,
    enableExperimentalFlowMatchSyntax: options.enableExperimentalFlowMatchSyntax === true,
    enableExperimentalFlowRecordSyntax: options.enableExperimentalFlowRecordSyntax === true,
    tokens: options.tokens === true,
    allowReturnOutsideFunction: options.allowReturnOutsideFunction === true
  });

  if (result.error != null) {
    const syntaxError = new SyntaxError(result.error);
    syntaxError.loc = {
      line: result.line,
      column: result.column
    };
    throw syntaxError;
  }

  const header = new Uint32Array(result.buffer, 0, 12);

  if (header[0] !== CONTAINER_MAGIC || header[1] !== CONTAINER_VERSION) {
    throw new Error('hermes-parser-native: unrecognized parse container ' + `(magic ${header[0]}, version ${header[1]})`);
  }

  if (header[2] !== _HermesParserKindHash.default) {
    throw new Error('hermes-parser-native: node-kind table mismatch. The native addon ' + `reports hash 0x${header[2].toString(16)} but this JavaScript ` + `package was generated for 0x${_HermesParserKindHash.default.toString(16)}. ` + 'The addon and the JavaScript package were built from different ' + 'versions of ESTree.def.');
  }

  const deserializer = new _HermesParserDeserializer.default(result.buffer, header, options);
  return deserializer.deserialize();
}