// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// Deliberately does not require the addon itself: doing so would load it
// through this entry's own require() and mask whether --preload also did.
// Instead this looks in require.cache, which Module._cache -- the loader's
// only cache (see libjs/bundle-loader.js) -- is keyed by and published into
// before a required module's body runs. A preload that ran and dlopen'd
// the addon leaves an entry there before this file's body executes; a
// build with no --preload of the addon leaves it absent, so this prints
// 'true' or 'false' depending on which happened.
var loaded = Object.keys(require.cache).some(function(k) {
  return k.slice(-'hello_addon.node'.length) === 'hello_addon.node';
});
console.log('PRELOADED', loaded);
