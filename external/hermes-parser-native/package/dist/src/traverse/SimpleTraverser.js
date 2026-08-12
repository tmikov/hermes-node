'use strict';

Object.defineProperty(exports, "__esModule", {
  value: true
});
exports.SimpleTraverserSkip = exports.SimpleTraverserBreak = exports.SimpleTraverser = void 0;

var _getVisitorKeys = require("./getVisitorKeys");

const SimpleTraverserSkip = new Error();
exports.SimpleTraverserSkip = SimpleTraverserSkip;
const SimpleTraverserBreak = new Error();
exports.SimpleTraverserBreak = SimpleTraverserBreak;

class SimpleTraverser {
  traverse(node, options) {
    try {
      this._traverse(node, null, options);
    } catch (ex) {
      if (ex === SimpleTraverserBreak) {
        return;
      }

      throw ex;
    }
  }

  _traverse(node, parent, options) {
    if (!(0, _getVisitorKeys.isNode)(node)) {
      return;
    }

    try {
      options.enter(node, parent);
    } catch (ex) {
      if (ex === SimpleTraverserSkip) {
        return;
      }

      this._setErrorContext(ex, node);

      throw ex;
    }

    const keys = (0, _getVisitorKeys.getVisitorKeys)(node, options.visitorKeys);

    for (const key of keys) {
      const lookupKey = key;
      const childAny = node[lookupKey];
      const child = childAny;

      if (Array.isArray(child)) {
        for (let j = 0; j < child.length; ++j) {
          this._traverse(child[j], node, options);
        }
      } else {
        this._traverse(child, node, options);
      }
    }

    try {
      options.leave(node, parent);
    } catch (ex) {
      if (ex === SimpleTraverserSkip) {
        return;
      }

      this._setErrorContext(ex, node);

      throw ex;
    }
  }

  _setErrorContext(ex, node) {
    ex.currentNode = {
      type: node.type,
      range: node.range,
      loc: node.loc
    };
  }

  static traverse(node, options) {
    new SimpleTraverser().traverse(node, options);
  }

}

exports.SimpleTraverser = SimpleTraverser;
SimpleTraverser.Break = SimpleTraverserBreak;
SimpleTraverser.Skip = SimpleTraverserSkip;