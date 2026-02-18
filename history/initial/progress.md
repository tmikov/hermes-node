# Implementation Progress

Tracks progress on `history/initial/2026-02-18-repl-plan.md`.

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
| R1 | Stub `internalBinding('modules')` | — | done | |
| R2 | Shim `internal/modules/helpers.js` | R1 | | |
| R3 | Enhance `BuiltinModule` shim for REPL | — | | |
| R4 | Stub `internal/modules/esm/formats.js` | — | | |
| R5 | Stub `domain` module | — | | |
| R6 | Remove `internal/readline/interface.js` shim | — | | |
| R7 | Implement minimal contextify -- ContextifyScript | — | | |
| R8 | Stub `startSigintWatchdog` / `stopSigintWatchdog` | R7 | | |
| R9 | Stub `makeContext` and `compileFunction` | R7 | | |
| R10 | Stub `internal/modules/esm/utils.js` if needed | — | | |
| R11 | Verify `vm` module loads | R7, R8, R9 | | |
| R12 | Verify `readline` module loads | R6 | | |
| R13 | Verify `domain` module loads | R5 | | |
| R14 | Shim CJS loader `Module` class for REPL | R2 | | |
| R15 | Wire REPL entry point in `hermes-node.cpp` | R7, R11 | | |
| R16 | Handle `repl.js` line 216 -- `vm.runInNewContext` | R9 | | |
| R17 | Integration test -- REPL loads | R1-R6, R7-R9, R14-R16 | | |
| R18 | REPL entry point test (pipe mode) | R15, R17 | | |
| R19 | Implement SIGINT watchdog | R7, R8 | | |
| R20 | Verify REPL features | R17, R18 | | |
| R21 | REPL history persistence | R6, R17 | | |

## Context Notes

### R1: Stub `internalBinding('modules')`
- **Files**: created `lib/bindings/node_modules.cpp`, `include/hermes/node-compat/bindings/node_modules.h`, `test/test-modules-binding.js`. Modified `lib/bindings/CMakeLists.txt`, `tools/hermes-node/hermes-node.cpp`.
- **What was done**: Created stub `modules` binding with no-op functions: `enableCompileCache`, `getCompileCacheDir`, `flushCompileCache`, `readPackageJSON`, `getPackageScopeConfig`, `getPackageType`, `getNearestParentPackageJSONType`, `setLazyPathHelpers`. Also exports `compileCacheStatus` array `["FAILED", "ENABLED", "ALREADY_ENABLED", "DISABLED"]` (consumed by `helpers.js` to build name->index map).
- **Decisions**: Included stubs for `readPackageJSON`/`getPackageScopeConfig`/`getPackageType`/`getNearestParentPackageJSONType`/`setLazyPathHelpers` beyond what `helpers.js` needs, since `package_json_reader.js`, `run_main.js`, and `esm/initialize_import_meta.js` also destructure from this binding.

