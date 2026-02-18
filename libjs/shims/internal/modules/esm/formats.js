// Copyright (c) Tzvetan Mikov.
'use strict';

const {
  RegExpPrototypeExec,
} = primordials;

const extensionFormatMap = {
  '__proto__': null,
  '.cjs': 'commonjs',
  '.js': 'module',
  '.json': 'json',
  '.mjs': 'module',
  '.wasm': 'wasm',
};

function mimeToFormat(mime) {
  if (
    RegExpPrototypeExec(
      /^\s*(text|application)\/javascript\s*(;\s*charset=utf-?8\s*)?$/i,
      mime,
    ) !== null
  ) { return 'module'; }
  if (mime === 'application/json') { return 'json'; }
  if (mime === 'application/wasm') { return 'wasm'; }
  return null;
}

function getFormatOfExtensionlessFile(url) {
  return 'module';
}

module.exports = {
  extensionFormatMap,
  getFormatOfExtensionlessFile,
  mimeToFormat,
};
