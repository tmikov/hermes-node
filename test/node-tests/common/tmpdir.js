// Copyright (c) Tzvetan Mikov.
// Minimal shim of Node.js test/common/tmpdir.js for hermes-node.
'use strict';

var fs = require('fs');
var path = require('path');

var testRoot = path.resolve(__dirname, '..');

var tmpdirName = '.tmp.' + (process.env.TEST_THREAD_ID || '0');
var tmpPath = path.join(testRoot, tmpdirName);

function rmSyncRecursive(pathname) {
  try {
    fs.rmSync(pathname, { recursive: true, force: true });
  } catch (e) {
    // Ignore errors during cleanup.
  }
}

var firstRefresh = true;
function refresh() {
  rmSyncRecursive(tmpPath);
  fs.mkdirSync(tmpPath, { recursive: true });

  if (firstRefresh) {
    firstRefresh = false;
    process.on('exit', function() {
      try {
        process.chdir(testRoot);
      } catch (e) { /* ignore */ }
      rmSyncRecursive(tmpPath);
    });
  }
}

function resolve() {
  var args = [tmpPath];
  for (var i = 0; i < arguments.length; i++) {
    args.push(arguments[i]);
  }
  return path.resolve.apply(path, args);
}

function hasEnoughSpace(size) {
  try {
    var s = fs.statfsSync(tmpPath);
    return s.bavail >= Math.ceil(size / s.bsize);
  } catch (e) {
    return true;
  }
}

function fileURL() {
  var url = require('url');
  return url.pathToFileURL(tmpPath);
}

module.exports = {
  refresh: refresh,
  resolve: resolve,
  hasEnoughSpace: hasEnoughSpace,
  fileURL: fileURL,

  get path() {
    return tmpPath;
  },
  set path(newPath) {
    tmpPath = path.resolve(newPath);
  },
};
