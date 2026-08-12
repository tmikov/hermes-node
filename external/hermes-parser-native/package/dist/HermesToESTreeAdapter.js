"use strict";

Object.defineProperty(exports, "__esModule", {
  value: true
});
exports.default = void 0;

var _HermesASTAdapter = _interopRequireDefault(require("./HermesASTAdapter"));

var _getModuleDocblock = require("./getModuleDocblock");

function _interopRequireDefault(obj) { return obj && obj.__esModule ? obj : { default: obj }; }

class HermesToESTreeAdapter extends _HermesASTAdapter.default {
  constructor(options, code) {
    super(options);
    this.code = void 0;
    this.code = code;
  }

  fixSourceLocation(node) {
    var _this$sourceFilename;

    const loc = node.loc;

    if (loc == null) {
      return;
    }

    node.loc = {
      source: (_this$sourceFilename = this.sourceFilename) != null ? _this$sourceFilename : null,
      start: loc.start,
      end: loc.end
    };
    node.range = [loc.rangeStart, loc.rangeEnd];
    delete node.start;
    delete node.end;
  }

  mapNode(node) {
    this.fixSourceLocation(node);

    switch (node.type) {
      case 'Program':
        return this.mapProgram(node);

      case 'NullLiteral':
        return this.mapNullLiteral(node);

      case 'BooleanLiteral':
      case 'StringLiteral':
      case 'NumericLiteral':
      case 'JSXStringLiteral':
        return this.mapSimpleLiteral(node);

      case 'BigIntLiteral':
        return this.mapBigIntLiteral(node);

      case 'RegExpLiteral':
        return this.mapRegExpLiteral(node);

      case 'Empty':
        return this.mapEmpty(node);

      case 'TemplateElement':
        return this.mapTemplateElement(node);

      case 'BigIntLiteralTypeAnnotation':
        return this.mapBigIntLiteralTypeAnnotation(node);

      case 'GenericTypeAnnotation':
        return this.mapGenericTypeAnnotation(node);

      case 'ImportDeclaration':
        return this.mapImportDeclaration(node);

      case 'ImportSpecifier':
        return this.mapImportSpecifier(node);

      case 'ExportDefaultDeclaration':
        return this.mapExportDefaultDeclaration(node);

      case 'ExportNamedDeclaration':
        return this.mapExportNamedDeclaration(node);

      case 'ExportAllDeclaration':
        return this.mapExportAllDeclaration(node);

      case 'FunctionDeclaration':
      case 'FunctionExpression':
      case 'ArrowFunctionExpression':
        return this.mapFunction(node);

      case 'PrivateName':
        return this.mapPrivateName(node);

      case 'ClassProperty':
      case 'ClassPrivateProperty':
        return this.mapClassProperty(node);

      case 'MemberExpression':
      case 'OptionalMemberExpression':
      case 'CallExpression':
      case 'OptionalCallExpression':
        return this.mapChainExpression(node);

      case 'BlockStatement':
        return this.mapBlockStatement(node);

      default:
        return this.mapNodeDefault(node);
    }
  }

  mapProgram(node) {
    const nodeDefault = this.mapNodeDefault(node);
    node.sourceType = this.getSourceType();
    node.docblock = (0, _getModuleDocblock.getModuleDocblock)(nodeDefault);
    return nodeDefault;
  }

  mapSimpleLiteral(node) {
    return {
      type: 'Literal',
      loc: node.loc,
      range: node.range,
      value: node.value,
      raw: this.code.slice(node.range[0], node.range[1]),
      literalType: (() => {
        switch (node.type) {
          case 'NullLiteral':
            return 'null';

          case 'BooleanLiteral':
            return 'boolean';

          case 'StringLiteral':
          case 'JSXStringLiteral':
            return 'string';

          case 'NumericLiteral':
            return 'numeric';

          case 'BigIntLiteral':
            return 'bigint';

          case 'RegExpLiteral':
            return 'regexp';
        }

        return null;
      })()
    };
  }

  mapBigIntLiteral(node) {
    const newNode = this.mapSimpleLiteral(node);
    return { ...newNode,
      ...this.getBigIntLiteralValue(node.bigint)
    };
  }

  mapNullLiteral(node) {
    return { ...this.mapSimpleLiteral(node),
      value: null
    };
  }

  mapRegExpLiteral(node) {
    const {
      pattern,
      flags
    } = node;
    let value;

    try {
      value = new RegExp(pattern, flags);
    } catch {
      value = null;
    }

    return { ...this.mapSimpleLiteral(node),
      value,
      regex: {
        pattern,
        flags
      }
    };
  }

  mapBigIntLiteralTypeAnnotation(node) {
    return { ...node,
      ...this.getBigIntLiteralValue(node.raw)
    };
  }

  mapTemplateElement(node) {
    return {
      type: 'TemplateElement',
      loc: node.loc,
      range: node.range,
      tail: node.tail,
      value: {
        cooked: node.cooked,
        raw: node.raw
      }
    };
  }

  mapGenericTypeAnnotation(node) {
    if (node.typeParameters == null && node.id.type === 'Identifier' && node.id.name === 'this') {
      return {
        type: 'ThisTypeAnnotation',
        loc: node.loc,
        range: node.range
      };
    }

    return this.mapNodeDefault(node);
  }

  mapComment(node) {
    if (node.type === 'CommentBlock') {
      node.type = 'Block';
    } else if (node.type === 'CommentLine') {
      node.type = 'Line';
    }

    return node;
  }

  mapFunction(nodeUnprocessed) {
    const node = this.mapNodeDefault(nodeUnprocessed);

    switch (node.type) {
      case 'FunctionDeclaration':
      case 'FunctionExpression':
        node.expression = false;
        return node;

      case 'ArrowFunctionExpression':
        node.id = null;
        node.expression = node.body.type !== 'BlockStatement';
        return node;
    }

    return node;
  }

  mapChainExpression(nodeUnprocessed) {
    const node = this.mapNodeDefault(nodeUnprocessed);

    const {
      child,
      childKey,
      isOptional
    } = (() => {
      const isOptional = node.optional === true;

      if (node.type.endsWith('MemberExpression')) {
        return {
          child: node.object,
          childKey: 'object',
          isOptional
        };
      } else if (node.type.endsWith('CallExpression')) {
        return {
          child: node.callee,
          childKey: 'callee',
          isOptional
        };
      } else {
        return {
          child: node.expression,
          childKey: 'expression',
          isOptional: false
        };
      }
    })();

    const isChildUnwrappable = child.type === 'ChainExpression' && node.type !== 'MemberExpression' && node.type !== 'CallExpression';

    if (node.type.startsWith('Optional')) {
      node.type = node.type.replace('Optional', '');
      node.optional = isOptional;
    } else {
      node.optional = false;
    }

    if (!isChildUnwrappable && !isOptional) {
      return node;
    }

    if (isChildUnwrappable) {
      const newChild = child.expression;
      node[childKey] = newChild;
    }

    return {
      type: 'ChainExpression',
      expression: node,
      loc: node.loc,
      range: node.range
    };
  }

  mapClassProperty(nodeUnprocessed) {
    const node = this.mapNodeDefault(nodeUnprocessed);

    const key = (() => {
      if (node.type === 'ClassPrivateProperty') {
        const key = this.mapNodeDefault(node.key);
        return {
          type: 'PrivateIdentifier',
          name: key.name,
          range: key.range,
          loc: key.loc
        };
      }

      return node.key;
    })();

    return { ...node,
      computed: node.type === 'ClassPrivateProperty' ? false : node.computed,
      key,
      type: 'PropertyDefinition'
    };
  }

  mapPrivateName(node) {
    return {
      type: 'PrivateIdentifier',
      name: node.id.name,
      range: node.range,
      loc: node.loc
    };
  }

  mapExportNamedDeclaration(nodeUnprocessed) {
    const node = super.mapExportNamedDeclaration(nodeUnprocessed);
    const namespaceSpecifier = node.specifiers.find(spec => spec.type === 'ExportNamespaceSpecifier');

    if (namespaceSpecifier != null) {
      var _node$exportKind;

      if (node.specifiers.length !== 1) {
        throw new Error('Cannot use an export all with any other specifiers');
      }

      return {
        type: 'ExportAllDeclaration',
        source: node.source,
        exportKind: (_node$exportKind = node.exportKind) != null ? _node$exportKind : 'value',
        exported: namespaceSpecifier.exported,
        range: node.range,
        loc: node.loc
      };
    }

    return node;
  }

  mapExportAllDeclaration(nodeUnprocessed) {
    var _node$exported;

    const node = super.mapExportAllDeclaration(nodeUnprocessed);
    node.exported = (_node$exported = node.exported) != null ? _node$exported : null;
    return node;
  }

  mapBlockStatement(node) {
    if (node.implicit && node.body.length) {
      return this.mapNode(node.body[0]);
    }

    delete node.implicit;
    return this.mapNodeDefault(node);
  }

}

exports.default = HermesToESTreeAdapter;