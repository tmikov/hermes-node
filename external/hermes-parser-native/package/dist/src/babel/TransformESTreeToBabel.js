'use strict';

Object.defineProperty(exports, "__esModule", {
  value: true
});
exports.transformProgram = transformProgram;

var _SimpleTransform = require("../transform/SimpleTransform");

var _SimpleTraverser = require("../traverse/SimpleTraverser");

var _ESTreeVisitorKeys = _interopRequireDefault(require("../generated/ESTreeVisitorKeys"));

var _createSyntaxError = require("../utils/createSyntaxError");

function _interopRequireDefault(obj) { return obj && obj.__esModule ? obj : { default: obj }; }

const EMPTY_PARENT = null;
const FlowESTreeAndBabelVisitorKeys = { ..._ESTreeVisitorKeys.default,
  BigIntLiteral: [],
  BlockStatement: ['directives', ..._ESTreeVisitorKeys.default.BlockStatement],
  BooleanLiteral: [],
  ClassExpression: ['superTypeParameters', ..._ESTreeVisitorKeys.default.ClassExpression],
  ClassDeclaration: ['superTypeParameters', ..._ESTreeVisitorKeys.default.ClassDeclaration],
  ClassMethod: ['key', 'params', 'body', 'returnType', 'typeParameters'],
  ClassPrivateMethod: ['key', 'params', 'body', 'returnType', 'typeParameters'],
  ClassProperty: ['key', 'value', 'typeAnnotation', 'variance'],
  ClassPrivateProperty: ['key', 'value', 'typeAnnotation', 'variance'],
  Directive: ['value'],
  DirectiveLiteral: [],
  ExportNamespaceSpecifier: ['exported'],
  File: ['program', 'comments'],
  Import: [],
  NullLiteral: [],
  NumericLiteral: [],
  ObjectMethod: [..._ESTreeVisitorKeys.default.Property, 'params', 'body', 'returnType', 'typeParameters'],
  ObjectProperty: _ESTreeVisitorKeys.default.Property,
  OptionalCallExpression: ['callee', 'arguments', 'typeArguments'],
  OptionalMemberExpression: ['object', 'property'],
  PrivateName: ['id'],
  Program: ['directives', ..._ESTreeVisitorKeys.default.Program],
  RegExpLiteral: [],
  RestElement: [..._ESTreeVisitorKeys.default.RestElement, 'typeAnnotation'],
  StringLiteral: [],
  TupleTypeAnnotation: ['types'],
  CommentBlock: [],
  CommentLine: []
};

function nodeWith(node, overrideProps) {
  return _SimpleTransform.SimpleTransform.nodeWith(node, overrideProps, FlowESTreeAndBabelVisitorKeys);
}

function fixSourceLocation(node, _options) {
  const loc = node.loc;

  if (loc == null) {
    return;
  }

  node.loc = {
    start: loc.start,
    end: loc.end
  };

  if (node.type === 'Identifier') {
    node.loc.identifierName = node.name;
  }

  node.start = node.range[0];
  node.end = node.range[1];
  delete node.range;
  delete node.parent;
}

function mapNodeWithDirectives(node) {
  if (node.directives != null) {
    return node;
  }

  const directives = [];

  for (const child of node.body) {
    if (child.type === 'ExpressionStatement' && child.directive != null) {
      directives.push({
        type: 'Directive',
        value: {
          type: 'DirectiveLiteral',
          value: child.directive,
          extra: {
            rawValue: child.directive,
            raw: child.expression.type === 'Literal' ? child.expression.raw : ''
          },
          loc: child.expression.loc,
          range: child.expression.range,
          parent: EMPTY_PARENT
        },
        loc: child.loc,
        range: child.range,
        parent: node
      });
    } else {
      break;
    }
  }

  return nodeWith(node, {
    directives,
    body: directives.length === 0 ? node.body : node.body.slice(directives.length)
  });
}

function mapProgram(node) {
  var _program$directives;

  const program = mapNodeWithDirectives(node);
  const startLoc = {
    line: 1,
    column: 0
  };
  let endLoc = program.loc.end;
  let endRange = program.range[1];

  if (program.comments.length > 0) {
    const lastComment = program.comments[program.comments.length - 1];

    if (lastComment.range[1] > endRange) {
      endLoc = lastComment.loc.end;
      endRange = lastComment.range[1];
    }
  }

  const loc = {
    start: startLoc,
    end: endLoc
  };
  const range = [0, endRange];
  const babelComments = program.comments.map(comment => {
    switch (comment.type) {
      case 'Line':
        {
          return {
            type: 'CommentLine',
            value: comment.value,
            loc: comment.loc,
            range: comment.range
          };
        }

      case 'Block':
        {
          return {
            type: 'CommentBlock',
            value: comment.value,
            loc: comment.loc,
            range: comment.range
          };
        }
    }
  });
  return {
    type: 'File',
    program: {
      type: 'Program',
      body: program.body,
      directives: (_program$directives = program.directives) != null ? _program$directives : [],
      sourceType: program.sourceType,
      interpreter: program.interpreter,
      loc,
      range,
      parent: EMPTY_PARENT
    },
    comments: babelComments,
    loc,
    range,
    parent: EMPTY_PARENT
  };
}

function mapTemplateElement(node) {
  const startCharsToExclude = 1;
  const endCharsToExclude = node.tail ? 1 : 2;
  node.loc = {
    start: {
      line: node.loc.start.line,
      column: node.loc.start.column + startCharsToExclude
    },
    end: {
      line: node.loc.end.line,
      column: node.loc.end.column - endCharsToExclude
    }
  };
  node.range = [node.range[0] + startCharsToExclude, node.range[1] - endCharsToExclude];
  return node;
}

function mapProperty(node) {
  const key = node.key;
  const value = node.value;

  if (node.method || node.kind !== 'init') {
    if (value.type !== 'FunctionExpression') {
      throw (0, _createSyntaxError.createSyntaxError)(node, `Invalid method property, the value must be a "FunctionExpression" or "ArrowFunctionExpression. Instead got "${value.type}".`);
    }

    const newNode = {
      type: 'ObjectMethod',
      kind: node.kind === 'init' ? 'method' : node.kind,
      method: node.kind === 'init' ? true : false,
      computed: node.computed,
      key: key,
      id: null,
      params: value.params,
      body: value.body,
      async: value.async,
      generator: value.generator,
      returnType: value.returnType,
      typeParameters: value.typeParameters,
      loc: node.loc,
      range: node.range,
      parent: node.parent
    };

    if (node.kind !== 'init') {
      newNode.variance = null;
    }

    return newNode;
  }

  const propertyValue = value;
  return {
    type: 'ObjectProperty',
    computed: node.computed,
    key: node.key,
    value: propertyValue,
    method: node.method,
    shorthand: node.shorthand,
    loc: node.loc,
    range: node.range,
    parent: node.parent
  };
}

function mapMethodDefinition(node) {
  const value = node.value;
  const BaseClassMethod = {
    kind: node.kind,
    computed: node.computed,
    static: node.static,
    key: node.key,
    id: null,
    params: value.params,
    body: value.body,
    async: value.async,
    generator: value.generator,
    returnType: value.returnType,
    typeParameters: value.typeParameters,
    predicate: null,
    loc: node.loc,
    range: node.range,
    parent: node.parent
  };

  if (node.key.type === 'PrivateIdentifier') {
    return { ...BaseClassMethod,
      type: 'ClassPrivateMethod'
    };
  }

  return { ...BaseClassMethod,
    type: 'ClassMethod'
  };
}

function mapExportAllDeclaration(node) {
  if (node.exported != null) {
    return {
      type: 'ExportNamedDeclaration',
      declaration: null,
      specifiers: [{
        type: 'ExportNamespaceSpecifier',
        exported: node.exported,
        loc: {
          start: {
            column: node.loc.start.column + 'export '.length,
            line: node.loc.start.line
          },
          end: node.exported.loc.end
        },
        range: [node.range[0] + 'export '.length, node.exported.range[1]],
        parent: EMPTY_PARENT
      }],
      source: node.source,
      exportKind: node.exportKind,
      loc: node.loc,
      range: node.range,
      parent: node.parent
    };
  }

  delete node.exported;
  return node;
}

function mapRestElement(node) {
  const argument = node.argument;

  if ((argument.type === 'Identifier' || argument.type === 'ObjectPattern' || argument.type === 'ArrayPattern') && argument.typeAnnotation != null) {
    const argumentRange = argument.range;
    let argumentLoc = argument.loc;

    if (argument.type === 'Identifier') {
      argumentRange[1] = argumentRange[0] + argument.name.length;
      argumentLoc = {
        start: argumentLoc.start,
        end: {
          line: argumentLoc.start.line,
          column: argumentLoc.start.column + argument.name.length
        }
      };
    }

    return nodeWith(node, {
      typeAnnotation: argument.typeAnnotation,
      argument: nodeWith(argument, {
        typeAnnotation: null,
        range: argumentRange,
        loc: argumentLoc
      })
    });
  }

  return node;
}

function mapImportExpression(node) {
  return {
    type: 'CallExpression',
    callee: {
      type: 'Import',
      loc: {
        start: node.loc.start,
        end: {
          line: node.loc.start.line,
          column: node.loc.start.column + 'import'.length
        }
      },
      range: [node.range[0], node.range[0] + 'import'.length],
      parent: EMPTY_PARENT
    },
    arguments: [node.source],
    loc: node.loc,
    range: node.range,
    parent: node.parent
  };
}

function mapPrivateIdentifier(node) {
  return {
    type: 'PrivateName',
    id: {
      type: 'Identifier',
      name: node.name,
      optional: false,
      typeAnnotation: null,
      loc: {
        start: {
          line: node.loc.start.line,
          column: node.loc.start.column + 1
        },
        end: node.loc.end
      },
      range: [node.range[0] + 1, node.range[1]],
      parent: EMPTY_PARENT
    },
    loc: node.loc,
    range: node.range,
    parent: node.parent
  };
}

function mapPropertyDefinition(node) {
  if (node.key.type === 'PrivateIdentifier') {
    return {
      type: 'ClassPrivateProperty',
      key: mapPrivateIdentifier(node.key),
      value: node.value,
      typeAnnotation: node.typeAnnotation,
      static: node.static,
      variance: node.variance,
      loc: node.loc,
      range: node.range,
      parent: node.parent
    };
  }

  return {
    type: 'ClassProperty',
    key: node.key,
    value: node.value,
    typeAnnotation: node.typeAnnotation,
    static: node.static,
    variance: node.variance,
    declare: node.declare,
    optional: node.optional,
    computed: node.computed,
    loc: node.loc,
    range: node.range,
    parent: node.parent
  };
}

function mapTypeofTypeAnnotation(node) {
  delete node.typeArguments;

  if (node.argument.type !== 'GenericTypeAnnotation') {
    return nodeWith(node, {
      argument: {
        type: 'GenericTypeAnnotation',
        id: node.argument,
        typeParameters: null,
        loc: node.argument.loc,
        range: node.argument.range,
        parent: EMPTY_PARENT
      }
    });
  }

  return node;
}

function mapDeclareVariable(node) {
  if (node.kind != null) {
    delete node.kind;
  }

  return node;
}

function mapJSXElement(node) {
  if (node.openingElement.typeArguments != null) {
    delete node.openingElement.typeArguments;
  }

  return node;
}

function mapChainExpressionInnerNode(node) {
  switch (node.type) {
    case 'MemberExpression':
      {
        const innerObject = mapChainExpressionInnerNode(node.object);

        if (!node.optional && innerObject.type !== 'OptionalMemberExpression' && innerObject.type !== 'OptionalCallExpression') {
          return node;
        }

        return {
          type: 'OptionalMemberExpression',
          object: innerObject,
          property: node.property,
          computed: node.computed,
          optional: node.optional,
          loc: node.loc,
          range: node.range,
          parent: node.parent
        };
      }

    case 'CallExpression':
      {
        const innerCallee = mapChainExpressionInnerNode(node.callee);

        if (!node.optional && innerCallee.type !== 'OptionalMemberExpression' && innerCallee.type !== 'OptionalCallExpression') {
          return node;
        }

        return {
          type: 'OptionalCallExpression',
          callee: innerCallee,
          optional: node.optional,
          arguments: node.arguments,
          typeArguments: node.typeArguments,
          loc: node.loc,
          range: node.range,
          parent: node.parent
        };
      }

    default:
      {
        return node;
      }
  }
}

function mapChainExpression(node) {
  return mapChainExpressionInnerNode(node.expression);
}

function mapLiteral(node) {
  const base = {
    loc: node.loc,
    range: node.range,
    parent: node.parent
  };

  switch (node.literalType) {
    case 'string':
      {
        return {
          type: 'StringLiteral',
          value: node.value,
          extra: {
            rawValue: node.value,
            raw: node.raw
          },
          ...base
        };
      }

    case 'numeric':
      {
        return {
          type: 'NumericLiteral',
          value: node.value,
          extra: {
            rawValue: node.value,
            raw: node.raw
          },
          ...base
        };
      }

    case 'bigint':
      {
        return {
          type: 'BigIntLiteral',
          value: node.bigint,
          extra: {
            rawValue: node.bigint,
            raw: node.raw
          },
          ...base
        };
      }

    case 'boolean':
      {
        return {
          type: 'BooleanLiteral',
          value: node.value,
          ...base
        };
      }

    case 'null':
      {
        return {
          type: 'NullLiteral',
          ...base
        };
      }

    case 'regexp':
      {
        return {
          type: 'RegExpLiteral',
          extra: {
            raw: node.raw
          },
          pattern: node.regex.pattern,
          flags: node.regex.flags,
          ...base
        };
      }
  }
}

function transformNode(node) {
  switch (node.type) {
    case 'Program':
      {
        var _node$parent;

        if (((_node$parent = node.parent) == null ? void 0 : _node$parent.type) === 'File') {
          return node;
        }

        return mapProgram(node);
      }

    case 'BlockStatement':
      {
        return mapNodeWithDirectives(node);
      }

    case 'Property':
      {
        return mapProperty(node);
      }

    case 'TemplateElement':
      {
        return mapTemplateElement(node);
      }

    case 'MethodDefinition':
      {
        return mapMethodDefinition(node);
      }

    case 'ExportAllDeclaration':
      {
        return mapExportAllDeclaration(node);
      }

    case 'RestElement':
      {
        return mapRestElement(node);
      }

    case 'ImportExpression':
      {
        return mapImportExpression(node);
      }

    case 'PrivateIdentifier':
      {
        return mapPrivateIdentifier(node);
      }

    case 'PropertyDefinition':
      {
        return mapPropertyDefinition(node);
      }

    case 'TypeofTypeAnnotation':
      {
        return mapTypeofTypeAnnotation(node);
      }

    case 'DeclareVariable':
      {
        return mapDeclareVariable(node);
      }

    case 'JSXElement':
      {
        return mapJSXElement(node);
      }

    case 'Literal':
      {
        return mapLiteral(node);
      }

    case 'ChainExpression':
      {
        return mapChainExpression(node);
      }

    case 'TypeCastExpression':
      {
        node.loc.start.column = node.loc.start.column + 1;
        node.loc.end.column = node.loc.end.column - 1;
        node.range = [node.range[0] + 1, node.range[1] - 1];
        return node;
      }

    case 'AsExpression':
      {
        const {
          typeAnnotation
        } = node;
        node.type = 'TypeCastExpression';
        node.typeAnnotation = {
          type: 'TypeAnnotation',
          typeAnnotation,
          loc: typeAnnotation.loc,
          range: typeAnnotation.range
        };
        return node;
      }

    case 'AsConstExpression':
      {
        return node.expression;
      }

    case 'NumberLiteralTypeAnnotation':
      {
        node.extra = {
          rawValue: node.value,
          raw: node.raw
        };
        delete node.raw;
        return node;
      }

    case 'StringLiteralTypeAnnotation':
      {
        node.extra = {
          rawValue: node.value,
          raw: node.raw
        };
        delete node.raw;
        return node;
      }

    case 'BigIntLiteralTypeAnnotation':
      {
        node.extra = {
          rawValue: node.bigint,
          raw: node.raw
        };
        delete node.bigint;
        delete node.raw;
        return node;
      }

    case 'BooleanLiteralTypeAnnotation':
      {
        delete node.raw;
        return node;
      }

    case 'TupleTypeAnnotation':
      {
        delete node.inexact;
        node.types = node.elementTypes;
        delete node.elementTypes;
        return node;
      }

    case 'UnknownTypeAnnotation':
      {
        return {
          type: 'GenericTypeAnnotation',
          id: {
            type: 'Identifier',
            name: 'unknown',
            optional: false,
            typeAnnotation: null,
            loc: node.loc,
            range: node.range,
            parent: EMPTY_PARENT
          },
          typeParameters: null,
          loc: node.loc,
          range: node.range,
          parent: EMPTY_PARENT
        };
      }

    case 'NeverTypeAnnotation':
      {
        return {
          type: 'GenericTypeAnnotation',
          id: {
            type: 'Identifier',
            name: 'never',
            optional: false,
            typeAnnotation: null,
            loc: node.loc,
            range: node.range,
            parent: EMPTY_PARENT
          },
          typeParameters: null,
          loc: node.loc,
          range: node.range,
          parent: EMPTY_PARENT
        };
      }

    case 'UndefinedTypeAnnotation':
      {
        return {
          type: 'GenericTypeAnnotation',
          id: {
            type: 'Identifier',
            name: 'undefined',
            optional: false,
            typeAnnotation: null,
            loc: node.loc,
            range: node.range,
            parent: EMPTY_PARENT
          },
          typeParameters: null,
          loc: node.loc,
          range: node.range,
          parent: EMPTY_PARENT
        };
      }

    case 'JSXText':
      {
        node.extra = {
          rawValue: node.value,
          raw: node.raw
        };
        delete node.raw;
        return node;
      }

    case 'Identifier':
      {
        if (node.optional === false || node.optional == null) {
          delete node.optional;
        }

        if (node.typeAnnotation == null) {
          delete node.typeAnnotation;
        }

        return node;
      }

    case 'CallExpression':
      {
        if (node.optional === false) {
          delete node.optional;
        }

        if (node.typeArguments == null) {
          delete node.typeArguments;
        }

        return node;
      }

    case 'OptionalCallExpression':
      {
        if (node.typeArguments == null) {
          delete node.typeArguments;
        }

        return node;
      }

    case 'MemberExpression':
      {
        delete node.optional;
        return node;
      }

    case 'ExpressionStatement':
      {
        delete node.directive;
        return node;
      }

    case 'ObjectPattern':
    case 'ArrayPattern':
      {
        if (node.typeAnnotation == null) {
          delete node.typeAnnotation;
        }

        return node;
      }

    case 'FunctionDeclaration':
    case 'FunctionExpression':
    case 'ArrowFunctionExpression':
    case 'ClassMethod':
      {
        delete node.expression;

        if (node.predicate == null) {
          delete node.predicate;
        }

        if (node.returnType == null) {
          delete node.returnType;
        }

        if (node.typeParameters == null) {
          delete node.typeParameters;
        }

        return node;
      }

    case 'ObjectMethod':
      {
        if (node.returnType == null) {
          delete node.returnType;
        }

        if (node.typeParameters == null) {
          delete node.typeParameters;
        }

        return node;
      }

    case 'ClassPrivateMethod':
      {
        if (node.computed === false) {
          delete node.computed;
        }

        if (node.predicate == null) {
          delete node.predicate;
        }

        if (node.returnType == null) {
          delete node.returnType;
        }

        if (node.typeParameters == null) {
          delete node.typeParameters;
        }

        return node;
      }

    case 'ClassExpression':
    case 'ClassDeclaration':
      {
        if (node.decorators == null || node.decorators.length === 0) {
          delete node.decorators;
        }

        if (node.implements == null || node.implements.length === 0) {
          delete node.implements;
        }

        if (node.superTypeArguments == null) {
          delete node.superTypeArguments;
        } else {
          node.superTypeParameters = node.superTypeArguments;
          delete node.superTypeArguments;
        }

        if (node.typeParameters == null) {
          delete node.typeParameters;
        }

        return node;
      }

    case 'ClassProperty':
      {
        if (node.optional === false) {
          delete node.optional;
        }

        if (node.declare === false) {
          delete node.declare;
        }

        if (node.typeAnnotation == null) {
          delete node.typeAnnotation;
        }

        return node;
      }

    case 'ClassPrivateProperty':
      {
        if (node.typeAnnotation == null) {
          delete node.typeAnnotation;
        }

        return node;
      }

    case 'ExportNamedDeclaration':
      {
        if (node.declaration == null) {
          delete node.declaration;
        }

        return node;
      }

    case 'ImportDeclaration':
      {
        if (node.attributes == null || node.attributes.length === 0) {
          delete node.attributes;
        }

        return node;
      }

    case 'ArrayExpression':
      {
        delete node.trailingComma;
        return node;
      }

    case 'JSXOpeningElement':
      {
        if (node.typeArguments == null) {
          delete node.typeArguments;
        }

        return node;
      }

    case 'DeclareOpaqueType':
    case 'OpaqueType':
      {
        if (node.lowerBound != null) {
          delete node.lowerBound;
        }

        if (node.upperBound != null) {
          delete node.upperBound;
        }

        return node;
      }

    default:
      {
        return node;
      }
  }
}

function transformProgram(program, options) {
  var _resultNode$type;

  const resultNode = _SimpleTransform.SimpleTransform.transform(program, {
    transform(node) {
      return transformNode(node);
    },

    visitorKeys: FlowESTreeAndBabelVisitorKeys
  });

  _SimpleTraverser.SimpleTraverser.traverse(resultNode, {
    enter(node) {
      fixSourceLocation(node, options);
    },

    leave() {},

    visitorKeys: FlowESTreeAndBabelVisitorKeys
  });

  if ((resultNode == null ? void 0 : resultNode.type) === 'File') {
    return resultNode;
  }

  throw new Error(`Unknown AST node of type "${(_resultNode$type = resultNode == null ? void 0 : resultNode.type) != null ? _resultNode$type : 'NULL'}" returned from Babel conversion`);
}