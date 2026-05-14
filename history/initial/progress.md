# Implementation Progress

Tracks progress on `history/initial/cjs-module-resolution-plan.md`.

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
| S1 | Add module_wrap binding stub | — | | |
| S2 | Implement real readPackageJSON in modules binding | — | | |
| S3 | Implement real compileFunctionForCJSLoader in contextify binding | — | | |
| S4 | Add legacyMainResolve to fs binding | — | | |
| S5 | Embed required modules | S1, S2 | | |
| S6 | Create/update shims for newly embedded modules | S5 | | |
| S7 | Integrate Node's CJS loader with bootstrap | S3, S6 | | |
| S8 | Test: basic node_modules resolution | S7 | | |
| S9 | Test: package.json "main" field | S7 | | |
| S10 | Test: package.json "exports" field | S7 | | |
| S11 | Test: .json file loading | S7 | | |
| S12 | Test: nested node_modules | S7 | | |
| S13 | Test: circular deps across node_modules | S7 | | |
| S14 | Test: require.resolve | S7 | | |
| S15 | Test: real npm package | S8–S14 | | |

## Context Notes

