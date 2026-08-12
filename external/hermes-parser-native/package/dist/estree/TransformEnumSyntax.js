'use strict';

Object.defineProperty(exports, "__esModule", {
  value: true
});
exports.transformProgram = transformProgram;

var _hermesEstree = require("hermes-estree");

var _SimpleTransform = require("../transform/SimpleTransform");

var _Builders = require("../utils/Builders");

function mapEnumDeclaration(node, options) {
  var _options$transformOpt, _options$transformOpt2;

  const {
    body
  } = node;
  const {
    members
  } = body;
  const getRuntime = (_options$transformOpt = options.transformOptions) == null ? void 0 : (_options$transformOpt2 = _options$transformOpt.TransformEnumSyntax) == null ? void 0 : _options$transformOpt2.getRuntime;
  const enumModule = typeof getRuntime === 'function' ? getRuntime() : (0, _Builders.callExpression)((0, _Builders.ident)('require'), [(0, _Builders.stringLiteral)('flow-enums-runtime')]);
  const mirrored = body.type === 'EnumStringBody' && (!members.length || members[0].type === 'EnumDefaultedMember');
  const enumExpression = mirrored ? (0, _Builders.callExpression)({
    type: 'MemberExpression',
    object: enumModule,
    property: (0, _Builders.ident)('Mirrored'),
    computed: false,
    optional: false,
    ...(0, _Builders.etc)()
  }, [{
    type: 'ArrayExpression',
    elements: members.map(member => (0, _Builders.stringLiteral)(member.id.name)),
    trailingComma: false,
    ...(0, _Builders.etc)()
  }]) : (0, _Builders.callExpression)(enumModule, [{
    type: 'ObjectExpression',
    properties: members.map(member => ({
      type: 'Property',
      key: member.id,
      value: member.type === 'EnumDefaultedMember' ? (0, _Builders.callExpression)((0, _Builders.ident)('Symbol'), [(0, _Builders.stringLiteral)(member.id.name)]) : member.init,
      kind: 'init',
      method: false,
      shorthand: false,
      computed: false,
      ...(0, _Builders.etc)(),
      parent: _Builders.EMPTY_PARENT
    })),
    ...(0, _Builders.etc)()
  }]);
  return (0, _Builders.variableDeclaration)('const', node.id, enumExpression);
}

function transformProgram(program, options) {
  return _SimpleTransform.SimpleTransform.transformProgram(program, {
    transform(node) {
      switch (node.type) {
        case 'EnumDeclaration':
          {
            return mapEnumDeclaration(node, options);
          }

        case 'ExportDefaultDeclaration':
          {
            const {
              declaration
            } = node;

            if ((0, _hermesEstree.isEnumDeclaration)(declaration)) {
              const enumDeclaration = mapEnumDeclaration(declaration, options);

              const exportDefault = _SimpleTransform.SimpleTransform.nodeWith(node, {
                declaration: (0, _Builders.ident)(declaration.id.name)
              });

              return [enumDeclaration, exportDefault];
            } else {
              return node;
            }
          }

        default:
          {
            return node;
          }
      }
    }

  });
}