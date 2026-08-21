// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// Deliberately no require() of the addon here: a require() of a kNative
// module takes the loader's native branch and dlopens the sidecar, so it
// never reaches __bundleLoad() and could not exercise the refusal this test
// is about. The refusal has to be reached directly, which is what the call
// below does. The addon is packaged instead with --include on the build
// line, which walks it into the container the same way a real require()
// specifier would, without ever calling require() on it.

// Reach the native directly, bypassing the loader, and confirm it refuses.
const natives = globalThis.__bundleNatives();
if (natives.length !== 1) throw new Error('natives: ' + natives.length);
if (natives[0].sidecar !== 'hello_addon.node') {
  throw new Error('sidecar: ' + natives[0].sidecar);
}
let threw = false;
try {
  globalThis.__bundleLoad(natives[0].identity);
} catch (e) {
  threw = true;
}
if (!threw) throw new Error('__bundleLoad did not refuse a native');
console.log('PASS refused');
