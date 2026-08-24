// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// The whole example. tetris-cli is a third-party package that starts the
// game when its module body runs, so requiring it is all there is to do --
// which is the point: nothing here is written for hermes-node, and the same
// file runs under node.
require('tetris-cli/tetris.js');
