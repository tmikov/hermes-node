# Implementation Progress

Tracks progress on `history/initial/2026-02-24-debugger-plan.md`.

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
| Step 1 | Enable HERMES_ENABLE_DEBUGGER in CMake | — |  |  |
| Step 2 | Switch runtime creation to makeHermesRuntime() | 1 |  |  |
| Step 3 | Add --inspect / --inspect-brk flag parsing | — |  |  |
| Step 4 | Create CDPDebugAPI and CDPAgent with placeholder callbacks | 2 |  |  |
| Step 5 | Add RuntimeTask queue and uv_async_t for CDP processing | 4 |  |  |
| Step 6 | Vendor ws package | — | done | Vendored ws 8.19.0 in prior commit |
| Step 7 | Add inspectorBridgeContext to config and RuntimeState | 5 |  |  |
| Step 8 | Create inspector_bridge native binding | 7 |  |  |
| Step 9 | Create inspector JS server script | 6, 8 |  |  |
| Step 10 | Start inspector runtime on a dedicated thread | 5, 8, 9 |  |  |
| Step 11 | Wire end-to-end CDP message flow | 10 |  |  |
| Step 12 | Add /json discovery endpoints | 10 |  |  |
| Step 13 | Add DevTools CDN redirect | 12 |  |  |
| Step 14 | Add --inspect-brk (pause at first line) | 11 |  |  |
| Step 15 | Add stderr diagnostic messages | 10 |  |  |
| Step 16 | End-to-end integration test | 11, 12, 13, 14, 15 |  |  |

## Context Notes

