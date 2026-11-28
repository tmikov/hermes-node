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
// filesystem only through the `fs` it was created with (`stat` and `open`,
// and on the handle `stat`, `read` and `close`). Only a regular file is
// opened: anything else (a FIFO, a device, a socket, a directory, or a
// symbolic link to one) is the error NOT_A_REGULAR_FILE, decided by a stat
// of the path first. test/imgui-app-viewer.js loads it
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
 * Whether `second` may follow the lead byte `lead` in UTF-8: the ranges
 * that exclude overlong forms (after 0xE0 and 0xF0), surrogates (after
 * 0xED) and code points past U+10FFFF (after 0xF4); 0x80..0xBF otherwise
 * (RFC 3629, section 4).
 */
function secondByteOk(lead, second) {
  const [lo, hi] = lead === 0xe0 ? [0xa0, 0xbf] :
      lead === 0xed ? [0x80, 0x9f] :
      lead === 0xf0 ? [0x90, 0xbf] :
      lead === 0xf4 ? [0x80, 0x8f] :
                      [0x80, 0xbf];
  return second >= lo && second <= hi;
}

/**
 * The length of `bytes` without a trailing incomplete UTF-8 sequence: a
 * suffix that is a proper prefix of a valid encoding, a lead byte at most
 * three bytes from the end followed by fewer continuation bytes than its
 * sequence needs, the first of them in the range its lead allows. Only
 * real lead bytes count (0xC2..0xF4), and a suffix no following byte could
 * complete (0xE0 0x80, 0xED 0xA0, 0xF4 0x90, ...) is kept: anything else
 * is left for isUtf8() to reject, so a cut file ending in an invalid
 * sequence is a hex dump, not text.
 */
function completeLength(bytes) {
  const end = bytes.length;
  for (let k = 1; k <= 3 && k <= end; ++k) {
    const b = bytes[end - k];
    if ((b & 0xc0) === 0x80)
      continue; // a continuation byte: look further back for its lead
    if (b >= 0xc2 && b <= 0xf4) {
      const need = b >= 0xf0 ? 4 : b >= 0xe0 ? 3 : 2;
      if (k < need && (k === 1 || secondByteOk(b, bytes[end - k + 1])))
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

/** What a stat says `path` is, for a message, when it is not a file. */
function kindOf(stat) {
  if (stat.isFIFO())
    return 'a FIFO';
  if (stat.isCharacterDevice())
    return 'a character device';
  if (stat.isSocket())
    return 'a socket';
  if (stat.isBlockDevice())
    return 'a block device';
  if (stat.isDirectory())
    return 'a directory';
  return 'an unknown kind of file';
}

/**
 * Rejects unless `path` is a regular file, by a stat of the path, which
 * follows a symbolic link and cannot block. Only then is it opened: open()
 * of a FIFO with no writer, or a read of a terminal, would block a libuv
 * threadpool thread for good, and four of those (the pool's default size)
 * stop every later listing and read. A path replaced by a FIFO between
 * the stat and the open is not caught.
 */
async function checkRegular(fs, path) {
  const stat = await fs.stat(path);
  if (stat.isFile())
    return;
  const e = new Error('not a regular file: ' + kindOf(stat));
  e.code = 'NOT_A_REGULAR_FILE';
  throw e;
}

/**
 * Read at most CAP bytes of `path` through `fs`. Resolves with
 * `{bytes, stat, truncated}`; rejects with the filesystem's error. The
 * handle is closed whether or not the reads succeed.
 *
 * Whether the file goes on past the cap is decided by reading, not by
 * stat.size: procfs files and devices report 0, and a file being appended
 * to grows after the stat. Once CAP bytes are in, one more read of one
 * byte at position CAP says whether there is more.
 */
async function readCapped(fs, path) {
  await checkRegular(fs, path);
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
    let truncated = false;
    if (position === CAP) {
      const probe = Buffer.alloc(1);
      truncated = (await fh.read(probe, 0, 1, CAP)).bytesRead > 0;
    }
    const bytes = Buffer.concat(chunks, position);
    return {bytes, stat, truncated};
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
            'Truncated: showing the first 1 MiB (the file is larger).');
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
