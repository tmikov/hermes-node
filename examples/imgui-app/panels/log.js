// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// Log: the application's own events, newest at the bottom, kept in a ring
// of the last LIMIT entries. It follows the bottom unless the user has
// scrolled up; a Clear button and a filter field above it.

'use strict';

const {oneLine} = require('../display-text.js');

const LIMIT = 1000;

const entries = new Array(LIMIT);
let first = 0; // index of the oldest entry
let count = 0;
const filter = [''];
let filtered = null; // the entries the filter keeps, or null when stale
let filteredFor = null;

function stamp(date) {
  const two = (n) => String(n).padStart(2, '0');
  return two(date.getHours()) + ':' + two(date.getMinutes()) + ':' +
      two(date.getSeconds());
}

/**
 * Append one entry, dropping the oldest when the ring is full. The text is
 * made one line (display-text.js): the list clipper below gives every
 * entry one line's height, and seeks to an entry by multiplying its index
 * by that height (imgui.cpp:3554), so a message holding a line break (a
 * path, which may) would put every later entry in the wrong place.
 */
function add(text) {
  const line = stamp(new Date()) + '  ' + oneLine(text);
  if (count < LIMIT) {
    entries[(first + count) % LIMIT] = line;
    ++count;
  } else {
    entries[first] = line;
    first = (first + 1) % LIMIT;
  }
  filtered = null;
}

function at(i) {
  return entries[(first + i) % LIMIT];
}

function clear() {
  first = 0;
  count = 0;
  filtered = null;
}

/** The entries the filter keeps, recomputed only when either changed. */
function visible() {
  if (filter[0] === '')
    return null;
  if (filtered === null || filteredFor !== filter[0]) {
    filtered = [];
    for (let i = 0; i < count; ++i) {
      if (at(i).includes(filter[0]))
        filtered.push(at(i));
    }
    filteredFor = filter[0];
  }
  return filtered;
}

module.exports = {
  name: 'Log',
  open: [true],
  add,
  clear,
  get count() {
    return count;
  },

  draw(ctx) {
    const {imgui} = ctx;
    if (imgui.button('Clear'))
      clear();
    imgui.sameLine();
    imgui.setNextItemWidth(-imgui.FLT_MIN);
    imgui.inputTextWithHint('##filter', 'filter', filter, 256);
    if (imgui.beginChild('entries', [0, 0], imgui.ChildFlags_Borders,
            imgui.WindowFlags_HorizontalScrollbar)) {
      const shown = visible();
      const n = shown === null ? count : shown.length;
      imgui.listClipper(n, imgui.getTextLineHeightWithSpacing(),
          (start, end) => {
            for (let i = start; i < end; ++i) {
              imgui.text(shown === null ? at(i) : shown[i]);
              ctx.state.recordRect('log:' + i);
            }
          });
      // Follow the bottom only while the view is already there (as of the
      // last frame), so a user who scrolled up to read is not pulled back
      // down. ImGui's demo log does the same.
      if (imgui.getScrollY() >= imgui.getScrollMaxY())
        imgui.setScrollHereY(1);
    }
    imgui.endChild();
  },
};
