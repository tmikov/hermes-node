'use strict';

Object.defineProperty(exports, "__esModule", {
  value: true
});
exports.default = mutate;

var _SimpleTransform = require("../transform/SimpleTransform");

function transformChainExpression(node, comments) {
  if (comments != null) {
    var _node$comments;

    const joinedComments = comments.concat((_node$comments = node.comments) != null ? _node$comments : []);
    node.comments = joinedComments;
  }

  switch (node.type) {
    case 'CallExpression':
      return { ...node,
        type: 'OptionalCallExpression',
        callee: transformChainExpression(node.callee)
      };

    case 'MemberExpression':
      return { ...node,
        type: 'OptionalMemberExpression',
        object: transformChainExpression(node.object)
      };
  }

  return node;
}

function mutate(rootNode, visitorKeys) {
  _SimpleTransform.SimpleTransform.transform(rootNode, {
    transform(node) {
      if (node.parent) {
        delete node.parent;
      }

      if (node.type === 'ChainExpression') {
        return transformChainExpression(node.expression, node == null ? void 0 : node.comments);
      }

      if (node.type === 'ObjectTypeProperty') {
        if (node.method === false && node.kind === 'init' && node.range[0] === 1 && node.value.range[0] === 1) {
          node.value = { ...node.value,
            range: [2, node.value.range[1]]
          };
        }

        return node;
      }

      if (node.type === 'ImportSpecifier') {
        if (node.local.name === node.imported.name) {
          if (node.local.range == null) {
            node.local.range = [0, 0];
          }

          node.imported.range = [...node.local.range];
        }

        return node;
      }

      if (node.type === 'ExportSpecifier') {
        if (node.local.name === node.exported.name) {
          if (node.local.range == null) {
            node.local.range = [0, 0];
          }

          node.exported.range = [...node.local.range];
        }

        return node;
      }

      return node;
    },

    visitorKeys
  });
}