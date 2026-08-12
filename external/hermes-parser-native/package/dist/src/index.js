'use strict';

Object.defineProperty(exports, "__esModule", {
  value: true
});
var _exportNames = {
  parse: true,
  mutateESTreeASTForPrettier: true,
  Transforms: true,
  FlowVisitorKeys: true,
  astArrayMutationHelpers: true,
  astNodeMutationHelpers: true
};
exports.mutateESTreeASTForPrettier = exports.astNodeMutationHelpers = exports.astArrayMutationHelpers = exports.Transforms = void 0;
exports.parse = parse;

var HermesParser = _interopRequireWildcard(require("./HermesParser"));

var _HermesToESTreeAdapter = _interopRequireDefault(require("./HermesToESTreeAdapter"));

var _ESTreeVisitorKeys = _interopRequireDefault(require("./generated/ESTreeVisitorKeys"));

exports.FlowVisitorKeys = _ESTreeVisitorKeys.default;

var TransformComponentSyntax = _interopRequireWildcard(require("./estree/TransformComponentSyntax"));

var TransformEnumSyntax = _interopRequireWildcard(require("./estree/TransformEnumSyntax"));

var TransformMatchSyntax = _interopRequireWildcard(require("./estree/TransformMatchSyntax"));

var TransformRecordSyntax = _interopRequireWildcard(require("./estree/TransformRecordSyntax"));

var StripFlowTypesForBabel = _interopRequireWildcard(require("./estree/StripFlowTypesForBabel"));

var TransformESTreeToBabel = _interopRequireWildcard(require("./babel/TransformESTreeToBabel"));

var StripFlowTypes = _interopRequireWildcard(require("./estree/StripFlowTypes"));

var _ParserOptions = require("./ParserOptions");

Object.keys(_ParserOptions).forEach(function (key) {
  if (key === "default" || key === "__esModule") return;
  if (Object.prototype.hasOwnProperty.call(_exportNames, key)) return;
  if (key in exports && exports[key] === _ParserOptions[key]) return;
  exports[key] = _ParserOptions[key];
});

var _SimpleTraverser = require("./traverse/SimpleTraverser");

Object.keys(_SimpleTraverser).forEach(function (key) {
  if (key === "default" || key === "__esModule") return;
  if (Object.prototype.hasOwnProperty.call(_exportNames, key)) return;
  if (key in exports && exports[key] === _SimpleTraverser[key]) return;
  exports[key] = _SimpleTraverser[key];
});

var _SimpleTransform = require("./transform/SimpleTransform");

Object.keys(_SimpleTransform).forEach(function (key) {
  if (key === "default" || key === "__esModule") return;
  if (Object.prototype.hasOwnProperty.call(_exportNames, key)) return;
  if (key in exports && exports[key] === _SimpleTransform[key]) return;
  exports[key] = _SimpleTransform[key];
});

var _getVisitorKeys = require("./traverse/getVisitorKeys");

Object.keys(_getVisitorKeys).forEach(function (key) {
  if (key === "default" || key === "__esModule") return;
  if (Object.prototype.hasOwnProperty.call(_exportNames, key)) return;
  if (key in exports && exports[key] === _getVisitorKeys[key]) return;
  exports[key] = _getVisitorKeys[key];
});

var _astArrayMutationHelpers = _interopRequireWildcard(require("./transform/astArrayMutationHelpers"));

exports.astArrayMutationHelpers = _astArrayMutationHelpers;

var _astNodeMutationHelpers = _interopRequireWildcard(require("./transform/astNodeMutationHelpers"));

exports.astNodeMutationHelpers = _astNodeMutationHelpers;

var _mutateESTreeASTForPrettier = _interopRequireDefault(require("./utils/mutateESTreeASTForPrettier"));

exports.mutateESTreeASTForPrettier = _mutateESTreeASTForPrettier.default;

function _interopRequireDefault(obj) { return obj && obj.__esModule ? obj : { default: obj }; }

function _getRequireWildcardCache(nodeInterop) { if (typeof WeakMap !== "function") return null; var cacheBabelInterop = new WeakMap(); var cacheNodeInterop = new WeakMap(); return (_getRequireWildcardCache = function (nodeInterop) { return nodeInterop ? cacheNodeInterop : cacheBabelInterop; })(nodeInterop); }

function _interopRequireWildcard(obj, nodeInterop) { if (!nodeInterop && obj && obj.__esModule) { return obj; } if (obj === null || typeof obj !== "object" && typeof obj !== "function") { return { default: obj }; } var cache = _getRequireWildcardCache(nodeInterop); if (cache && cache.has(obj)) { return cache.get(obj); } var newObj = {}; var hasPropertyDescriptor = Object.defineProperty && Object.getOwnPropertyDescriptor; for (var key in obj) { if (key !== "default" && Object.prototype.hasOwnProperty.call(obj, key)) { var desc = hasPropertyDescriptor ? Object.getOwnPropertyDescriptor(obj, key) : null; if (desc && (desc.get || desc.set)) { Object.defineProperty(newObj, key, desc); } else { newObj[key] = obj[key]; } } } newObj.default = obj; if (cache) { cache.set(obj, newObj); } return newObj; }

const DEFAULTS = {
  flow: 'detect'
};

function getOptions(options = { ...DEFAULTS
}) {
  if (options.flow == null) {
    options.flow = DEFAULTS.flow;
  } else if (options.flow !== 'all' && options.flow !== 'detect') {
    throw new Error('flow option must be "all" or "detect"');
  }

  if (options.sourceType === 'unambiguous') {
    delete options.sourceType;
  } else if (options.sourceType != null && options.sourceType !== 'script' && options.sourceType !== 'module') {
    throw new Error('sourceType option must be "script", "module", or "unambiguous" if set');
  }

  if (options.enableExperimentalComponentSyntax == null) {
    options.enableExperimentalComponentSyntax = true;
  }

  if (options.enableExperimentalFlowMatchSyntax == null) {
    options.enableExperimentalFlowMatchSyntax = true;
  }

  if (options.enableExperimentalFlowRecordSyntax == null) {
    options.enableExperimentalFlowRecordSyntax = true;
  }

  options.tokens = options.tokens === true;
  options.allowReturnOutsideFunction = options.allowReturnOutsideFunction === true;
  return options;
}

function parse(code, opts) {
  var _options$transformOpt, _options$transformOpt2;

  const options = getOptions(opts);
  const ast = HermesParser.parse(code, options);
  const estreeAdapter = new _HermesToESTreeAdapter.default(options, code);
  const estreeAST = estreeAdapter.transform(ast);

  if (options.babel !== true) {
    return estreeAST;
  }

  const loweredESTreeAST = [(_options$transformOpt = options.transformOptions) != null && (_options$transformOpt2 = _options$transformOpt.TransformEnumSyntax) != null && _options$transformOpt2.enable ? TransformEnumSyntax.transformProgram : null, TransformMatchSyntax.transformProgram, TransformComponentSyntax.transformProgram, TransformRecordSyntax.transformProgram, StripFlowTypesForBabel.transformProgram].reduce((ast, transform) => {
    var _transform;

    return (_transform = transform == null ? void 0 : transform(ast, options)) != null ? _transform : ast;
  }, estreeAST);
  return TransformESTreeToBabel.transformProgram(loweredESTreeAST, options);
}

const Transforms = {
  transformEnumSyntax: TransformEnumSyntax.transformProgram,
  transformMatchSyntax: TransformMatchSyntax.transformProgram,
  transformComponentSyntax: TransformComponentSyntax.transformProgram,
  transformRecordSyntax: TransformRecordSyntax.transformProgram,
  stripFlowTypesForBabel: StripFlowTypesForBabel.transformProgram,
  stripFlowTypes: StripFlowTypes.transformProgram
};
exports.Transforms = Transforms;