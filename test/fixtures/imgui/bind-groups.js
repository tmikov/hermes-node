// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// The checked-in surface of `hermes-node:imgui`: the bound names by group,
// as the surface table (docs/superpowers/plans/2026-11-27-imgui-binding-
// surface.md) sections them, and a sample of constants with their values.
//
// test/imgui-bind-surface.js compares all() with the module's own property
// names, so an added or removed export fails there; each group's test ends
// with harness.checkGroupCoverage(h, '<group>'), which fails unless every
// function of the group has reached ImGui. Each task appends what it binds.
// A group listed here can be incomplete until the task that completes it:
// only then is its coverage checked.

'use strict';

const groups = {
  Frame: [
    'newFrame',
    'render',
    'showDemoWindow',
    'version', // a string property, not a function
    'showMetricsWindow',
    'getTime',
    'getFrameRate',
    'getDeltaTime',
    'getDisplaySize',
    'wantCaptureMouse',
    'wantCaptureKeyboard',
  ],
  // Incomplete until Task 3.
  Windows: [
    'begin',
    'end',
    'beginChild',
    'endChild',
    'setNextWindowPos',
    'setNextWindowSize',
  ],
  // Incomplete until Task 3.
  Layout: ['beginGroup', 'endGroup', 'setCursorScreenPos'],
  // Incomplete until Task 3.
  IDs: ['pushID', 'popID'],
  // Incomplete until Task 3.
  Text: ['text'],
  // Incomplete until Task 4.
  Widgets: ['button', 'checkbox', 'sliderFloat3'],
  // Incomplete until Task 3.
  Queries: ['getItemRectMin', 'getItemRectMax'],
  // Incomplete until Task 7.
  Style: ['pushStyleColor', 'popStyleColor', 'pushStyleVar', 'popStyleVar'],
};

// Exported constant -> its value, copied by hand from imgui.h (Task 2).
const constants = {};

/** Every exported name, sorted: the groups' names and the constants. */
function all() {
  const names = [];
  for (const g of Object.keys(groups))
    names.push(...groups[g]);
  names.push(...Object.keys(constants));
  return names.sort();
}

module.exports = {groups, constants, all};
