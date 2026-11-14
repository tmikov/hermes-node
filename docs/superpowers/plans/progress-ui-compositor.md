# UI compositor — progress

Tracks `docs/superpowers/plans/2026-09-21-ui-compositor.md`, which implements
`docs/superpowers/specs/2026-09-18-ui-compositor-design.md`. Branch
`work-imgui`. Everything is behind `HERMES_NODE_ENABLE_UI`, default OFF.

## Status

| Slice | Tasks | State | Finished |
|---|---|---|---|
| A — foundations, no GPU | 1–4 | **complete** | 2026-09-22 |
| B — one window presenting one known pattern | 5–10 | **complete** | 2026-09-25 |
| C — the rest of the surface area | 11–14 | not started | — |
| D — lifecycle, Metal, suite | 15–17 | not started | — |

Slice A: 49 tests in `unittests/CompositorTest.cpp`. Slice B: 33 commits, 75
files, and the suite at 414 unit tests plus 201 JS tests.

## What Slice B actually produced

`hermes-node-ui` opens a real window, the reference addon draws a fixed
pattern into a compositor surface **on the JS thread**, the platform thread
blits it, and `test/ui-present.js` compares the presented pixels. The
comparison is **exact** — 0 or 255, no tolerance — with samples kept 8px from
any edge. That is stronger than a threshold rather than weaker, and it is
affordable because the capture's agreement rule forces target size to equal
framebuffer size, so the blit is 1:1, linear filtering degenerates to a copy,
the pattern is flat-shaded, and nothing on the path converts colour (the
swapchain is RGBA8, not sRGB). The presenter's own clear is `32/255`,
deliberately a value the pattern never produces, so a missing blit is
legible rather than ambiguous.

Per task: **5** the `hermes-node:` embedder-module prefix; **6** the binary,
`UiHost`, and the windowless path; **7** the public C ABI (`hnui_abi.h`);
**8** Sokol vendored at `c0db757`, the GPU target, two `sokol_gfx` instances
under renamed symbols, and the window; **9** test hooks — controllable
completion, frame capture, synthetic event injection; **10** the picture.

## Things a newcomer needs to know

- **Four lit features, and they are not interchangeable.** `ui-enabled`
  probes the *plain* `hermes-node` binary for `--test-embedder-module` and
  really means "the CMake option was compiled in". `ui` probes the separate
  `hermes-node-ui` binary. `ui-gpu` is `ui-enabled` **and** a backend
  platform, mirroring `if(NOT (UNIX AND NOT APPLE))` in
  `lib/compositor-gpu/CMakeLists.txt`. `display` starts a real Xvfb in the
  RUN line. The names mislead: `ui-enabled` is the one that sounds like it
  means the UI and is the one that does not.
- **Environment variables**: `HERMES_NODE_UI_TEST_HOOKS`,
  `HERMES_NODE_UI_GL_ROUTE`, `HERMES_NODE_UI_GL_COMPLETION`, and
  `HERMES_NODE_DEBUG_NATIVE=UI` for tracing. `test/lit.cfg` replaces the
  environment for ShTest, so an exported value cannot reach a lit test —
  load-bearing, because `ui-present.js`'s third run depends on the
  completion variable being *unset*.
- **Assertions come from tracing, never from timing.** The suite runs 16-way
  parallel.
- **`sg_*` on the JS thread, `sgp_*` on the main thread.** One `_sg` global
  per process is why the presenting copy is compiled under renamed symbols;
  `utils/gen-sokol-rename.py` generates them and
  `utils/check-sgp-isolation.py` proves the two objects share no symbols.

## Known defects and limitations

Filed in `dz/`, not duplicated here — `dz list` is authoritative. As of the
end of Slice B: a Slice A test asserting a wall-clock throughput floor that
fails under machine load; lit setting no per-test timeout, so a hung test
stalls the suite; the completion probe's 2 s handshake budget now being
load-bearing; and closing the window hanging the process, which Task 15 owns.

## Obligations carried into later tasks

All live in the plan itself, at the task that discharges them, because task
briefs are cut from the plan and an obligation recorded anywhere else is one
no brief will carry. Task 11 owns the `sapp_event` → `UiEvent` conversion's
coverage and the dispatch loop's address-stability constraint; Task 16 owns a
backend-local copy of the `sg_view` asserts (optional — `HnuiAbiTest` already
covers every platform); Task 17 owns the first real execution of the posted
destroy callback, which is wired but has never fired because this build
allocates one generation and never retires it.
