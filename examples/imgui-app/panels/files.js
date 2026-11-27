// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// Files: a lazily loaded directory tree. A directory's entries are
// requested with fs.promises.readdir the first time its node is open, and
// it shows "loading" until the promise settles: the frame callback only
// draws the state it finds, and the continuation stores the result. A
// click on a file asks the viewer to load it.
//
// A node is {name, path, kind, children}, kind 'dir', 'file' or 'link',
// children null (not requested), 'loading', an array of nodes, or
// {error}. Symbolic links are shown and never followed into.

'use strict';

const path = require('path');

/** At most this many entries of one directory are shown. */
const ENTRY_LIMIT = 5000;

let root = null;
let selected = null;

function makeNode(name, nodePath, kind) {
  return {name, path: nodePath, kind, children: null, more: 0};
}

function kindOf(dirent) {
  if (dirent.isSymbolicLink())
    return 'link';
  return dirent.isDirectory() ? 'dir' : 'file';
}

/** Directories first, then by name (code unit order, as `ls` in C). */
function compareNodes(a, b) {
  if ((a.kind === 'dir') !== (b.kind === 'dir'))
    return a.kind === 'dir' ? -1 : 1;
  return a.name < b.name ? -1 : a.name > b.name ? 1 : 0;
}

function requestChildren(ctx, node) {
  const {state} = ctx;
  node.children = 'loading';
  state.track(state.fs.readdir(node.path, {withFileTypes: true}).then(
      (dirents) => {
        const all = dirents.map(
            (d) => makeNode(d.name, path.join(node.path, d.name), kindOf(d)));
        all.sort(compareNodes);
        node.children = all.slice(0, ENTRY_LIMIT);
        node.more = all.length - node.children.length;
        if (ctx.testMode)
          console.log('APP dir ' + node.path + ' entries=' + all.length);
      },
      (e) => {
        const code = (e && e.code) || 'EUNKNOWN';
        node.children = {error: code};
        ctx.log('cannot list ' + node.path + ': ' + code);
        if (ctx.testMode)
          console.log('APP error ' + node.path + ' ' + code);
      }));
}

function openFile(ctx, node) {
  const {state} = ctx;
  const viewer = state.viewer;
  selected = node.path;
  state.track(viewer.load(node.path).then(() => {
    const s = viewer.state;
    if (!ctx.testMode || s.path !== node.path)
      return;
    if (s.status === 'error')
      console.log('APP error ' + s.path + ' ' + s.code);
    else if (s.status === 'text')
      console.log(
          'APP opened ' + s.path + ' ' + s.size + ' lines=' + s.lines.length);
    else
      console.log('APP opened ' + s.path + ' ' + s.size + ' hex=' +
          s.rows.length);
  }));
}

/** Remember the row just drawn, without allocating once it is known. */
function recordRect(ctx, key) {
  const {imgui, state} = ctx;
  let r = state.rects.get(key);
  if (r === undefined) {
    r = {min: [0, 0], max: [0, 0]};
    state.rects.set(key, r);
  }
  imgui.getItemRectMin(r.min);
  imgui.getItemRectMax(r.max);
}

function drawChildren(ctx, node) {
  const {imgui} = ctx;
  const children = node.children;
  if (children === null)
    requestChildren(ctx, node);
  if (children === null || children === 'loading') {
    imgui.textDisabled('loading...');
  } else if (Array.isArray(children)) {
    if (children.length === 0)
      imgui.textDisabled('(empty)');
    for (const child of children)
      drawNode(ctx, child);
    if (node.more > 0)
      imgui.textDisabled('... ' + node.more + ' more');
  } else {
    imgui.textColored([1, 0.4, 0.4, 1], 'cannot list: ' + children.error);
  }
}

function drawNode(ctx, node, isRoot) {
  const {imgui} = ctx;
  imgui.pushID(node.path);
  if (node.kind === 'dir') {
    if (isRoot)
      imgui.setNextItemOpen(true, imgui.Cond_Once);
    const open = imgui.treeNodeEx(isRoot ? node.path : node.name + '/',
        imgui.TreeNodeFlags_SpanAvailWidth);
    recordRect(ctx, node.path);
    if (open) {
      drawChildren(ctx, node);
      imgui.treePop();
    }
  } else {
    const flags = imgui.TreeNodeFlags_Leaf |
        imgui.TreeNodeFlags_NoTreePushOnOpen |
        imgui.TreeNodeFlags_SpanAvailWidth |
        (selected === node.path ? imgui.TreeNodeFlags_Selected : 0);
    imgui.treeNodeEx(
        node.kind === 'link' ? node.name + ' (link)' : node.name, flags);
    recordRect(ctx, node.path);
    if (imgui.isItemClicked())
      openFile(ctx, node);
  }
  imgui.popID();
}

module.exports = {
  name: 'Files',
  open: [true],

  /** Start over at `dir`: nothing is read until the next frame draws it. */
  setRoot(dir) {
    root = makeNode(dir, dir, 'dir');
    selected = null;
  },

  get root() {
    return root;
  },

  draw(ctx) {
    if (root === null)
      module.exports.setRoot(ctx.state.root);
    drawNode(ctx, root, true);
  },
};
