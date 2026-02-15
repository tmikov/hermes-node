// Minimal blob shim — Blob is not supported yet.
'use strict';

function createBlobFromFilePath() {
  throw new Error('Blob is not supported in hermes-node');
}

module.exports = {
  Blob: undefined,
  createBlobFromFilePath,
  resolveObjectURL: function() { return undefined; },
  isBlob: function() { return false; },
};
