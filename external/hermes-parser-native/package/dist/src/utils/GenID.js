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