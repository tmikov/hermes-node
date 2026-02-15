// Copyright (c) Tzvetan Mikov.
// Minimal shim of Node.js test/common/fixtures.js for hermes-node.
'use strict';

var path = require('path');
var fs = require('fs');

var fixturesDir = path.join(__dirname, '..', 'fixtures');

function fixturesPath() {
  var args = [fixturesDir];
  for (var i = 0; i < arguments.length; i++) {
    args.push(arguments[i]);
  }
  return path.join.apply(path, args);
}

function readFixtureSync(args, enc) {
  if (Array.isArray(args))
    return fs.readFileSync(fixturesPath.apply(null, args), enc);
  return fs.readFileSync(fixturesPath(args), enc);
}

module.exports = {
  fixturesDir: fixturesDir,
  path: fixturesPath,
  readSync: readFixtureSync,
};
