// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// Files: a lazily loaded directory tree. A directory's entries are
// requested with fs.promises.readdir the first time it is open, and it
// shows "loading" until the promise settles: the frame callback only
// draws the state it finds, and the continuation stores the result. A
// click on a file asks the viewer to load it.
//
// A node is {name, path, kind, children, more, open}: kind 'dir', 'file'
// or 'link'; children null (not requested), 'loading', an array of nodes,
// or {error}; `more` the entries past ENTRY_LIMIT; `open` whether a
// directory is expanded. Symbolic links are shown and never followed into.
//
// The visible tree is flattened into one array of rows, rebuilt only when
// a directory opens, closes or finishes loading, and drawn through the
// list clipper: a frame costs the rows on screen, not the rows in the
// tree. That is why the open state is the node's own rather than ImGui's
// (a row scrolled out of view is not drawn, so ImGui would not know it),
// handed to ImGui with setNextItemOpen(..., Cond_Always) on every row.

'use strict';

const path = require('path');

/** At most this many entries of one directory are shown. */
const ENTRY_LIMIT = 5000;

let root = null;
let selected = null;
/** The flattened visible tree, or null when it must be rebuilt. */
let rows = null;

function makeNode(name, nodePath, kind) {
  return {name, path: nodePath, kind, children: null, more: 0, open: false};
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

/**
 * Rows: {type, node, depth}, type 'node' for the node itself, or
 * 'loading', 'empty', 'error' and 'more' for a status row under it.
 */
function flatten(node, depth, out) {
  out.push({type: 'node', node, depth});
  if (node.kind !== 'dir' || !node.open)
    return out;
  const children = node.children;
  if (children === null || children === 'loading') {
    out.push({type: 'loading', node, depth: depth + 1});
  } else if (Array.isArray(children)) {
    if (children.length === 0)
      out.push({type: 'empty', node, depth: depth + 1});
    for (const child of children)
      flatten(child, depth + 1, out);
    if (node.more > 0)
      out.push({type: 'more', node, depth: depth + 1});
  } else {
    out.push({type: 'error', node, depth: depth + 1});
  }
  return out;
}

function requestChildren(ctx, node) {
  const {state} = ctx;
  node.children = 'loading';
  rows = null;
  state.track(state.fs.readdir(node.path, {withFileTypes: true}).then(
      (dirents) => {
        const all = dirents.map(
            (d) => makeNode(d.name, path.join(node.path, d.name), kindOf(d)));
        all.sort(compareNodes);
        node.children = all.slice(0, ENTRY_LIMIT);
        node.more = all.length - node.children.length;
        rows = null;
        if (ctx.testMode)
          console.log('APP dir ' + node.path + ' entries=' + all.length);
      },
      (e) => {
        const code = (e && e.code) || 'EUNKNOWN';
        node.children = {error: code};
        rows = null;
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

/**
 * Control characters, replaced by '?' for display: ImGui lays text out as
 * it is, so a newline in a file name would make its row two lines tall,
 * and the list clipper's uniform row height would be wrong for every row
 * below it.
 */
const CONTROL_CHARS = /[\u0000-\u001f\u007f-\u009f]/g;

/**
 * One tree node row labelled `text`, recorded under `key`; returns
 * {result, clicked} (clicked only read when `wantClick`). The row's ID is
 * the pushID() around it, never the label. ImGui stops displaying a label
 * at its first "##" (FindRenderedTextEnd, imgui.cpp:4037-4046), so a name
 * containing one is drawn as a node with an empty label followed by the
 * name as plain text, which ImGui shows whole; the text's rectangle is
 * recorded under `label:<key>`.
 */
function treeRow(ctx, key, text, flags, wantClick) {
  const {imgui, state} = ctx;
  const label = text.replace(CONTROL_CHARS, '?');
  const asText = label.includes('##');
  const result = imgui.treeNodeEx(asText ? '##row' : label, flags);
  state.recordRect(key);
  const clicked = wantClick && imgui.isItemClicked();
  if (asText) {
    imgui.sameLine(0, 0);
    imgui.text(label);
    state.recordRect('label:' + key);
  }
  return {result, clicked};
}

function drawNodeRow(ctx, node, isRoot) {
  const {imgui} = ctx;
  imgui.pushID(node.path);
  if (node.kind === 'dir') {
    imgui.setNextItemOpen(node.open, imgui.Cond_Always);
    const open = treeRow(ctx, node.path, isRoot ? node.path : node.name + '/',
        imgui.TreeNodeFlags_SpanAvailWidth |
            imgui.TreeNodeFlags_NoTreePushOnOpen,
        false).result;
    if (open !== node.open) {
      node.open = open;
      rows = null;
    }
    if (node.open && node.children === null)
      requestChildren(ctx, node);
  } else {
    const flags = imgui.TreeNodeFlags_Leaf |
        imgui.TreeNodeFlags_NoTreePushOnOpen |
        imgui.TreeNodeFlags_SpanAvailWidth |
        (selected === node.path ? imgui.TreeNodeFlags_Selected : 0);
    const row = treeRow(ctx, node.path,
        node.kind === 'link' ? node.name + ' (link)' : node.name, flags, true);
    if (row.clicked)
      openFile(ctx, node);
  }
  imgui.popID();
}

function drawRow(ctx, row, indentWidth) {
  const {imgui} = ctx;
  const indent = row.depth * indentWidth;
  if (indent > 0)
    imgui.indent(indent);
  switch (row.type) {
    case 'node':
      drawNodeRow(ctx, row.node, row.node === root);
      break;
    case 'loading':
      imgui.textDisabled('loading...');
      break;
    case 'empty':
      imgui.textDisabled('(empty)');
      break;
    case 'more':
      imgui.textDisabled('... ' + row.node.more + ' more');
      ctx.state.recordRect('more:' + row.node.path);
      break;
    case 'error':
      imgui.textColored(
          [1, 0.4, 0.4, 1], 'cannot list: ' + row.node.children.error);
      break;
  }
  if (indent > 0)
    imgui.unindent(indent);
}

module.exports = {
  name: 'Files',
  open: [true],
  ENTRY_LIMIT,

  /** Start over at `dir`: nothing is read until the next frame draws it. */
  setRoot(dir) {
    root = makeNode(dir, dir, 'dir');
    root.open = true;
    selected = null;
    rows = null;
  },

  get root() {
    return root;
  },

  draw(ctx) {
    const {imgui} = ctx;
    if (root === null)
      module.exports.setRoot(ctx.state.root);
    if (rows === null)
      rows = flatten(root, 0, []);
    // Every row is one line of text (a tree node without a frame, or a
    // text item), so the clipper's uniform height is exact.
    const list = rows;
    const indentWidth = imgui.getTextLineHeight();
    imgui.listClipper(list.length, imgui.getTextLineHeightWithSpacing(),
        (start, end) => {
          for (let i = start; i < end; ++i)
            drawRow(ctx, list[i], indentWidth);
        });
  },
};
