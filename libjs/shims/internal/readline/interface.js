// Copyright (c) Tzvetan Mikov.
//
// Minimal shim for internal/readline/interface.
// The original has a deep dependency chain:
//   internal/readline/interface -> internal/repl/history -> os
//   -> internalBinding('credentials') (not implemented)
//
// fs/promises.js uses Interface only for FileHandle.readLines(),
// which is a niche feature. This stub throws on construction.

'use strict';

const {
  codes: { ERR_METHOD_NOT_IMPLEMENTED },
} = require('internal/errors');

function Interface() {
  throw new ERR_METHOD_NOT_IMPLEMENTED('readline.Interface');
}

module.exports = { Interface };
