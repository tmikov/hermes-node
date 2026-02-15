// Copyright (c) Tzvetan Mikov.
//
// Minimal shim for internal/worker/js_transferable.
// The original depends on:
//   - internalBinding('messaging') (not implemented)
//   - internalBinding('symbols') (messaging_*_symbol)
//   - internal/webidl
//
// Worker threads are not supported. This shim provides the symbols used
// by fs/promises.js FileHandle class and a no-op markTransferMode.

'use strict';

const kDeserialize = Symbol('kDeserialize');
const kTransfer = Symbol('kTransfer');
const kTransferList = Symbol('kTransferList');

function markTransferMode() {}

module.exports = {
  kDeserialize,
  kTransfer,
  kTransferList,
  markTransferMode,
};
