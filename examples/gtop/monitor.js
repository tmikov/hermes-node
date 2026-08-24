// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// The whole example. Requiring gtop builds its dashboard -- the screen and
// the grid are created by the module body -- but the monitors that fill the
// panels only start when init() is called, which is what gtop's own
// bin/gtop does. Requiring without calling it draws every panel and leaves
// them all empty, which is a convincing-looking way to demo nothing.
require('gtop').init();
