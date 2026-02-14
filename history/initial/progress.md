# Implementation Progress

Tracks progress on `history/initial/2026-02-14-hermes-node-compat-detailed-plan.md`.

The file has two sections: "Status" and "Context Notes".

**Status Section**:

Each row contains the step label from the detailed, plan, a brief description, list of
dependency labels, Status (initially empty), optional brief note (initially empty).

The status of a row is one of:
- "" (empty) initially, before work has started
- "wip" as soon as work on that raw has started.
- "done" when work has completed successfully. Rarely "Brief Note" may contain very brief
explanation. More details in "Context Notes".
- "blocked" when work cannot proceed for some reason. "Brief Note" must contain a brief
explanation. More details in "Context Notes".

Example on start:

| Step 11 | Port util binding | 5 |  |  |

**Context Notes**:

After completing work on a step, either successfully or by blocking, a section for that
step is added. It needs to have the format from the following example (empty bullets can
be omitted):

```
### Step 11: Port util binding
- **Files**: created `foo.c`, modified `bar.c`.
- **Decisions**:
-- Decision 1 concise explanation
-- Decision 2 concise explanation
- **What was done**: ...
- **Issues**: ...
- **Notes for next step**: ...
```

## Status

| Step | Description | Depends On | Status | Brief Note (optional) |
|------|-------------|------------|--------|-----------------------|
| Step 1 | Create repo and CMake scaffolding | — | done | |
| Step 2 | Vendor libuv | 1 | | |
| Step 3 | Implement libuv-backed event loop adapter | 2 | | |
| Step 4 | Implement primordials thin shim | — | | |
| Step 5 | Implement internalBinding registry | 1 | | |
| Step 6 | Implement internal module loader | 5 | | |
| Step 7 | Implement process object (basic properties) | 5 | | |
| Step 8 | Implement bootstrap sequence | 3, 4, 6, 7 | | |
| Step 9 | Port constants binding | 5 | | |
| Step 10 | Port types binding | 5 | | |
| Step 11 | Port util binding | 5 | | |
| Step 12 | Port string_decoder binding | 5 | | |
| Step 13 | Port errors binding | 5 | | |
| Step 14 | Port config binding | 5 | | |
| Step 15 | Port symbols binding | 5 | | |
| Step 16 | Implement internal/options shim | 6 | | |
| Step 17 | Verify bootstrap modules load | 8, 9–16 | | |
| Step 18 | Port buffer binding | 5 | | |
| Step 19 | Port encoding_binding | 5 | | |
| Step 20 | Port async_wrap binding (stub) | 5 | | |
| Step 21 | Implement process.nextTick | 3, 7 | | |
| Step 22 | Implement timers binding | 3, 5 | | |
| Step 23 | Implement process.stdout/stderr (minimal) | 7, 21 | | |
| Step 24 | Verify core modules load and work | 17–23 | | |
| Step 25 | Port stream_wrap binding (minimal) | 5, 3 | | |
| Step 26 | Verify streams work | 24, 25 | | |
| Step 27 | Port fs binding — sync operations | 2, 5, 9 | | |
| Step 28 | Port fs binding — async operations | 3, 27 | | |
| Step 29 | Port fs_dir binding | 27 | | |
| Step 30 | Port fs_event_wrap binding | 3, 5 | | |
| Step 31 | Verify fs sync operations | 27, 9 | | |
| Step 32 | Verify fs async operations | 28, 29 | | |
| Step 33 | Run Node.js fs test subset | 28, 29, 30 | | |

## Context Notes

### Step 1: Create repo and CMake scaffolding
- **Files**: created `CMakeLists.txt`, `lib/placeholder/CMakeLists.txt`, `lib/placeholder/placeholder.cpp`, `tools/hermes-node/CMakeLists.txt`, `tools/hermes-node/hermes-node.cpp`, `unittests/CMakeLists.txt`, `libjs-node/README.md`. Vendored 347 JS files from Node.js into `libjs-node/`.
- **Decisions**:
  - Use Hermes cmake macros (`add_hermes_library`, `add_hermes_tool`, `add_unittest`) for consistency with Hermes build conventions.
  - `check-hermes-node` custom target depends on `NodeCompatUnitTests` (unit test suite target).
  - `add_node_compat_unittest()` helper function in `unittests/CMakeLists.txt` mirrors Hermes's `add_hermes_unittest()`.
- **What was done**: Created full directory structure (external/, include/hermes/node-compat/, lib/placeholder/, libjs/, libjs-node/, tools/hermes-node/, unittests/, test/). Top-level CMakeLists.txt adds hermes submodule, includes Hermes cmake modules, builds placeholder lib and hermes-node tool. Vendored Node.js v24.13.0 lib/ tree (commit def0bdf8) into libjs-node/ with provenance README. Built and verified: `hermes-node` binary prints usage with no args and exits 0; `check-hermes-node` target succeeds.
- **Notes for next step**: The `hermesNodePlaceholder` library is temporary; replace with real libraries as they are implemented. Hermes key targets: `hermesNapi` (NAPI lib), `hermesvm_a` (static VM), `gtest_main` (testing). LLVH_SOURCE_DIR must be set for gtest includes.

