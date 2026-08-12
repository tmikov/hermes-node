'use strict';

Object.defineProperty(exports, "__esModule", {
  value: true
});
exports.deepCloneNode = deepCloneNode;
exports.nodeWith = nodeWith;
exports.removeNodeOnParent = removeNodeOnParent;
exports.replaceNodeOnParent = replaceNodeOnParent;
exports.setParentPointersInDirectChildren = setParentPointersInDirectChildren;
exports.shallowCloneNode = shallowCloneNode;
exports.updateAllParentPointers = updateAllParentPointers;

var _astArrayMutationHelpers = require("./astArrayMutationHelpers");

var _getVisitorKeys = require("../traverse/getVisitorKeys");

var _SimpleTraverser = require("../traverse/SimpleTraverser");

function getParentKey(target, parent, visitorKeys) {
  if (parent == null) {
    throw new Error(`Expected parent node to be set on "${target.type}"`);
  }

  for (const key of (0, _getVisitorKeys.getVisitorKeys)(parent, visitorKeys)) {
    if ((0, _getVisitorKeys.isNode)(parent[key])) {
      if (parent[key] === target) {
        return {
          type: 'single',
          node: parent,
          key
        };
      }
    } else if (Array.isArray(parent[key])) {
      for (let i = 0; i < parent[key].length; i += 1) {
        const current = parent[key][i];

        if (current === target) {
          return {
            type: 'array',
            node: parent,
            key,
            targetIndex: i
          };
        }
      }
    }
  }

  throw new Error(`Expected to find the ${target.type} as a direct child of the ${parent.type}.`);
}

function replaceNodeOnParent(originalNode, originalNodeParent, nodeToReplaceWith, visitorKeys) {
  const replacementParent = getParentKey(originalNode, originalNodeParent, visitorKeys);
  const parent = replacementParent.node;

  if (replacementParent.type === 'array') {
    parent[replacementParent.key] = (0, _astArrayMutationHelpers.replaceInArray)(parent[replacementParent.key], replacementParent.targetIndex, Array.isArray(nodeToReplaceWith) ? nodeToReplaceWith : [nodeToReplaceWith]);
  } else {
    if (Array.isArray(nodeToReplaceWith)) {
      throw new Error(`Cannot insert array into non-array parent type: ${parent.type}`);
    }

    parent[replacementParent.key] = nodeToReplaceWith;
  }
}

function removeNodeOnParent(originalNode, originalNodeParent, visitorKeys) {
  const replacementParent = getParentKey(originalNode, originalNodeParent, visitorKeys);
  const parent = replacementParent.node;

  if (replacementParent.type === 'array') {
    parent[replacementParent.key] = (0, _astArrayMutationHelpers.removeFromArray)(parent[replacementParent.key], replacementParent.targetIndex);
  } else {
    parent[replacementParent.key] = null;
  }
}

function setParentPointersInDirectChildren(node, visitorKeys) {
  for (const key of (0, _getVisitorKeys.getVisitorKeys)(node, visitorKeys)) {
    if ((0, _getVisitorKeys.isNode)(node[key])) {
      node[key].parent = node;
    } else if (Array.isArray(node[key])) {
      for (const child of node[key]) {
        child.parent = node;
      }
    }
  }
}

function updateAllParentPointers(node, visitorKeys) {
  _SimpleTraverser.SimpleTraverser.traverse(node, {
    enter(node, parent) {
      node.parent = parent;
    },

    leave() {},

    visitorKeys
  });
}

function nodeWith(node, overrideProps, visitorKeys) {
  const willBeUnchanged = Object.entries(overrideProps).every(([key, value]) => {
    const node_ = node;

    if (Array.isArray(value)) {
      return Array.isArray(node_[key]) ? (0, _astArrayMutationHelpers.arrayIsEqual)(node_[key], value) : false;
    }

    return node_[key] === value;
  });

  if (willBeUnchanged) {
    return node;
  }

  const newNode = { ...node,
    ...overrideProps
  };
  setParentPointersInDirectChildren(newNode, visitorKeys);
  return newNode;
}

function shallowCloneNode(node, visitorKeys) {
  const newNode = { ...node
  };
  setParentPointersInDirectChildren(newNode, visitorKeys);
  return newNode;
}

function deepCloneNode(node, visitorKeys) {
  const clone = JSON.parse(JSON.stringify(node, (key, value) => {
    if (key === 'parent') {
      return undefined;
    }

    return value;
  }));
  updateAllParentPointers(clone, visitorKeys);
  return clone;
}