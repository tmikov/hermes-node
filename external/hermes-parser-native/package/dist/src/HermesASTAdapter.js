"use strict";

Object.defineProperty(exports, "__esModule", {
  value: true
});
exports.default = void 0;

var _ParserVisitorKeys = require("./generated/ParserVisitorKeys");

class HermesASTAdapter {
  constructor(options) {
    this.sourceFilename = void 0;
    this.sourceType = void 0;
    this.sourceFilename = options.sourceFilename;
    this.sourceType = options.sourceType;
  }

  transform(program) {
    const comments = program.comments;

    for (let i = 0; i < comments.length; i++) {
      const comment = comments[i];
      this.fixSourceLocation(comment);
      comments[i] = this.mapComment(comment);
    }

    program.interpreter = comments.length > 0 && comments[0].type === 'InterpreterDirective' ? comments.shift() : null;
    const tokens = program.tokens;

    if (tokens) {
      for (let i = 0; i < tokens.length; i++) {
        this.fixSourceLocation(tokens[i]);
      }
    }

    const resultNode = this.mapNode(program);

    if (resultNode.type !== 'Program') {
      throw new Error(`HermesToESTreeAdapter: Must return a Program node, instead of "${resultNode.type}". `);
    }

    return resultNode;
  }

  mapNode(_node) {
    throw new Error('Implemented in subclasses');
  }

  mapNodeDefault(node) {
    const visitorKeys = _ParserVisitorKeys.HERMES_AST_VISITOR_KEYS[node.type];

    for (const key in visitorKeys) {
      const childType = visitorKeys[key];

      if (childType === _ParserVisitorKeys.NODE_CHILD) {
        const child = node[key];

        if (child != null) {
          node[key] = this.mapNode(child);
        }
      } else if (childType === _ParserVisitorKeys.NODE_LIST_CHILD) {
        const children = node[key];

        for (let i = 0; i < children.length; i++) {
          const child = children[i];

          if (child != null) {
            children[i] = this.mapNode(child);
          }
        }
      }
    }

    return node;
  }

  fixSourceLocation(_node) {
    throw new Error('Implemented in subclasses');
  }

  getSourceType() {
    var _this$sourceType;

    return (_this$sourceType = this.sourceType) != null ? _this$sourceType : 'script';
  }

  setModuleSourceType() {
    if (this.sourceType == null) {
      this.sourceType = 'module';
    }
  }

  mapComment(node) {
    return node;
  }

  mapEmpty(_node) {
    return null;
  }

  mapImportDeclaration(node) {
    if (node.importKind === 'value') {
      this.setModuleSourceType();
    }

    return this.mapNodeDefault(node);
  }

  mapImportSpecifier(node) {
    if (node.importKind === 'value') {
      node.importKind = null;
    }

    return this.mapNodeDefault(node);
  }

  mapExportDefaultDeclaration(node) {
    this.setModuleSourceType();
    return this.mapNodeDefault(node);
  }

  mapExportNamedDeclaration(node) {
    if (node.exportKind === 'value') {
      this.setModuleSourceType();
    }

    return this.mapNodeDefault(node);
  }

  mapExportAllDeclaration(node) {
    if (node.exportKind === 'value') {
      this.setModuleSourceType();
    }

    return this.mapNodeDefault(node);
  }

  formatError(node, message) {
    return `${message} (${node.loc.start.line}:${node.loc.start.column})`;
  }

  getBigIntLiteralValue(bigintString) {
    const bigint = bigintString.replace(/n$/, '').replaceAll('_', '');
    return {
      bigint,
      value: typeof BigInt === 'function' ? BigInt(bigint) : null
    };
  }

}

exports.default = HermesASTAdapter;