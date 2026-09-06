/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 *
 *  strict
 * @format
 */
'use strict';

Object.defineProperty(exports, "__esModule", {
  value: true
});
exports.default = void 0;
const genPrefix = '$$gen$';

class GenID {
  constructor(uniqueTransformPrefix) {
    this.genN = 0;
    this.used = new Set();
    this.prefix = void 0;
    this.prefix = `${genPrefix}${uniqueTransformPrefix}`;
  }

  id() {
    let name;

    do {
      name = `${this.prefix}${this.genN}`;
      this.genN++;
    } while (this.used.has(name));

    this.used.add(name);
    return name;
  }

  addUsage(name) {
    if (name.startsWith(this.prefix)) {
      this.used.add(name);
    }
  }

}

exports.default = GenID;