# Fix Plan: Enable Async Generators in Hermes Runtime

## Problem

Hermes supports async generators (`async function*`), but they must be explicitly
enabled via `RuntimeConfig::Builder().withEnableAsyncGenerators(true)`. This was
not known during implementation. Several commits introduced unnecessary workarounds:
patching vendored Node.js files, creating empty shims, and declaring features as
permanently unavailable.

## Hermes Async Generator Support

- **CLI flag:** `-Xasync-generators` (hidden, off by default)
- **RuntimeConfig:** `.withEnableAsyncGenerators(true)` on the `RuntimeConfig::Builder`
- **Source:** `hermes/public/hermes/Public/RuntimeConfig.h` line 72-73
- **Runtime member:** `hasAsyncGenerators_`, set from `runtimeConfig.getEnableAsyncGenerators()`
- **Parser:** `SemanticResolver.cpp` checks `astContext_.getEnableAsyncGenerators()`;
  emits `"async generators are unsupported"` error when false
- **Tests:** `hermes/test/hermes/async-generators.js` demonstrates full functionality

## Affected Commits

| Commit | Step | What's Wrong |
|--------|------|-------------|
| `939bb5a` | Step 4 (primordials) | `AsyncIteratorPrototype` is a manual stub instead of being derived from `async function*` prototype chain |
| `0ef1fe4` | Step 8 (bootstrap) | Missing `.withEnableAsyncGenerators(true)` in RuntimeConfig — **root cause** |
| `dc1d66b` | Step 26 (streams) | Patched `readable.js`, `pipeline.js`; created empty `operators.js` shim |
| `489723c` | Step 29 (fs_dir) | Patched `internal/fs/dir.js` with manual async iterator |
| `0108960` | Step 32 (fs async verify) | Test declares `fs.promises` as "permanently unavailable" |

### NOT affected (keep as-is)

| File | Reason to Keep |
|------|---------------|
| `libjs/shims/internal/abort_controller.js` | Still needed despite the `FinalizationRegistry` polyfill (see Fix 2b below), because the original also depends on `internal/event_target` → `internalBinding('performance')`, `internal/webidl`, `internal/worker/js_transferable` — a chain we haven't ported. Comments should be corrected to not mention async generators. |

### FinalizationRegistry

Hermes does not currently have `FinalizationRegistry`, but it is being added and
will be available soon. In the meantime, a no-op polyfill is sufficient: all 3 uses
in core Node modules (`abort_controller`, `event_target`, `process/finalization`)
are leak-prevention cleanup — no correctness impact if callbacks never fire.

The polyfill goes in `primordials.js` so it is picked up by the existing conditional
code that creates `SafeFinalizationRegistry` (lines 253-255, 308-309).

---

## Fix Steps

**IMPORTANT:** Every fix commit message MUST include the target step commit hash
that it will be squashed into during the rebase. Use this format:

```
Fix N: <description>

Fixup for Step M (<commit-hash>).

<details>
```

This makes the rebase straightforward — the target is recorded in each commit.

### Fix 1: Enable async generators in runtime [amend Step 8, `0ef1fe4`]

**File:** `tools/hermes-node/hermes-node.cpp`

**Change:** Add `.withEnableAsyncGenerators(true)` to the `RuntimeConfig::Builder`:

```cpp
auto config = hermes::vm::RuntimeConfig::Builder()
                  .withMicrotaskQueue(true)
                  .withEnableAsyncGenerators(true)  // ← add this
                  .build();
```

**Verify:** Build succeeds. Existing tests pass (no test depends on async generators
being disabled).

```bash
cmake --build cmake-build-asan --target check-hermes-node
```

---

### Fix 2: Fix primordials — `AsyncIteratorPrototype` + `FinalizationRegistry` polyfill [amend Step 4, `939bb5a`]

**File:** `libjs/primordials.js`

#### 2a. Fix `AsyncIteratorPrototype`

**Current code (lines 390-392):**
```js
// AsyncIteratorPrototype — requires async generators which Hermes doesn't
// support yet. Provide a minimal stub object.
primordials.AsyncIteratorPrototype = { __proto__: null, [Symbol.asyncIterator]: function() { return this; } };
```

**Replace with** (matching Node's `lib/internal/per_context/primordials.js` line 456-458):
```js
primordials.AsyncIteratorPrototype =
  ReflectGetPrototypeOf(
    ReflectGetPrototypeOf(
      async function* () {}).prototype);
```

Note: `ReflectGetPrototypeOf` is already available at this point in primordials.js
as a local alias for `Reflect.getPrototypeOf`. Verify by checking the surrounding
code. If not, use `Reflect.getPrototypeOf` directly.

#### 2b. Add no-op `FinalizationRegistry` polyfill

Add the polyfill **before** the `intrinsicNames` array (before line 232), so that
the existing conditional code at lines 253-255 and 308-309 picks it up automatically:

```js
// ---------------------------------------------------------------------------
// FinalizationRegistry polyfill (no-op)
// ---------------------------------------------------------------------------
// Hermes does not yet have FinalizationRegistry. Provide a no-op polyfill so
// that SafeFinalizationRegistry is available in primordials. All Node.js uses
// (abort_controller, event_target, process/finalization) are leak-prevention
// cleanup — no correctness impact if callbacks never fire.
// This polyfill will be removed when Hermes adds native FinalizationRegistry.
if (typeof FinalizationRegistry === 'undefined') {
  globalThis.FinalizationRegistry = function FinalizationRegistry(callback) {};
  globalThis.FinalizationRegistry.prototype.register = function(target, heldValue, unregisterToken) {};
  globalThis.FinalizationRegistry.prototype.unregister = function(unregisterToken) {};
}
```

This is intentionally minimal — a no-op where `register()` and `unregister()`
do nothing and the callback is never called. The guard `typeof FinalizationRegistry
=== 'undefined'` ensures the polyfill is not installed when the real implementation
becomes available in Hermes.

After this, the existing code handles the rest:
- Line 253-255: `FinalizationRegistry` is added to `intrinsicNames`, so
  `FinalizationRegistryPrototype*` primordials are generated.
- Line 308-309: `SafeFinalizationRegistry` is set to `FinalizationRegistry`.

#### 2c. Update `test/primordials.js`

Update the `AsyncIteratorPrototype` test (around line 218-219) to verify the real
prototype chain instead of just checking it's an object:
```js
// ---- AsyncIteratorPrototype ----
assert(typeof p.AsyncIteratorPrototype === 'object', 'AsyncIteratorPrototype');
assert(typeof p.AsyncIteratorPrototype[Symbol.asyncIterator] === 'function',
       'AsyncIteratorPrototype has @@asyncIterator');
// Verify it's the real prototype, not a stub
assert(p.AsyncIteratorPrototype === Object.getPrototypeOf(
  Object.getPrototypeOf(async function* () {}).prototype),
  'AsyncIteratorPrototype is the real async iterator prototype');
```

Add a test for `FinalizationRegistry`:
```js
// ---- FinalizationRegistry (polyfill or native) ----
assert(typeof p.SafeFinalizationRegistry === 'function', 'SafeFinalizationRegistry exists');
var fr = new p.SafeFinalizationRegistry(function() {});
assert(typeof fr.register === 'function', 'register method exists');
assert(typeof fr.unregister === 'function', 'unregister method exists');
// register/unregister don't throw
fr.register({}, 'held');
fr.unregister({});
```

**Verify:**
```bash
cmake --build cmake-build-asan --target check-hermes-node
```

---

### Fix 3: Revert stream async generator workarounds [amend Step 26, `dc1d66b`]

This is the largest fix. Three vendored files were patched and one shim was created
unnecessarily.

#### 3a. Revert `libjs-node/internal/streams/readable.js`

Restore the original Node.js `async function* createAsyncIterator(stream, options)`
at line 1362. The diff from Step 26 commit `dc1d66b` shows exactly what was changed.

**Method:** Get the original file from the commit before Step 26:
```bash
git show dc1d66b~1:libjs-node/internal/streams/readable.js > libjs-node/internal/streams/readable.js
```

Or equivalently, reverse-apply only the readable.js hunk from the Step 26 diff.

#### 3b. Revert `libjs-node/internal/streams/pipeline.js`

Restore the original `async function* fromReadable(val)` at line 90.

**Method:**
```bash
git show dc1d66b~1:libjs-node/internal/streams/pipeline.js > libjs-node/internal/streams/pipeline.js
```

#### 3c. Delete `libjs/shims/internal/streams/operators.js`

Remove the empty stub shim. With async generators enabled, the original
`libjs-node/internal/streams/operators.js` will parse and load correctly.

The original `operators.js` imports `AbortController` and `AbortSignal` from
`internal/abort_controller` — our shim (which we're keeping for
`FinalizationRegistry` reasons) exports both of these.

```bash
rm libjs/shims/internal/streams/operators.js
```

If the parent directory `libjs/shims/internal/streams/` is now empty, remove it too.

#### 3d. Update `abort_controller.js` shim comments

**File:** `libjs/shims/internal/abort_controller.js`

The header comment (lines 1-11) mentions async generators. Update to accurately
reflect the real reason for the shim:

```js
// Copyright (c) Tzvetan Mikov.
//
// Minimal AbortController/AbortSignal shim for Hermes.
// The original Node.js internal/abort_controller.js depends on:
//   - FinalizationRegistry (not available in Hermes)
//   - internal/event_target (complex dependency chain)
//   - internal/webidl
//   - internal/worker/js_transferable
//
// This shim provides a minimal implementation sufficient for stream
// and fs operation abort signaling without those heavy dependencies.
```

#### 3e. Update `libjs-node/README.md`

Remove the modification log entries for `readable.js` and `pipeline.js` from the
Step 26 section, since those files are no longer modified.

#### 3f. Update `test/test-streams.js`

The existing stream tests should still pass. Additionally, add tests for stream
operators that were previously unavailable:

```js
// Stream operators (previously unavailable, now working with async generators)
const { Readable } = require('stream');

// Test .map() operator
const mapped = Readable.from([1, 2, 3]).map((x) => x * 2);
const mapResults = [];
mapped.on('data', (d) => mapResults.push(d));
mapped.on('end', () => {
  assert(mapResults.length === 3);
  assert(mapResults[0] === 2);
  assert(mapResults[1] === 4);
  assert(mapResults[2] === 6);
});

// Test .filter() operator
const filtered = Readable.from([1, 2, 3, 4]).filter((x) => x % 2 === 0);
const filterResults = [];
filtered.on('data', (d) => filterResults.push(d));
filtered.on('end', () => {
  assert(filterResults.length === 2);
  assert(filterResults[0] === 2);
  assert(filterResults[1] === 4);
});
```

Note: The exact stream operator API may need adjustment based on how Node's
operators work (they return new Readable streams). Some operators may need
`async` callbacks. Test and adjust.

**Verify:**
```bash
cmake --build cmake-build-asan --target check-hermes-node
```

---

### Fix 4: Revert `internal/fs/dir.js` workaround [amend Step 29, `489723c`]

#### 4a. Revert `libjs-node/internal/fs/dir.js`

Restore the original `async* entries()` method at line 294.

**Method:**
```bash
git show 489723c~1:libjs-node/internal/fs/dir.js > libjs-node/internal/fs/dir.js
```

#### 4b. Update `libjs-node/README.md`

Remove the modification log entry for `dir.js` from the Step 29 section.

#### 4c. Update `test/test-fs-dir.js`

Add a test for async directory iteration using `for await...of`:

```js
// Test async iteration with for-await-of (requires async generators)
const asyncDir = fs.opendirSync(tmpDir);
const asyncNames = [];
// Use an async IIFE since top-level await isn't available
(async () => {
  for await (const dirent of asyncDir) {
    asyncNames.push(dirent.name);
  }
  assert(asyncNames.sort().join(',') === 'a.txt,b.txt,sub');
})().then(() => { /* ... */ });
```

**Verify:**
```bash
cmake --build cmake-build-asan --target check-hermes-node
```

---

### Fix 5: Test `fs.promises` [amend Step 32, `0108960`]

With async generators enabled, `internal/fs/promises.js` should no longer throw
a SyntaxError when parsed. However, it has eager top-level requires that may not
all be available:

- `require('internal/readline/interface')` (line 118) — may need a shim
- `require('internal/worker/js_transferable')` (line 121) — may need a shim

#### 5a. Attempt to load `fs.promises`

Try `require('fs').promises` and see what fails. Create minimal shims for any
missing dependencies. Likely candidates:

- `internal/readline/interface` — may need a stub exporting a minimal `Interface` class
- `internal/worker/js_transferable` — may need a stub exporting `kDeserialize`,
  `kTransfer`, `kTransferList`, `markTransferMode`

#### 5b. Update `test/test-fs-async-verify.js`

Replace the "fs.promises permanently unavailable" test with actual promise-based
tests:

```js
// fs.promises should now work (async generators enabled)
const fsp = require('fs').promises;

// Basic promise operations
const tmpDir = fs.mkdtempSync('/tmp/hermes-promises-');
await fsp.writeFile(tmpDir + '/p.txt', 'promise-hello');
const data = await fsp.readFile(tmpDir + '/p.txt', 'utf8');
assert(data === 'promise-hello');

const stat = await fsp.stat(tmpDir + '/p.txt');
assert(stat.isFile());

await fsp.rm(tmpDir, { recursive: true });
```

If `fs.promises` still cannot load due to missing dependencies that are too complex
to shim, document the actual blocking dependency (NOT async generators) and add
the simpler shims that are feasible.

**Verify:**
```bash
cmake --build cmake-build-asan --target check-hermes-node
```

---

### Fix 6: Update documentation [amend Step 33, `8957f55`]

#### 6a. `history/initial/issues-and-workarounds.md`

Major rewrite of the "No Async Generators" section. Replace with a note that async
generators ARE supported (with `withEnableAsyncGenerators(true)`) and remove all
references to async generators being a limitation. Update the shims table, vendored
file modifications table, and unimplemented features lists accordingly.

#### 6b. `history/initial/memory.md`

- Remove "No async generators" from "Hermes JS Limitations" section
- Update "Streams Verification" section to remove async generator workaround notes
- Update "FS Dir Binding" section to remove dir.js patch note
- Update "FS Async Verification" section re: `fs.promises`
- Update "Core Module Readiness" section to remove `Duplex.from()` broken note
  (if it now works)

#### 6c. `history/initial/progress.md`

Update context notes for Steps 4, 8, 26, 29, 32 to reflect the fixes. Add a note
explaining the async generator discovery and fix.

---

## Rebase Strategy

After all fix commits are created on top of HEAD, perform an interactive rebase
to squash each fix into its corresponding step:

```bash
git rebase -i 939bb5a~1
```

Reorder the pick list so each fix commit immediately follows its target step,
then mark fix commits as `fixup` (or `squash` if you want to merge commit messages):

```
pick 939bb5a Step 4: Implement primordials thin shim
fixup XXXXXXX Fix 2: Fix primordials AsyncIteratorPrototype
pick 1c298fe Step 5: Implement internalBinding registry
pick 4361bfb Step 6: Implement internal module loader
pick 7c9b402 Step 7: Implement process object (basic properties)
pick 0ef1fe4 Step 8: Implement bootstrap sequence
fixup XXXXXXX Fix 1: Enable async generators in runtime
pick c78207e Step 9: Port constants binding
...
pick dc1d66b Step 26: Verify streams work
fixup XXXXXXX Fix 3: Revert stream async generator workarounds
...
pick 489723c Step 29: Port fs_dir binding
fixup XXXXXXX Fix 4: Revert fs/dir.js workaround
...
pick 0108960 Step 32: Verify fs async operations
fixup XXXXXXX Fix 5: Test fs.promises
pick 8957f55 Step 33: Run Node.js fs test subset (8/8 pass)
fixup XXXXXXX Fix 6: Update documentation
```

### Conflict Risk Assessment

Each fix commit only modifies files that were originally changed in its target step.
No intermediate steps (between a target and its fix) modify the same files. Therefore
the rebase should be conflict-free:

| Fix | Files Modified | Intermediate Steps Touching Same Files | Risk |
|-----|---------------|---------------------------------------|------|
| Fix 1 (→ Step 8) | `hermes-node.cpp` | Steps 9,21,22,23 register bindings in same file | **Medium** — but changes are in different sections (RuntimeConfig vs binding registration) |
| Fix 2 (→ Step 4) | `primordials.js`, `test/primordials.js` | Step 17 modified `primordials.js` | **Medium** — Step 17 added `Error.captureStackTrace` polyfill, different section. FinalizationRegistry polyfill goes before line 232, well separated from Step 17's changes. |
| Fix 3 (→ Step 26) | `readable.js`, `pipeline.js`, `operators.js` shim, `abort_controller.js`, `test-streams.js`, `README.md` | None touch these files | **Low** |
| Fix 4 (→ Step 29) | `dir.js`, `test-fs-dir.js`, `README.md` | None touch these files | **Low** |
| Fix 5 (→ Step 32) | `test-fs-async-verify.js`, possibly new shim files | None | **Low** |
| Fix 6 (→ Step 33) | `memory.md`, `progress.md`, `issues-and-workarounds.md` | Many steps update these | **High** — may need manual conflict resolution for documentation files |

### Handling Fix 6 (documentation) Conflicts

Documentation files (`memory.md`, `progress.md`) are modified in nearly every step.
Squashing Fix 6 into Step 33 will likely conflict with earlier steps that also
modified these files.

**Recommended approach:** Instead of squashing Fix 6 into a single step, split it:
- Documentation changes relevant to Step 4 → squash into Step 4 (with Fix 2)
- Documentation changes relevant to Step 26 → squash into Step 26 (with Fix 3)
- etc.

Or alternatively: apply documentation fixes as a final standalone commit after
Step 33 (no squash needed for docs).

---

## Verification After Rebase

After the rebase completes, run the full test suite:

```bash
cmake --build cmake-build-asan --target check-hermes-node
```

All existing tests must pass. The new tests added in Fixes 3-5 must also pass.

Then verify key capabilities that were previously broken:
```bash
# Stream operators work
echo "const {Readable} = require('stream'); Readable.from([1,2,3]).map(x=>x*2).toArray().then(r => { if(r.join(',') === '2,4,6') print('PASS'); else print('FAIL: '+r); })" | cmake-build-asan/bin/hermes-node --node-lib-path .

# for-await-of on directory iteration works
# fs.promises loads (if shims were sufficient)
```

---

## Summary of Net Changes After All Fixes

| Item | Before | After |
|------|--------|-------|
| `withEnableAsyncGenerators` | not set (false) | `true` |
| `FinalizationRegistry` | missing (typeof === 'undefined') | no-op polyfill in `primordials.js` (guard: only if real one absent) |
| `SafeFinalizationRegistry` | missing from primordials | available (backed by polyfill or native) |
| `primordials.AsyncIteratorPrototype` | manual stub object | real prototype from `async function*` chain |
| `readable.js` | patched (manual async iterator) | original Node.js code |
| `pipeline.js` | patched (direct delegation) | original Node.js code |
| `operators.js` | empty shim | original Node.js code (shim deleted) |
| `dir.js` | patched (manual async iterator) | original Node.js code |
| `abort_controller.js` shim | kept (needed for dependency chain) | kept, comments corrected |
| Stream operators (`.map`, `.filter`, etc.) | unavailable | **available** |
| `Duplex.from()` | broken | **likely working** |
| `fs.promises` | permanently unavailable | **available** (may need dependency shims) |
| `for await...of` on Dir | unavailable | **available** |
