// Copyright (c) Tzvetan Mikov.
// Shim for internal/v8/startup_snapshot.
// Hermes doesn't support V8 snapshots. All functions are stubs.
'use strict';

function isBuildingSnapshot() {
  return false;
}

function throwIfNotBuildingSnapshot() {
  throw new Error('Not building snapshot');
}

function throwIfBuildingSnapshot(reason) {
  // No-op: never building a snapshot.
}

function noop() {}

module.exports = {
  runDeserializeCallbacks: noop,
  throwIfBuildingSnapshot,
  namespace: {
    addDeserializeCallback: throwIfNotBuildingSnapshot,
    addSerializeCallback: throwIfNotBuildingSnapshot,
    setDeserializeMainFunction: throwIfNotBuildingSnapshot,
    isBuildingSnapshot,
  },
  addAfterUserSerializeCallback: noop,
};
