'use strict';

Object.defineProperty(exports, "__esModule", {
  value: true
});
exports.SimpleTransform = void 0;

var _SimpleTraverser = require("../traverse/SimpleTraverser");

var _astNodeMutationHelpers = require("./astNodeMutationHelpers");

function setParentPointer(node, parent) {
  if (parent != null) {
    if (Array.isArray(node)) {
      for (const item of node) {
        item.parent = parent;
      }
    } else {
      node.parent = parent;
    }
  }
}

class SimpleTransform {
  transform(rootNode, options) {
    let resultRootNode = rootNode;

    _SimpleTraverser.SimpleTraverser.traverse(rootNode, {
      enter: (node, parent) => {
        setParentPointer(node, parent);
        const resultNode = options.transform(node);

        if (resultNode !== node) {
          let traversedResultNode = null;

          if (resultNode != null) {
            setParentPointer(resultNode, parent);

            if (Array.isArray(resultNode)) {
              traversedResultNode = resultNode.map(item => this.transform(item, options)).filter(item => item != null);
            } else {
              traversedResultNode = this.transform(resultNode, options);
            }
          }

          if (parent == null) {
            if (node !== rootNode) {
              throw new Error('SimpleTransform infra error: Parent not set on non root node, this should not be possible');
            }

            if (Array.isArray(traversedResultNode)) {
              throw new Error('SimpleTransform: invalid array result for root node');
            } else {
              resultRootNode = traversedResultNode;
            }
          } else if (traversedResultNode == null) {
            (0, _astNodeMutationHelpers.removeNodeOnParent)(node, parent, options.visitorKeys);
          } else {
            (0, _astNodeMutationHelpers.replaceNodeOnParent)(node, parent, traversedResultNode, options.visitorKeys);
            setParentPointer(traversedResultNode, parent);
          }

          throw _SimpleTraverser.SimpleTraverser.Skip;
        }
      },

      leave(_node) {},

      visitorKeys: options.visitorKeys
    });

    return resultRootNode;
  }

  static transform(node, options) {
    return new SimpleTransform().transform(node, options);
  }

  static transformProgram(program, options) {
    const result = SimpleTransform.transform(program, options);

    if ((result == null ? void 0 : result.type) === 'Program') {
      return result;
    }

    throw new Error('SimpleTransform.transformProgram: Expected program node.');
  }

  static nodeWith(node, overrideProps, visitorKeys) {
    return (0, _astNodeMutationHelpers.nodeWith)(node, overrideProps, visitorKeys);
  }

}

exports.SimpleTransform = SimpleTransform;