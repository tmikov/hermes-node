// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// Text the application draws from outside sources (file names, paths in
// log messages), made safe to lay out as one line.

'use strict';

/**
 * Control characters, replaced by '?' for display: ImGui lays text out as
 * it is, so a newline in a file name would make a row two lines tall, and
 * a list clipper's uniform row height (the Files tree's, the Log's) would
 * be wrong for every row below it.
 */
const CONTROL_CHARS = /[\u0000-\u001f\u007f-\u009f]/g;

/** `text` with every control character replaced by '?'. */
function oneLine(text) {
  return text.replace(CONTROL_CHARS, '?');
}

module.exports = {oneLine};
