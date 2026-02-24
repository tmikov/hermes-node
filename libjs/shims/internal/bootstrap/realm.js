// Copyright (c) Tzvetan Mikov.
// Shim for internal/bootstrap/realm.js
// Provides a minimal BuiltinModule class for module inspection.

'use strict';

// Public built-in module names that can be required without the 'node:' prefix.
var builtinIds = [
  'assert', 'assert/strict', 'async_hooks', 'buffer', 'child_process',
  'cluster', 'console', 'constants', 'dgram', 'diagnostics_channel',
  'dns', 'dns/promises', 'domain', 'events', 'fs', 'fs/promises', 'http', 'https',
  'net', 'os', 'path', 'path/posix', 'path/win32', 'process',
  'querystring', 'readline', 'readline/promises', 'repl',
  'module',
  'crypto',
  'stream', 'stream/consumers', 'stream/promises', 'stream/web',
  'string_decoder', 'timers', 'timers/promises', 'tls', 'tty', 'url', 'util',
  'util/types', 'vm',
];

var builtinSet = new Set(builtinIds);

// Vendored package names (available via require('node:ws') or as fallback).
var vendoredIds = ['ws'];
var allRequirableIds = builtinIds.concat(vendoredIds);
var allRequirableSet = new Set(allRequirableIds);

// Capture bootstrap loader require for compileForPublicLoader.
var _bootstrapRequire = globalThis.require;

var BuiltinModule = class BuiltinModule {
  constructor(id) {
    this.id = id;
    this.loaded = false;
    this.loading = false;
    this.exports = {};
  }

  // Load the module via the bootstrap loader and cache its exports.
  compileForPublicLoader() {
    if (!this.loaded) {
      this.loading = true;
      this.exports = _bootstrapRequire(this.id);
      this.loaded = true;
      this.loading = false;
    }
    return this;
  }

  static exists(id) {
    return allRequirableSet.has(id);
  }

  static canBeRequiredByUsers(id) {
    return allRequirableSet.has(id);
  }

  // Only builtins can be required without 'node:' prefix (so bare
  // require('ws') goes through node_modules first, not short-circuiting).
  static canBeRequiredWithoutScheme(id) {
    return builtinSet.has(id);
  }

  static isBuiltin(id) {
    if (allRequirableSet.has(id)) return true;
    if (typeof id === 'string' && id.startsWith('node:')) {
      return allRequirableSet.has(id.slice(5));
    }
    return false;
  }

  // For 'node:' prefix: resolve against allRequirableSet (so node:ws works).
  // For bare names: resolve only against builtinSet (so require('ws')
  // doesn't short-circuit to builtin -- goes through node_modules first).
  static normalizeRequirableId(id) {
    if (typeof id === 'string' && id.startsWith('node:')) {
      var normalizedId = id.slice(5);
      if (allRequirableSet.has(normalizedId)) return normalizedId;
    } else if (builtinSet.has(id)) {
      return id;
    }
    return undefined;
  }

  static getSchemeOnlyModuleNames() {
    // Modules that can only be loaded via 'node:' prefix.
    // We don't support any of these (test, sea, sqlite, quic).
    return [];
  }

  static getAllBuiltinModuleIds() {
    return allRequirableIds.slice();
  }
};

BuiltinModule.map = new Map(
  allRequirableIds.map(function(id) { return [id, new BuiltinModule(id)]; })
);

module.exports = { BuiltinModule };
