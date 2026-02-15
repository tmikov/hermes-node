// Shim for internal/bootstrap/realm.js
// Provides a minimal BuiltinModule class for module inspection.

'use strict';

var BuiltinModule = class BuiltinModule {
  static exists(id) {
    return false;
  }

  static canBeRequiredByUsers(id) {
    return false;
  }

  static canBeRequiredWithoutScheme(id) {
    return false;
  }

  static isBuiltin(id) {
    return false;
  }
};

BuiltinModule.map = new Map();

module.exports = { BuiltinModule };
