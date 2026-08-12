// Copyright (c) Tzvetan Mikov.
// Preloaded via --require by test/test-cli-require.js.

'use strict';

globalThis.__preloadOrder = globalThis.__preloadOrder || [];
globalThis.__preloadOrder.push('set-global');

// A preloaded module can require its own relative dependencies.
globalThis.__preloadHelper = require('./helper.js').name;
