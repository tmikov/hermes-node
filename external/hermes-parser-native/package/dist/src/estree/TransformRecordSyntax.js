'use strict';

Object.defineProperty(exports, "__esModule", {
  value: true
});
exports.transformProgram = transformProgram;

var _hermesEstree = require("hermes-estree");

var _SimpleTransform = require("../transform/SimpleTransform");

var _astNodeMutationHelpers = require("../transform/astNodeMutationHelpers");

var _Builders = require("../utils/Builders");

var _isReservedWord = _interopRequireDefault(require("../utils/isReservedWord"));

var _GenID = _interopRequireDefault(require("../utils/GenID"));

function _interopRequireDefault(obj) { return obj && obj.__esModule ? obj : { default: obj }; }

function nameOfKey(key) {
  switch (key.type) {
    case 'Identifier':
      return key.name;

    case 'Literal':
      if ((0, _hermesEstree.isBigIntLiteral)(key)) {
        return key.bigint;
      }

      return String(key.value);
  }
}

function mapRecordDeclaration(genID, node) {
  const ownProperties = [];
  const staticProperties = [];
  const methods = [];
  const staticMethods = [];

  for (const element of node.body.elements) {
    switch (element.type) {
      case 'RecordDeclarationProperty':
        ownProperties.push(element);
        break;

      case 'RecordDeclarationStaticProperty':
        staticProperties.push(element);
        break;

      case 'MethodDefinition':
        if (element.static) {
          staticMethods.push(element);
        } else {
          methods.push(element);
        }

        break;
    }
  }

  const reservedPropNames = new Map();
  const constructorParam = {
    type: 'ObjectPattern',
    properties: ownProperties.map(prop => {
      const {
        key,
        defaultValue
      } = prop;
      const keyName = nameOfKey(key);

      const getValue = bindingIdent => defaultValue != null ? {
        type: 'AssignmentPattern',
        left: bindingIdent,
        right: (0, _astNodeMutationHelpers.deepCloneNode)(defaultValue),
        ...(0, _Builders.etc)()
      } : bindingIdent;

      switch (key.type) {
        case 'Identifier':
          {
            const needsNewBinding = (0, _isReservedWord.default)(keyName);
            const bindingName = needsNewBinding ? genID.id() : keyName;
            const bindingIdent = (0, _Builders.ident)(bindingName);

            if (needsNewBinding) {
              reservedPropNames.set(keyName, bindingName);
            }

            if (needsNewBinding) {
              return {
                type: 'Property',
                kind: 'init',
                key: (0, _astNodeMutationHelpers.shallowCloneNode)(key),
                value: getValue(bindingIdent),
                shorthand: false,
                method: false,
                computed: false,
                ...(0, _Builders.etc)(),
                parent: _Builders.EMPTY_PARENT
              };
            } else {
              return {
                type: 'Property',
                kind: 'init',
                key: (0, _astNodeMutationHelpers.shallowCloneNode)(key),
                value: getValue(bindingIdent),
                shorthand: true,
                method: false,
                computed: false,
                ...(0, _Builders.etc)(),
                parent: _Builders.EMPTY_PARENT
              };
            }
          }

        case 'Literal':
          {
            const bindingName = genID.id();
            const bindingIdent = (0, _Builders.ident)(bindingName);
            reservedPropNames.set(keyName, bindingName);
            return {
              type: 'Property',
              kind: 'init',
              key: (0, _astNodeMutationHelpers.shallowCloneNode)(key),
              value: getValue(bindingIdent),
              shorthand: false,
              method: false,
              computed: false,
              ...(0, _Builders.etc)(),
              parent: _Builders.EMPTY_PARENT
            };
          }
      }
    }),
    typeAnnotation: null,
    ...(0, _Builders.etc)()
  };
  const constructor = {
    type: 'MethodDefinition',
    key: (0, _Builders.ident)('constructor'),
    kind: 'constructor',
    computed: false,
    static: false,
    decorators: [],
    value: {
      type: 'FunctionExpression',
      id: null,
      params: [constructorParam],
      body: {
        type: 'BlockStatement',
        body: ownProperties.map(({
          key
        }) => {
          var _reservedPropNames$ge;

          const keyName = nameOfKey(key);
          const bindingIdent = (0, _Builders.ident)((_reservedPropNames$ge = reservedPropNames.get(keyName)) != null ? _reservedPropNames$ge : keyName);
          const object = {
            type: 'ThisExpression',
            ...(0, _Builders.etc)()
          };
          const memberExpression = key.type === 'Identifier' ? {
            type: 'MemberExpression',
            object,
            property: (0, _astNodeMutationHelpers.shallowCloneNode)(key),
            computed: false,
            optional: false,
            ...(0, _Builders.etc)()
          } : {
            type: 'MemberExpression',
            object,
            property: (0, _astNodeMutationHelpers.shallowCloneNode)(key),
            computed: true,
            optional: false,
            ...(0, _Builders.etc)()
          };
          return {
            type: 'ExpressionStatement',
            expression: {
              type: 'AssignmentExpression',
              operator: '=',
              left: memberExpression,
              right: bindingIdent,
              ...(0, _Builders.etc)()
            },
            directive: null,
            ...(0, _Builders.etc)()
          };
        }),
        ...(0, _Builders.etc)()
      },
      generator: false,
      async: false,
      predicate: null,
      returnType: null,
      typeParameters: null,
      ...(0, _Builders.etc)()
    },
    ...(0, _Builders.etc)(),
    parent: _Builders.EMPTY_PARENT
  };
  const classStaticProperties = staticProperties.map(prop => ({
    type: 'PropertyDefinition',
    key: (0, _astNodeMutationHelpers.shallowCloneNode)(prop.key),
    value: (0, _astNodeMutationHelpers.deepCloneNode)(prop.value),
    static: true,
    decorators: [],
    typeAnnotation: null,
    variance: null,
    computed: false,
    declare: false,
    optional: false,
    ...(0, _Builders.etc)(),
    parent: _Builders.EMPTY_PARENT
  }));
  const classBodyElements = [constructor, ...methods, ...classStaticProperties, ...staticMethods];
  return {
    type: 'ClassDeclaration',
    id: (0, _astNodeMutationHelpers.shallowCloneNode)(node.id),
    body: {
      type: 'ClassBody',
      body: classBodyElements,
      ...(0, _Builders.etc)(),
      parent: _Builders.EMPTY_PARENT
    },
    superClass: null,
    typeParameters: null,
    superTypeArguments: null,
    implements: [],
    decorators: [],
    ...(0, _Builders.etc)()
  };
}

function mapRecordExpression(node) {
  const obj = {
    type: 'ObjectExpression',
    properties: node.properties.properties,
    ...(0, _Builders.etc)()
  };
  return {
    type: 'NewExpression',
    callee: node.recordConstructor,
    arguments: [obj],
    typeArguments: null,
    ...(0, _Builders.etc)()
  };
}

function transformProgram(program, _options) {
  const genID = new _GenID.default('r');
  return _SimpleTransform.SimpleTransform.transformProgram(program, {
    transform(node) {
      switch (node.type) {
        case 'RecordDeclaration':
          {
            return mapRecordDeclaration(genID, node);
          }

        case 'RecordExpression':
          {
            return mapRecordExpression(node);
          }

        case 'Identifier':
          {
            genID.addUsage(node.name);
            return node;
          }

        default:
          return node;
      }
    }

  });
}