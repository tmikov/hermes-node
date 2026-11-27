// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// Viewer: the selected file's first 1 MiB, as text or as a hex dump, with
// a table of its stat fields above it.
//
// The file is read with bounded FileHandle.read() calls at explicit
// positions (never readFile, which reads the whole file whatever is
// shown), and the handle is closed in a `finally`. Each load() takes a
// request number; a load that finishes after a newer one started stores
// nothing, so the state always describes the last file asked for.
//
// This module requires neither hermes-node:ui nor hermes-node:imgui: it
// reaches ImGui only through the `ctx` draw() is given, and the
// filesystem only through the `fs` it was created with (`open`, and on the
// handle `stat`, `read` and `close`). test/imgui-app-viewer.js loads it
// under plain hermes-node.

'use strict';

const {isUtf8} = require('buffer');

/** How much of a file is read. */
const CAP = 1 << 20;
/** The size of one read() request. */
const CHUNK = 64 * 1024;
/** Lines longer than this many UTF-16 code units are cut for display. */
const LINE_LIMIT = 4096;
/** What a cut line ends with. */
const CUT_MARKER = ' [... line cut at ' + LINE_LIMIT + ' characters]';
/** Bytes per hex dump row. */
const HEX_ROW = 16;

/**
 * The length of `bytes` without a trailing incomplete UTF-8 sequence: up
 * to three continuation-or-lead bytes at the end that a cut at the cap
 * left without the rest of their character.
 */
function completeLength(bytes) {
  const end = bytes.length;
  for (let k = 1; k <= 3 && k <= end; ++k) {
    const b = bytes[end - k];
    if ((b & 0xc0) === 0x80)
      continue; // a continuation byte: look further back for its lead
    if (b >= 0xc0) {
      const need = b >= 0xf0 ? 4 : b >= 0xe0 ? 3 : 2;
      if (k < need)
        return end - k;
    }
    return end;
  }
  return end;
}

function cutLine(line) {
  if (line.length <= LINE_LIMIT)
    return line;
  let n = LINE_LIMIT;
  // Never split a surrogate pair.
  const c = line.charCodeAt(n - 1);
  if (c >= 0xd800 && c <= 0xdbff)
    --n;
  return line.slice(0, n) + CUT_MARKER;
}

function toLines(text) {
  if (text.length === 0)
    return [];
  const lines = text.split(/\r?\n/);
  if (lines[lines.length - 1] === '')
    lines.pop();
  return lines.map(cutLine);
}

function hex2(b) {
  return (b < 16 ? '0' : '') + b.toString(16);
}

function toHexRows(bytes) {
  const rows = [];
  for (let off = 0; off < bytes.length; off += HEX_ROW) {
    const n = Math.min(HEX_ROW, bytes.length - off);
    let hex = '';
    let ascii = '';
    for (let i = 0; i < HEX_ROW; ++i) {
      if (i < n) {
        const b = bytes[off + i];
        hex += hex2(b) + ' ';
        ascii += b >= 0x20 && b < 0x7f ? String.fromCharCode(b) : '.';
      } else {
        hex += '   ';
      }
      if (i === 7)
        hex += ' ';
    }
    rows.push(off.toString(16).padStart(8, '0') + '  ' + hex + ' ' + ascii);
  }
  return rows;
}

/**
 * Read at most CAP bytes of `path` through `fs`. Resolves with
 * `{bytes, stat, truncated}`; rejects with the filesystem's error. The
 * handle is closed whether or not the reads succeed.
 */
async function readCapped(fs, path) {
  const fh = await fs.open(path, 'r');
  try {
    const stat = await fh.stat();
    const chunks = [];
    let position = 0;
    while (position < CAP) {
      const length = Math.min(CHUNK, CAP - position);
      const buffer = Buffer.alloc(length);
      const {bytesRead} = await fh.read(buffer, 0, length, position);
      if (bytesRead === 0)
        break;
      chunks.push(bytesRead === length ? buffer :
                                         buffer.subarray(0, bytesRead));
      position += bytesRead;
    }
    const bytes = Buffer.concat(chunks, position);
    return {bytes, stat, truncated: position === CAP && stat.size > CAP};
  } finally {
    await fh.close();
  }
}

/** What a finished read is shown as. */
function classify(path, {bytes, stat, truncated}) {
  const kept = truncated ? bytes.subarray(0, completeLength(bytes)) : bytes;
  const base = {path, stat, size: stat.size, truncated};
  if (kept.indexOf(0) < 0 && isUtf8(kept))
    return Object.assign(
        base, {status: 'text', lines: toLines(kept.toString())});
  return Object.assign(base, {status: 'hex', rows: toHexRows(bytes)});
}

/** The stat fields the table above the content shows. */
function statRows(stat) {
  if (!stat)
    return [];
  const rows = [];
  for (const key of ['size', 'mode', 'uid', 'gid', 'nlink', 'ino', 'dev',
                     'blksize', 'blocks']) {
    if (typeof stat[key] === 'number')
      rows.push([key, key === 'mode' ? '0o' + stat[key].toString(8) :
                                       String(stat[key])]);
  }
  for (const key of ['mtime', 'atime', 'ctime', 'birthtime']) {
    if (stat[key] instanceof Date)
      rows.push([key, stat[key].toISOString()]);
  }
  return rows;
}

function createViewer({fs, log}) {
  let requests = 0;
  const viewer = {
    name: 'Viewer',
    open: [true],
    state: {status: 'empty', path: null, truncated: false},
    /** The stat table's rows for the current state, built once per load. */
    statRows: [],

    /**
     * Load `path`. Resolves once this request has settled, whether it was
     * stored or superseded by a newer one; never rejects.
     */
    async load(path) {
      const request = ++requests;
      viewer.state = {status: 'loading', path, truncated: false};
      viewer.statRows = [];
      let next;
      try {
        next = classify(path, await readCapped(fs, path));
      } catch (e) {
        const code = (e && e.code) || 'EUNKNOWN';
        next = {status: 'error', path, code, message: String(e && e.message),
                truncated: false};
      }
      if (request !== requests)
        return;
      viewer.state = next;
      viewer.statRows = statRows(next.stat);
      if (next.status === 'error')
        log('read error ' + path + ' ' + next.code);
      else
        log('opened ' + path + ' (' + next.size + ' bytes' +
            (next.truncated ? ', first 1 MiB shown' : '') + ')');
    },

    draw(ctx) {
      const {imgui} = ctx;
      const s = viewer.state;
      if (s.status === 'empty') {
        imgui.textDisabled('Select a file in Files.');
        return;
      }
      imgui.text(s.path);
      if (s.status === 'loading') {
        imgui.textDisabled('loading...');
        return;
      }
      if (s.status === 'error') {
        imgui.textColored([1, 0.4, 0.4, 1], 'Cannot read: ' + s.code);
        imgui.textWrapped(s.message);
        return;
      }
      // Two fields per row, so the table leaves room for the content.
      if (imgui.beginTable('stat', 4,
              imgui.TableFlags_Borders | imgui.TableFlags_RowBg |
                  imgui.TableFlags_SizingFixedFit)) {
        viewer.statRows.forEach(([key, value], i) => {
          if (i % 2 === 0)
            imgui.tableNextRow();
          imgui.tableNextColumn();
          imgui.textDisabled(key);
          imgui.tableNextColumn();
          imgui.text(value);
        });
        imgui.endTable();
      }
      if (s.truncated)
        imgui.textColored([1, 0.8, 0.3, 1],
            'Truncated: showing the first 1 MiB of ' + s.size + ' bytes.');
      const items = s.status === 'text' ? s.lines : s.rows;
      imgui.textDisabled(s.status === 'text' ?
          items.length + ' lines' : 'not UTF-8 text: hex dump, ' +
              items.length + ' rows');
      if (imgui.beginChild('content', [0, 0], imgui.ChildFlags_Borders,
              imgui.WindowFlags_HorizontalScrollbar)) {
        imgui.listClipper(items.length,
            imgui.getTextLineHeightWithSpacing(), (start, end) => {
              for (let i = start; i < end; ++i)
                imgui.text(items[i]);
            });
      }
      imgui.endChild();
    },
  };
  return viewer;
}

module.exports = {createViewer, CAP, LINE_LIMIT, CUT_MARKER};
