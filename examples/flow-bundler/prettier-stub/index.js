// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.
//
// Stands in for prettier inside the artifacts build-bundle.sh produces, by
// way of a symlink in its staging node_modules. Never used by run.sh.
//
// The one importer is hermes-transform/dist/transform/print.js, which
// requires prettier when it loads -- hermes-transform's transform.js loads it
// unconditionally -- and touches it only inside print(), as prettier.format.
// The bundler never calls print(). It could not here even if it tried: the
// first thing print() does is require('prettier/plugins/estree'), a
// Prettier 3 path, while this example pins 2.8.8.
//
// Without this stub the producer packages all of prettier, because its
// index.js names every language parser in a literal require(): 7.0 MB of
// bytecode never loaded, and in a build-native artifact, several times that
// in machine code. See "Why prettier is stubbed" in README.md.

'use strict';

function format() {
  throw new Error(
    'prettier is stubbed out of the flow-bundler artifacts ' +
      '(examples/flow-bundler/prettier-stub); nothing in them should format code',
  );
}

module.exports = {format};
