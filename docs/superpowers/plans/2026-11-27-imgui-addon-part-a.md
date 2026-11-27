# Dear ImGui Addon, Part A: Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `require('hermes-node:imgui')` in `hermes-node-ui` loads a Dear ImGui addon, built on the public `hnui` API only, that shows ImGui's demo window from a JS program on GL/X11 and Metal.

**Architecture:** ImGui is vendored unmodified under `external/imgui` and compiled into a `.node` shared object in `addons/imgui/`, which holds a platform half (events, cursor, clipboard on `hnui`), a renderer half (`sokol_gfx` through the host's exported `sg_*` symbols) and a three-function N-API surface. `hermes-node-ui` registers the embedder module `hermes-node:imgui`, whose init loads `hermes-node-imgui.node` from the executable's directory through the code `process.dlopen` uses.

**Tech Stack:** C++17, Dear ImGui `v1.93.0-docking`, `sokol_gfx` at the vendored pin, Node-API, CMake + Ninja, lit + FileCheck.

**Spec:** `docs/superpowers/specs/2026-11-27-imgui-addon-design.md`. Read it whole before any task; this plan does not repeat its reasoning.

**Progress file:** `docs/superpowers/plans/progress-imgui-addon.md` (created in Task 1; update it at the end of every task).

## Global Constraints

- The addon includes only `hnui.h` / `hnui_abi.h`, `node_api.h`, the vendored `sokol_gfx.h` (declarations only, never `SOKOL_GFX_IMPL`) and ImGui. No header from `tools/`, `lib/` or any other `include/hermes/node-compat/` directory.
- `external/imgui/imgui/` is byte-identical to upstream tag `v1.93.0-docking`. Configuration goes through `addons/imgui/imgui_config.h`, named by `IMGUI_USER_CONFIG`.
- Everything new is built only under `HERMES_NODE_ENABLE_UI`.
- New files carry `Copyright (c) Tzvetan Mikov.` and the MIT header used in `include/hermes/node-compat/ui/hnui.h`. Vendored files are not touched.
- Commit messages: ASCII only, ending with `Co-Authored-By: Claude Fable 5.2 <noreply@anthropic.com>`. Never write a commit hash into any committed file; an upstream **tag** is fine.
- Before every commit: `./utils/format.sh -f`, then `cmake --build cmake-build-asan --target check-hermes-node` (the UI is on in `cmake-build-asan`). One test: `utils/lit-run.sh cmake-build-asan -v $(pwd)/test/<name>.js`. Never run two lit invocations over one build directory at once.
- Window tests are named `test/ui-imgui-*.js` so that `utils/check-ui-stderr-rule.py` (which globs `ui-*.js`) covers them. Each is `// REQUIRES: ui, ui-gpu, ui-imgui` and `// REQUIRES: display`, runs under `%with-display 300 sh -c '... > %t.out 2> %t.err'`, and reads `%t.err` with `--implicit-check-not='[sapp:error' --implicit-check-not='[sokol-'`. Model the RUN block on `test/ui-events.js`.
- Assertions come from traces and read-backs, never from elapsed time. A test that waits for ImGui state runs frames up to a frame-count bound (use 600) and fails with a printed line when the bound is hit.
- **Every test must be shown able to fail:** after it passes, break the code it tests (the step says how, or pick the line that implements the assertion), see it fail, restore. Record each mutation and its result in the progress file.
- Trace lines are printed only under `HERMES_NODE_DEBUG_NATIVE=UI`, to stderr, as `[ui] imgui: <text>`, and say what was done or read back.
- An `hnui` gap found on the way is fixed in `hnui`, in a commit of its own with its own test, never worked around through a private header.
- macOS cannot run the formatter; `.mm` is not expected here (the addon is plain C++ on both platforms).

## Review Focus

Inputs the spec implies and no task's listed test would otherwise exercise; each has its test in the task named.

1. **The addon used with no window** (`require` before `createWindow`, or in a script that never creates one): `newFrame()` returns false, `render()` throws the no-frame error, nothing crashes. Task 4.
2. **Paste when the clipboard has no text** (nothing copied yet, or the owner gone): the field is unchanged and one trace line reports the status; no hang beyond the 500 ms deadline. Task 8.
3. **Text outside the Basic Multilingual Plane** (an emoji, four UTF-8 bytes): it reaches the text field intact, which needs `IMGUI_USE_WCHAR32`. Task 7.
4. **Key codes ImGui has no key for** (0, 999): ignored, no assertion, later keys still work. Task 7.
5. **Focus lost with a key and a mouse button held:** ImGui reports neither down afterwards. Task 7.

## File Structure

| Path | Responsibility |
|---|---|
| `external/imgui/imgui/` | Upstream files at the tag: `imgui.cpp imgui.h imgui_demo.cpp imgui_draw.cpp imgui_internal.h imgui_tables.cpp imgui_widgets.cpp imconfig.h imstb_rectpack.h imstb_textedit.h imstb_truetype.h LICENSE.txt`. |
| `external/imgui/README.md`, `external/imgui/CMakeLists.txt` | Pin, file list, why; an `INTERFACE` target `imgui_headers` and a cache variable `IMGUI_SOURCES`. |
| `lib/process/node_process.cpp`, `include/hermes/node-compat/process/node_process.h` | `loadNativeAddon()` extracted from `processDlopen`. |
| `tools/hermes-node-ui/named_addons.{h,cpp}` | The name-to-file table and the embedder init that loads a named addon. |
| `addons/imgui/CMakeLists.txt` | Builds `hermes-node-imgui.node` into `${HERMES_TOOLS_OUTPUT_DIR}`. |
| `addons/imgui/imgui_config.h` | ImGui configuration. |
| `addons/imgui/imgui_addon.h` | Declarations shared by the addon's units (below). |
| `addons/imgui/imgui_module.cpp` | N-API surface, the frame state machine, tracing. |
| `addons/imgui/imgui_platform.cpp` | Events, display size, time, cursor, clipboard. |
| `addons/imgui/imgui_renderer.cpp` | Shader, pipeline, buffers, textures, draw loop. |
| `addons/imgui/imgui_test_hooks.cpp` | `__test.scene`, `__test.state`. |
| `addons/imgui/README.md` | What it is, loading, JS surface, limits. |
| `test/fixtures/imgui/harness.js` | Shared test helpers (below). |
| `test/fixtures/imgui/probe.js` | Prints `imgui ok <version>`; used by lit to decide `ui-imgui`. |
| `test/imgui-pin.js`, `test/ui-imgui-*.js` | Tests. |
| `examples/imgui-demo/demo.js`, `README.md` | The JS program from the spec. |

**Shared declarations, `addons/imgui/imgui_addon.h`** (namespace `hnimgui`; every unit sees exactly these):

```cpp
void trace(const char *fmt, ...);        // "[ui] imgui: ..." when UI tracing is on
bool testHooksEnabled();                 // HERMES_NODE_UI_TEST_HOOKS, same rule as the host
// platform
void platformInit(hnui_window *w);       // listener, clipboard hooks, backend flags
void platformNewFrame(hnui_window *w);   // DisplaySize, DisplayFramebufferScale, DeltaTime
void platformAfterRender(hnui_window *w);// cursor
int lastCursorRequested();               // ImGuiMouseCursor last acted on, for state()
// renderer
void rendererInit();                     // backend flags, sampler; lazily called
void rendererDraw(ImDrawData *dd, const hnui_target &target); // textures, pass, draws
struct RendererStats { int commands, vertices, indices, liveTextures; };
RendererStats rendererStats();           // totals of the last rendererDraw
// test hooks
napi_value createTestHooks(napi_env env);// the `__test` object
```

**Test harness, `test/fixtures/imgui/harness.js`** exports:

- `open(options)`: `await ui.createWindow({title, width: 640, height: 480, ...options})`, requires `hermes-node:imgui`, returns `{ui, win, imgui}`.
- `tick(h, fns)`: registers one frame callback per function in `fns`, all for the same tick, and resolves after the last has run, with the array of their return values. This is the only way the harness draws. It keeps `h.published`, the id of the last publication it caused, incremented **once per tick** in which any callback's `render()` returned true (the host submits once per tick however many renders ran).
- `frame(h, drawFn)`: `tick` with the single callback `() => { imgui.newFrame(); drawFn(); return imgui.render(); }`, resolving that boolean.
- `resync(h)`: for use after anything that can retire a generation (a resize), since a retired generation's work gets no publication id. It first awaits one callback-only tick (`tick(h, [() => false])`, no `newFrame`, no `render`), which is the completion barrier: the host sends no tick while the previous drawing tick's publication is outstanding. Then, with nothing drawing, it issues a `ui.__captureFrame` that cannot match (`publicationId` 0, `giveUpAfterFrames` 2) and sets `h.published` to the `publicationId` its result reports. **Prerequisite, done in Task 4 as a commit of its own before the harness is written:** today the unmatched result leaves `publicationId` 0 and only traces the id (`tools/hermes-node-ui/ui_platform.cpp:158-187`, result built at `ui_test_hooks.cpp:107`); change the give-up branch to return the publication that was presenting, document it beside `__captureFrame`, and extend `test/ui-present.js`'s expected-non-match case to check the returned id equals the last matched one. Confirm the barrier claim by reading `lib/compositor/tick_machine.cpp` before relying on it; if a callback-only tick can be sent while a publication is outstanding, stop and report.
- `until(h, drawFn, predicate, label)`: runs `frame` until `predicate(imgui.__test.state())` is true, at most 600 frames; on the bound prints `TIMEOUT <label>` and `process.exit(1)`.
- `capture(h, drawFn, path)` and `captureTick(h, fns, path)`: install `ui.__captureFrame` for publication `h.published + 1`, with the agreement fields of `test/ui-present.js` taken from `win` (sizes, `dpiScale`) and the generation from `h.generation`, run one `frame` / `tick`, await the capture, fail with `CAPTURE unmatched <label>` if it did not match, return the parsed image.
- `h.generation` starts at 1 and is **read back, not counted**: after a resize the test draws one uncaptured frame and sets `h.generation = imgui.__test.state().targetGeneration` (the generation of the target `render()` last drew into), then calls `resync(h)`.
- A test awaits a matched capture of the current frame before it causes a resize, and installs its `resize` listener before sending the driver command, as `test/ui-real-events.js:146-180` does.
- `readPpm(path)`, `pixelAt(image, x, y)` moved from `test/ui-present.js` semantics (rows top first, RGB).
- `CLEAR = [26, 26, 31]`.

---

### Task 1: Vendor Dear ImGui

**Files:**
- Create: `external/imgui/imgui/*` (the twelve files above), `external/imgui/README.md`, `external/imgui/CMakeLists.txt`, `test/imgui-pin.js`, `docs/superpowers/plans/progress-imgui-addon.md`
- Modify: `CMakeLists.txt` (add `add_subdirectory(external/imgui)` beside `external/sokol`, under `HERMES_NODE_ENABLE_UI`), `THIRD_PARTY_LICENSES.md`

**Interfaces:**
- Produces: CMake target `imgui_headers` (`INTERFACE`, `SYSTEM` include of `external/imgui/imgui`); cache variable `IMGUI_SOURCES` (absolute paths of the five `.cpp` files); cache variable `IMGUI_VERSION_PINNED` = `1.93.0`, extracted from the README line ``- **Pinned at tag `v1.93.0-docking`**`` the way `external/sokol/CMakeLists.txt` extracts its commit.

- [ ] **Step 1: Write `test/imgui-pin.js`**, modelled on `test/sokol-pin.js` (plain `hermes-node`, `// REQUIRES: ui-enabled`, unref'd 60 s watchdog). It reads `external/imgui/README.md` and `external/imgui/imgui/imgui.h` under `%source_dir` and prints `README ok` when the README's tag is `v<IMGUI_VERSION>-docking`, where `IMGUI_VERSION` is the header's `#define IMGUI_VERSION "..."`, and `DOCKING ok` when the header contains `#define IMGUI_HAS_DOCK`. `// CHECK: README ok`, `// CHECK: DOCKING ok`.
- [ ] **Step 2: Run it; expect FAIL** (files absent).
- [ ] **Step 3: Vendor.** `git clone --depth 1 --branch v1.93.0-docking https://github.com/ocornut/imgui` into a fresh directory under `/tmp`, copy exactly the twelve root files, and verify with `diff -r` over those files. Nothing from `backends/`, `examples/`, `misc/`, `docs/`.
- [ ] **Step 4: Write `external/imgui/README.md`** in the shape of `external/sokol/README.md`: upstream URL, MIT, copyright line from `LICENSE.txt`, the pinned-tag line, why the docking branch (spec: "Vendoring ImGui"), a table of the vendored files and why each is here, what is deliberately not vendored, and the re-sync steps. **Write `external/imgui/CMakeLists.txt`** producing the interfaces above. Add the ImGui entry to `THIRD_PARTY_LICENSES.md` in that file's existing format.
- [ ] **Step 5: Run `test/imgui-pin.js`; expect PASS.** Mutation: change the README tag to `v1.92.0-docking`; expect `WRONG`; restore.
- [ ] **Step 6: Create the progress file** (title, the plan and spec it tracks, a task table, a "Mutations" section, a "Notes" section) and commit: `Vendor Dear ImGui v1.93.0-docking`.

### Task 2: Extract the addon loader from `process.dlopen`

**Files:**
- Modify: `lib/process/node_process.cpp` (`processDlopen`, currently lines 879-966), `include/hermes/node-compat/process/node_process.h`

**Interfaces:**
- Produces, in namespace `hermes::node_compat`:
  ```cpp
  /// dlopen()s `filename`, finds its N-API init (napi_register_module_v1,
  /// else the last napi_module_register()ed module) and calls it with
  /// `exports`. On success stores the init's result (which may be
  /// `exports` itself or a replacement; never null) in `*result` and
  /// returns true. On failure returns false with a JS exception pending:
  /// ERR_DLOPEN_FAILED with process.dlopen's existing messages, or
  /// whatever the init threw.
  bool loadNativeAddon(napi_env env, const char *filename,
                       napi_value exports, napi_value *result);
  ```

- [ ] **Step 1: Baseline.** Run the existing addon tests and note they pass: `utils/lit-run.sh cmake-build-asan -v` over every `test/*.js` that matches `grep -l 'dlopen\|hello_addon\|\.node' test/*.js`.
- [ ] **Step 2: Move steps 4-8 of `processDlopen`** (the `dlopen` call through the init call) into `loadNativeAddon`, behaviour and message text unchanged; an init that returns null with no exception pending yields `*result = exports`. `processDlopen` keeps its argument handling and the `module.exports` assignment.
- [ ] **Step 3: Rerun the Step 1 set and `check-hermes-node`; expect the same results.** Mutation: make `loadNativeAddon` skip the `napi_register_module_v1` lookup; expect `test/` addon tests to fail; restore.
- [ ] **Step 4: Commit:** `Extract loadNativeAddon() from process.dlopen`.

### Task 3: Load a named addon; the addon skeleton

**Files:**
- Create: `tools/hermes-node-ui/named_addons.h`, `tools/hermes-node-ui/named_addons.cpp`, `addons/imgui/CMakeLists.txt`, `addons/imgui/imgui_config.h`, `addons/imgui/imgui_addon.h`, `addons/imgui/imgui_module.cpp`, `test/fixtures/imgui/probe.js`, `test/ui-imgui-feature.js`, `test/imgui-load.js`, `test/imgui-missing.js`
- Modify: `tools/hermes-node-ui/CMakeLists.txt` (add the source), `tools/hermes-node-ui/main.cpp` (register, beside `hermes-node:ui`), `CMakeLists.txt` (`add_subdirectory(addons/imgui)` after `tools/hermes-node-ui`; append the target to `HERMES_NODE_JS_TEST_DEPENDS` and `hermes_node_imgui=$<TARGET_FILE:hermes_node_imgui>` to `HERMES_NODE_LIT_PARAMS`), `test/lit.cfg`; create `test/imgui-symbols.js`

**Interfaces:**
- Consumes: `loadNativeAddon` (Task 2); `imgui_headers`, `IMGUI_SOURCES` (Task 1).
- Produces:
  - `bool registerNamedAddons(hermes::node_compat::HermesNodeConfig &config);` in `named_addons.h`; registers one `EmbedderModule` per table row. The table is `{"imgui", "hermes-node-imgui.node"}`.
  - CMake target `hermes_node_imgui`: `add_library(... SHARED)`, `PREFIX ""`, `OUTPUT_NAME hermes-node-imgui`, `SUFFIX .node`, `LIBRARY_OUTPUT_DIRECTORY ${HERMES_TOOLS_OUTPUT_DIR}`. Sources: the addon's units plus `${IMGUI_SOURCES}` (compiled with `-w`). Includes, backend defines and the Apple link option exactly as `hnui_demo_addon` has them (`CMakeLists.txt:173-224`), plus `imgui_headers` and `IMGUI_USER_CONFIG="imgui_config.h"` with `addons/imgui` on the include path.
  - Symbol isolation: `CXX_VISIBILITY_PRESET hidden`, `C_VISIBILITY_PRESET hidden`, `VISIBILITY_INLINES_HIDDEN ON` on the target (Hermes's own `-fvisibility=hidden` is scoped to its subdirectory and does not reach here). The N-API init stays exported through `NAPI_MODULE_INIT`'s own attribute.
  - `imgui_config.h` defines `IMGUI_DISABLE_OBSOLETE_FUNCTIONS`, `IMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS`, `IMGUI_USE_WCHAR32`.
  - Module exports after this task: `version` (string, `IMGUI_VERSION`). Init calls `HNUI_CHECK_VERSION(env)` first and traces `imgui: addon loaded (imgui <version>)`.
  - `hnimgui::trace` and `hnimgui::testHooksEnabled` (in `imgui_module.cpp`). The trace gate copies the parse rule of `lib/compositor-gpu/ui_trace.cpp` (read it; do not include it).
  - lit feature `ui-imgui` and substitution `%ui-imgui-state` (`on`/`off`).

**The embedder init** (one static function per row is not possible, since `init` takes no user data; with one row, one function): compute the executable's directory with `uv_exepath` then `realpath` (as `resolveKitDir` in `tools/hermes-node/hermes-node.cpp` does), append the file name; if `stat` says the file is absent, throw an `Error` with `code = 'MODULE_NOT_FOUND'` and message `Cannot find module 'hermes-node:imgui': <full path> does not exist`; otherwise return `loadNativeAddon`'s result (null on failure, exception pending).

- [ ] **Step 1: Write `test/fixtures/imgui/probe.js`:** `console.log('imgui ok ' + require('hermes-node:imgui').version)`.
- [ ] **Step 2: Write the three tests.**
  - `test/imgui-load.js` (`// REQUIRES: ui, ui-imgui`; no window, so `%timeout 60`, not `%with-display`). One run under `HERMES_NODE_DEBUG_NATIVE=UI` prints: `RESOLVE hermes-node:imgui` (the value of `require.resolve`), `SAME true` (two `require()` results identical), `VERSION 1.93.0`, `KEYS version` (sorted `Object.keys`, so later tasks extend this line: it is the guard that hooks are absent without the variable). stderr: exactly one `imgui: addon loaded (imgui 1.93.0)`, checked in a FileCheck run of its own as a positive line followed by a `-NOT` of the same pattern (this FileCheck has no `-COUNT`); mutation: trace it twice, the run must fail. A second RUN with plain `%hermes-node`: `require` throws, prints `PLAIN MODULE_NOT_FOUND`.
  - `test/imgui-missing.js` (`// REQUIRES: ui`): copies `%hermes-node-ui` to `%t.dir/hermes-node-ui` and runs a script there that calls `require.resolve` then `require`, twice each, printing each error's `code` and whether its message contains `%t.dir/hermes-node-imgui.node`: four lines `MISSING MODULE_NOT_FOUND path=true`. Then writes ten bytes of text to `%t.dir/hermes-node-imgui.node` and reruns: `UNLOADABLE ERR_DLOPEN_FAILED`.
  - `test/ui-imgui-feature.js` (`// REQUIRES: ui`), modelled on `test/ui-sokol-validation-feature.js`: runs `probe.js` itself and requires that its answer agrees with `%ui-imgui-state`, and that the state is `on`.
- [ ] **Step 3: `test/lit.cfg`:** when `ui` is available, run `[hermes_node_ui, <source>/test/fixtures/imgui/probe.js]` with a 60 s limit; add `ui-imgui` when it exits 0 and stdout starts `imgui ok `. Register `%ui-imgui-state` before any shorter prefix it could collide with.
- [ ] **Step 4: Run the three tests; expect FAIL / UNSUPPORTED** (`ui-imgui-feature.js` must be red, not unsupported).
- [ ] **Step 5: Implement** `named_addons`, the registration in `main.cpp` (return 1 if it fails, like the `ui` registration), the CMake target, `imgui_config.h`, `imgui_addon.h` with the block from "File Structure", and `imgui_module.cpp`.
- [ ] **Step 5b: Write `test/imgui-symbols.js`** (`// REQUIRES: ui, ui-imgui`; needs lit param `hermes_node_imgui=$<TARGET_FILE:hermes_node_imgui>`, registered as `%hermes_node_imgui` with `_register_path_param`): runs `nm -g` on the addon (`nm -gU` on darwin, `nm -g --defined-only` elsewhere) through `%python` or `child_process`, and prints `EXPORTS ok` when the defined external symbols are exactly the N-API registration symbol(s) and nothing containing `ImGui` or `Im` + an uppercase letter; otherwise prints the offenders. Mutation: remove the visibility properties; it must fail.
- [ ] **Step 6: Build; run the tests; expect PASS.** Run `utils/check-sgp-isolation.py`'s test and `test/ui-stderr-rule.js`; expect PASS.
- [ ] **Step 7: Mutations.** (a) Remove the `stat` check: `imgui-missing.js` must fail on `MODULE_NOT_FOUND`. (b) Remove the `registerNamedAddons` call: `ui-imgui-feature.js` must be red and `imgui-load.js` unsupported. (c) Drop `HNUI_CHECK_VERSION`: add to `imgui-load.js` nothing; instead confirm by reading that the macro is the first statement (no test can reach a mismatched host here; say so in the progress file).
- [ ] **Step 8: Commit:** `Load hermes-node:imgui from a .node file beside the binary`.

### Task 4: The frame protocol, and a cleared target

**Files:**
- Create: `addons/imgui/imgui_platform.cpp`, `addons/imgui/imgui_renderer.cpp`, `addons/imgui/imgui_test_hooks.cpp`, `test/fixtures/imgui/harness.js`, `test/ui-imgui-frame.js`
- Modify: `addons/imgui/imgui_module.cpp`, `addons/imgui/CMakeLists.txt`, `test/imgui-load.js` (the `KEYS` line)

**Interfaces:**
- Produces: exports `newFrame() -> boolean`, `render() -> boolean`, `showDemoWindow() -> undefined`; with test hooks, `__test.scene(name)` (this task: `'none'`, `'fill'`) and `__test.state()` returning at least `{frameOpen, displayWidth, displayHeight, scaleX, scaleY, targetGeneration, targetWidth, targetHeight, commands, vertices, indices, liveTextures}` (the three `target*` fields are those of the `hnui_target` the last drawing `render()` received; 0 before any).
- State machine in `imgui_module.cpp`: `frameOpen` (bool). `newFrame()`: no window (`hnui_get_window(env) == NULL`) returns false; first success creates the context (`ImGui::CreateContext`, `io.IniFilename = nullptr`, `io.LogFilename = nullptr`, `ConfigFlags |= ImGuiConfigFlags_DockingEnable`, `platformInit`, `rendererInit`) and traces `imgui: context created (imgui <v>, docking=1)`; if `frameOpen`, `ImGui::EndFrame()` and trace `imgui: frame discarded`; then `platformNewFrame`, `ImGui::NewFrame()`, `frameOpen = true`, return true.
- `render()`: not `frameOpen` throws; `ImGui::Render()`, `frameOpen = false`; ask `hnui_frame_target` (`target.size` set); false returns false; otherwise `rendererDraw`, `platformAfterRender`, return true.
- Errors: `Error` with `code = 'ERR_IMGUI_NO_FRAME'`, message `imgui.<name>(): no frame is open; call imgui.newFrame() first`.
- `rendererDraw` in this task: one pass on `target.color_view` exactly as `test/fixtures/hnui-demo/demo_visual.c:197-247` begins it, `SG_LOADACTION_CLEAR` to `{26.0f / 255.0f, 26.0f / 255.0f, 31.0f / 255.0f, 1.0f}`, `sg_end_pass()`, no draws. It records `rendererStats()` from `ImDrawData`.
- `platformNewFrame`: `hnui_get_window_state`; `io.DisplaySize = (width, height)`; `io.DisplayFramebufferScale = (framebuffer_width / width, framebuffer_height / height)` (1 when width or height is 0); `io.DeltaTime` from `std::chrono::steady_clock`, 1/60 on the first frame, clamped to at least 1e-5.
- `rendererInit` must set `ImGuiBackendFlags_RendererHasTextures` already (ImGui 1.93 asserts in `NewFrame` on a backend with neither that flag nor a built atlas). Texture requests are not answered until Task 5 and `rendererDraw` issues no draw and never calls `ImDrawCmd::GetTexID()` (it asserts on a texture not yet uploaded), so **no Task 4 test draws anything**: `fill` is added here and first captured in Task 5.
- `__test.scene('fill')`: `ImGui::GetBackgroundDrawList()->AddRectFilled((0,0), DisplaySize, IM_COL32(255,0,0,255))` with `ImDrawListFlags_AntiAliasedFill` cleared on that list for the call.

- [ ] **Step 0: The host hook prerequisite** described under `resync(h)` in "File Structure": change, test in `test/ui-present.js`, mutation (return 0 again: the new check fails), commit `Return the presenting publication from an unmatched __captureFrame`.
- [ ] **Step 1: Write `harness.js`** as specified under "File Structure".
- [ ] **Step 2: Write `test/ui-imgui-frame.js`.** Cases, each printing one line:
  - Before `createWindow`: `NOWINDOW newFrame=false`, then `render()` throws: `NOWINDOW render=ERR_IMGUI_NO_FRAME` (Review Focus 1).
  - `render()` and `showDemoWindow()` with no frame open, in a frame callback: `MISUSE render=ERR_IMGUI_NO_FRAME showDemoWindow=ERR_IMGUI_NO_FRAME`.
  - `newFrame()` outside a frame callback then `render()`: `OUTSIDE render=false`.
  - One empty frame captured: every sample of a 5-point set equals `CLEAR`: `CLEARED ok`.
  - `newFrame(); newFrame(); render()` in one callback: `TWICE render=true`; stderr has `imgui: frame discarded`.
  - A throw between `newFrame()` and `render()`, with an `uncaughtException` listener installed and the next `requestAnimationFrame` registered before the throw: the listener prints `CAUGHT`; the next frame prints `RECOVERED render=true`; stderr has, in order, `frame tick: 1 callback(s) ran, nothing drawn`, `imgui: frame discarded`, and later `frame tick: 1 callback(s) ran, drew`.
  - `state()` after a frame: `DISPLAY 640x480 scale=<win.dpiScale>` compared against `win.width`, `win.height`, `win.framebufferWidth / win.width`: prints `DISPLAY ok`.
  - Exit: an `'exit'` listener calls `newFrame()` and `render()` and prints `EXIT newFrame=true render=false`; the script ends with `process.exit(7)` from inside a frame callback, between `newFrame()` and `render()`; `EXITCODE 7`.
  - stderr also has `imgui: context created (imgui 1.93.0, docking=1)` once (positive line then `-NOT`, in its own FileCheck run).
  Update `imgui-load.js`: `KEYS newFrame,render,showDemoWindow,version`, and a third RUN with `HERMES_NODE_UI_TEST_HOOKS=1` printing `KEYS __test,newFrame,render,showDemoWindow,version`.
- [ ] **Step 3: Run; expect FAIL.**
- [ ] **Step 4: Implement** the interfaces above. `showDemoWindow()` calls `ImGui::ShowDemoWindow()` (nothing draws until Task 5; no test calls it with a frame open before Task 10).
- [ ] **Step 5: Run; expect PASS**, plus `test/ui-stderr-rule.js`.
- [ ] **Step 6: Mutations.** (a) Take the target in `newFrame()` instead of `render()`: the throw case must fail (`nothing drawn` missing). (b) Clear to black: `CLEARED` fails. (c) Remove the `EndFrame()` on a second `newFrame()`: ImGui asserts or `TWICE` fails. (d) Make `render()` throw when the target is unavailable: `OUTSIDE` and `EXIT` fail.
- [ ] **Step 7: Commit:** `Add the ImGui frame protocol: newFrame() and render()`.

### Task 5: The renderer's draw path

**Files:**
- Modify: `addons/imgui/imgui_renderer.cpp`, `addons/imgui/imgui_test_hooks.cpp`
- Create: `test/ui-imgui-draw.js`

**Interfaces:**
- Consumes: Task 4's `rendererDraw` shell and harness.
- Produces: untextured-capable drawing of every `ImDrawCmd`; scenes `clip`, `many`, `two` (below); `__test.scene(name, arg)` takes an optional second argument.

**Decisions:**
- Shader: attributes `position` (`vec2`), `texcoord0` (`vec2`), `color0` (`vec4` from `SG_VERTEXFORMAT_UBYTE4N`); one vertex uniform block holding a `mat4` named `proj` (GL uniform name `proj`, `msl_buffer_n = 0`); one fragment texture + sampler pair (`glsl_name = "tex_smp"`, `msl_texture_n = 0`, `msl_sampler_n = 0`); fragment output `color * texture(tex_smp, uv)`. GLSL `#version 410`, modelled on `imgui_impl_opengl3`'s 410 shader; MSL source modelled on `imgui_impl_metal.mm`'s, entry points `vs_main` / `fs_main`. Choose by `sg_query_backend()`: `SG_BACKEND_GLCORE` or `SG_BACKEND_METAL_MACOS`; anything else: one line on stderr and `_exit` is wrong for an addon, so throw nothing and draw nothing, tracing `imgui: unsupported sokol backend <n>` once.
- Projection: orthographic over `DisplayPos .. DisplayPos + DisplaySize`, the matrix of `imgui_impl_opengl3`. On a backend with `sg_query_features().origin_top_left` false (GL), the target is a texture whose row 0 is the bottom: check against `demo_visual.c` how it orients its triangle, and orient so that captured row 0 is ImGui's y = 0. The `clip` test is the proof.
- **The texture protocol is implemented in this task**, because even a flat fill on the background draw list carries the font atlas's texture reference and `ImDrawCmd::GetTexID()` asserts until that texture is uploaded:
  - At the top of `rendererDraw`, before the pass: for each `ImTextureData *tex` in `*dd->Textures` with `Status != ImTextureStatus_OK`:
    - `WantCreate` or `WantUpdates`: require `Format == ImTextureFormat_RGBA32`; `sg_make_image` (immutable, `SG_PIXELFORMAT_RGBA8`, `data.mip_levels[0] = {tex->GetPixels(), tex->GetSizeInBytes()}`), `sg_make_view` (`.texture.image`); if both are `SG_RESOURCESTATE_VALID`: destroy the previous view and image if any, store both ids in a heap record hung on `tex->BackendUserData`, `tex->SetTexID(view.id)`, `tex->SetStatus(ImTextureStatus_OK)`, trace `imgui: texture <UniqueID> created <w>x<h>` or `... replaced <w>x<h>`. If not valid: destroy what was made, leave the status alone, trace `imgui: texture <UniqueID> failed`.
    - `WantDestroy` with `UnusedFrames > 0`: destroy view and image, free the record, `SetTexID(ImTextureID_Invalid)`, `BackendUserData = nullptr`, `SetStatus(ImTextureStatus_Destroyed)`, trace `imgui: texture <UniqueID> destroyed`.
  - `rendererInit` sets `platform_io.Renderer_TextureMaxWidth/Height` to 8192 and creates one sampler: linear min/mag, clamp to edge.
  - A draw command binds `views[0].id = (uint32_t)cmd.GetTexID()` (the white fallback for an invalid id) and the sampler.
- A draw command whose texture is still not uploaded (creation failed: `cmd.TexRef._TexData` non-null with an invalid `TexID`) is skipped without calling `GetTexID()`. A command with a raw invalid id (`_TexData` null, `_TexID` invalid) binds a 1x1 white immutable image and view made in `rendererInit`.
- Pipeline: `SG_INDEXTYPE_UINT16`, layout from `offsetof(ImDrawVert, ...)`, blend `SRC_ALPHA / ONE_MINUS_SRC_ALPHA` for colour and `ONE / ONE_MINUS_SRC_ALPHA` for alpha, no cull, no depth (`depth.pixel_format = SG_PIXELFORMAT_NONE`), `colors[0].pixel_format = target.color_format`, `sample_count = target.sample_count`. Rebuilt, and traced `imgui: pipeline created (format <n>, samples <n>)`, when either differs from the last target's.
- Buffers: created with `usage.vertex_buffer` / `usage.index_buffer` and `usage.dynamic_update`; initial capacities 16,384 vertices and 32,768 indices. Per `rendererDraw`, for each draw list: `sg_query_buffer_will_overflow`; on overflow create a buffer of at least twice the size and at least the needed size, destroy the old one, trace `imgui: vertex buffer grown to <bytes> bytes` (or `index buffer`), then `sg_append_buffer` and keep the returned byte offsets.
- One function, `setupRenderState(dd, target)`, applies the pipeline, `sg_apply_viewport(0, 0, fbWidth, fbHeight, true)` and the projection uniforms. It runs after `sg_begin_pass` and again for a reset callback.
- Draw: for each command, user callbacks per the spec (`platform_io.DrawCallback_ResetRenderState` is set to a static no-op function; a command whose callback is that function runs `setupRenderState`, any other callback is called); clip rectangle math of `imgui_impl_metal.mm` (scaled by `FramebufferScale`, clamped to the framebuffer, empty skipped, `ElemCount == 0` skipped); `sg_apply_scissor_rectf(..., true)`; bindings with `vertex_buffer_offsets[0] = listVertexByteOffset`, `index_buffer_offset = listIndexByteOffset`; `sg_draw_ex(cmd.IdxOffset, cmd.ElemCount, 1, cmd.VtxOffset, 0)`. Set `ImGuiBackendFlags_RendererHasVtxOffset`. Both backends report `draw_base_vertex` at this pin. Check it once in `rendererInit`; if it is false the renderer is unusable (ImGui does not split lists by itself): trace `imgui: sokol backend lacks draw_base_vertex` once, to stderr unconditionally, and draw nothing.
- Scenes (all on the background draw list, anti-aliased fill off, opaque pure colours):
  - `clip`: `PushClipRect((100,100),(300,200), true)`, a green fill of the whole display, `PopClipRect`.
  - `many`: 4x4-unit quads tiling the 640x480 display, 160 columns by 120 rows, 19,200 quads; quad (c, r) is red, green or blue for `(c + r) % 3` = 0, 1, 2.
  - `two`: `scene('two', 0)` fills red, `scene('two', 1)` fills blue.
  - `callback`: `AddCallback` of a function that calls `sg_apply_viewport(0, 0, fbWidth / 4, fbHeight / 4, true)`, then `AddCallback(platform_io.DrawCallback_ResetRenderState, nullptr)`, then a full-display green fill.

- [ ] **Step 1: Write `test/ui-imgui-draw.js`** (window 640x480; sample points multiplied by `win.dpiScale`).
  - `fill`: five points all `[255,0,0]`: `FILL ok`.
  - `clip`: (200,150) is `[0,255,0]`; (50,50), (350,150), (200,250) are `CLEAR`; (101,101) green and (99,99) clear: `CLIP ok`.
  - `many`: centres of quads (0,0), (80,60), (159,119), (1,0), (2,0) have the colours the rule gives: `MANY ok`; `state()` reports `vertices` 76800 and `commands` at least 2: `MANY vertices=76800 split=true`; stderr has `imgui: vertex buffer grown to` and `imgui: index buffer grown to`.
  - Two pairs in one callback (`two` 0 then `two` 1): captured frame is blue: `TWOPAIRS ok`. Two callbacks in one tick (`captureTick`), the first drawing `two` 1 and the second `two` 0: red: `TWOCALLBACKS ok`. Both cases advance `h.published` by one, which the following capture proves.
  - `many` drawn in two pairs in one callback after growth: `MANYTWICE ok` (appends twice into one Sokol frame).
  - `callback`: (600,440), (320,240) and (20,20) are all green: `CALLBACK ok`.
  - stderr: one `imgui: texture <n> created` (the atlas).
  - stderr: `imgui: pipeline created (format` exactly once (positive line then `-NOT`, in its own FileCheck run).
- [ ] **Step 2: Run; expect FAIL.**
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run; expect PASS** in `cmake-build-asan` (Debug: Sokol validation on; check that `sokol-validation` is among lit's features).
- [ ] **Step 5: Mutations.** (a) Use `sg_update_buffer`: the two-pair cases must produce a `[sokol-` line. (b) Pass 0 as base vertex: `MANY` fails. (c) Skip the scissor: `CLIP` fails. (d) Flip the projection's y: `CLIP` fails. (e) Start the buffers at 1,000,000 vertices: the growth trace check fails. (f) Make the reset callback re-apply only the pipeline: `CALLBACK` fails at (600,440).
- [ ] **Step 6: Commit:** `Draw ImGui's command lists and textures with sokol_gfx`.

### Task 6: Texture scenes and tests

**Files:**
- Modify: `addons/imgui/imgui_renderer.cpp`, `addons/imgui/imgui_test_hooks.cpp`
- Create: `test/ui-imgui-textures.js`

**Interfaces:**
- Produces: scenes `quad`, `glyphs`. (`state().liveTextures` and the protocol exist since Task 5.)

**Decisions:**
- The texture protocol itself is Task 5's; this task adds the scenes and tests that prove it.
- Scenes: `quad`: an addon-owned 2x2 immutable image (red, green / blue, white; nearest sampling is not available, so draw it with `AddImage` over (100,100)-(300,300) and sample only at the four corners' insides: (105,105), (295,105), (105,295), (295,295)). Its view id is passed to `AddImage` as an `ImTextureID`. `glyphs`: `scene('glyphs', 0)` draws `AddText` at (20,20) of the string `abc` in white at the default size; `scene('glyphs', 1)` draws that and, at (20,200), `ImGui::PushFont(nullptr, 40.0f)`, the string `XYZ`, `PopFont()` (check the 1.93 `PushFont` signature in `imgui.h`).

- [ ] **Step 1: Write `test/ui-imgui-textures.js`.**
  - `quad`: the four samples are red, green, blue, white exactly: `QUAD ok`.
  - `glyphs` 0, captured: the box (20,20)-(60,40) contains at least one pixel that is not `CLEAR`, and the box (20,200)-(200,260) is all `CLEAR`: `GLYPHS0 ok`. stderr: one `imgui: texture <n> created`.
  - `glyphs` 1 for 3 frames then captured: both boxes contain non-`CLEAR` pixels: `GLYPHS1 ok`. stderr after the first line: at least one `imgui: texture <n> replaced` (or a second `created` if ImGui chose a new texture; accept either with a regex, and print which in the progress file).
  - `state().liveTextures` is at least 1: `LIVE ok`.
- [ ] **Step 2: Run; expect FAIL.**
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run; expect PASS.**
- [ ] **Step 5: Mutations.** (a) Ignore `WantUpdates`: `GLYPHS1` fails (ImGui also asserts). (b) Always bind the white fallback: `QUAD` fails (the glyph cases do not: a white glyph quad still leaves clear pixels around it). (c) Do not destroy the previous image on replace: no test fails; say so in the progress file and check by reading (Sokol's pool would exhaust only after thousands).
- [ ] **Step 6: Commit:** `Test ImGui textures: an image and a growing font atlas`.

### Task 7: Input

**Files:**
- Modify: `addons/imgui/imgui_platform.cpp`, `addons/imgui/imgui_test_hooks.cpp`
- Create: `test/ui-imgui-input.js`

**Interfaces:**
- Produces: the event listener; scene `widgets`; `state()` gains `mouseX, mouseY, mouseDown` (array of 3 booleans, ImGui numbering), `wantCaptureMouse, wantCaptureKeyboard, wantTextInput, clicks, keyAPresses, keyADown, text, wheel`.

**Decisions:**
- `platformInit` registers one listener with `hnui_add_event_listener` for all events and sets `io.BackendPlatformName = "hermes-node hnui"`.
- Per event, first the four modifier events from `event->modifiers` (`io.AddKeyEvent(ImGuiMod_Ctrl, ...)`, `Shift`, `Alt`, `Super` for `HNUI_MOD_META`), for key and mouse-button events.
- `MOUSE_MOVE`, and the position of `MOUSE_DOWN`/`MOUSE_UP`: `AddMousePosEvent(x, y)`. Buttons: `hnui` 0, 1, 2 to ImGui 0, 2, 1; other values ignored. `MOUSE_WHEEL`: `AddMouseWheelEvent(dx, dy)`. `MOUSE_LEAVE`: `AddMousePosEvent(-FLT_MAX, -FLT_MAX)`. `FOCUS` / `BLUR`: `AddFocusEvent(true / false)`.
- `KEY_DOWN` with `repeat != 0` is dropped. Otherwise the key is translated by a table copied from `ImGui_ImplGlfw_KeyToImGuiKey` (GLFW numbering is `hnui`'s); `ImGuiKey_None` is dropped. `KEY_UP` likewise.
- `TEXT_INPUT`: `AddInputCharactersUTF8(event->text)`, repeats included.
- `PASTE`, `RESIZE`, `CLOSE_REQUESTED`, `FILE_DROP`, `MINIMIZE`, `RESTORE`, `MOUSE_ENTER`: ignored.
- Scene `widgets`: `SetNextWindowPos((50,50))`, `SetNextWindowSize((300,200))`, both `ImGuiCond_Always`; `Begin("T", nullptr, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse)`; `if (Button("B", (100,30))) clicks++`; `InputText("##t", buf, 256)`; `End()`. It records the screen rectangles of the button and the field (`GetItemRectMin/Max`) into `state()` as `buttonRect` and `fieldRect` (`[x0,y0,x1,y1]`), and each frame adds to `keyAPresses` when `IsKeyPressed(ImGuiKey_A, false)`, sets `keyADown = IsKeyDown(ImGuiKey_A)`, accumulates `io.MouseWheel` into `wheel`.

Events are injected with `ui.__injectEvent` in Sokol's terms (`mouseX`/`mouseY` in framebuffer pixels, so multiply by `win.dpiScale`; buttons 0 left, 1 right, 2 middle; `CHAR` with `charCode`); the harness gains `inject(h, desc)`. Every expectation is reached through `until(...)`.

- [ ] **Step 1: Write `test/ui-imgui-input.js`,** drawing `widgets` every frame:
  - Move to the button's centre: `HOVER wantCaptureMouse=true`; move to (600,440): `AWAY wantCaptureMouse=false`.
  - Move to the button, left down, left up: `CLICK clicks=1`.
  - Right down: `RIGHT mouseDown=false,true,false`; up; middle down: `MIDDLE mouseDown=false,false,true`; up.
  - Wheel `scrollY: 1` over the window: `WHEEL 1`.
  - Click the field: `FIELD wantTextInput=true`. `CHAR` h, i: `TEXT hi`. Left arrow key, `CHAR` X: `TEXT hXi`. Backspace: `TEXT hi`.
  - End key, then `CHAR` 0x1F600: the text is `hi` followed by that code point (compare with `Array.from(text)`): `ASTRAL ok` (Review Focus 3).
  - With the field unfocused (Escape, then click at (600,440)): `KEY_DOWN` A, `KEY_DOWN` A with `repeat: true`, three frames, `KEY_UP` A: `APRESS 1`. A repeated `CHAR` into a focused field adds a character: `REPEATCHAR ok`.
  - `KEY_DOWN` with `keyCode` 0 and 999, then `KEY_DOWN`/`KEY_UP` A: `UNKNOWN ok apress=1` (Review Focus 4).
  - `KEY_DOWN` A and left down; `until` both read back held (`HELD keyADown=true mouseDown=true,false,false`); only then `UNFOCUSED`, and `until` both clear: `BLUR keyADown=false mouseDown=false,false,false` (Review Focus 5); then `FOCUSED`.
  - **Every `keyAPresses` expectation in this test is a delta** from a baseline sampled just before the case's first injection, never a running total: the lines print the delta (`APRESS 1`, `UNKNOWN ok apress=1`, `REAL apress=1`).
  - `MOUSE_LEAVE`: `LEAVE wantCaptureMouse=false`, `mouseX` below -1e30.
  - One real key through `test/fixtures/hnui-demo/window_driver.js` (`key a`, with `%x11_window_driver` on the RUN line as `test/ui-real-events.js` passes it): `REAL apress=1`.
- [ ] **Step 2: Run; expect FAIL.**
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run; expect PASS.**
- [ ] **Step 5: Mutations.** (a) Pass buttons through unrenumbered: `RIGHT`/`MIDDLE` fail. (b) Pass repeated key-downs on: check whether `APRESS` fails; if ImGui itself ignores a down for a key already down, the drop is redundant: keep it (the spec states it), and record that the test cannot distinguish. (c) Remove the listener registration: every case fails, `REAL` included. (d) Drop `IMGUI_USE_WCHAR32`: `ASTRAL` fails. (e) Skip the modifier events: no case here fails; Task 8's chords do.
- [ ] **Step 6: Commit:** `Feed hnui input events to ImGui`.

### Task 8: Cursor and clipboard

**Files:**
- Modify: `addons/imgui/imgui_platform.cpp`, `addons/imgui/imgui_test_hooks.cpp`
- Create: `test/ui-imgui-clipboard.js`, `test/ui-imgui-cursor.js`

**Interfaces:**
- Produces: `state().cursor` (the `ImGuiMouseCursor` last acted on); `lastCursorRequested()`.

**Decisions:**
- `platformInit` sets `ImGuiBackendFlags_HasMouseCursors` and the two `platform_io` clipboard functions.
- `platformAfterRender`: skip if `io.ConfigFlags & ImGuiConfigFlags_NoMouseCursorChange`. Two cached values, compared separately: **visibility** (`hidden = c == ImGuiMouseCursor_None || io.MouseDrawCursor`, where `c = ImGui::GetMouseCursor()`) and **shape**. When `hidden` changes: `hnui_show_cursor(w, !hidden)` and trace `imgui: cursor hidden` or `imgui: cursor shown`. When not hidden and the shape changed: `hnui_set_cursor` with: Arrow→`ARROW`, TextInput→`IBEAM`, ResizeAll→`RESIZE_ALL`, ResizeNS→`RESIZE_NS`, ResizeEW→`RESIZE_EW`, ResizeNESW→`RESIZE_NESW`, ResizeNWSE→`RESIZE_NWSE`, Hand→`POINTING_HAND`, NotAllowed→`NOT_ALLOWED`, Wait and Progress→`ARROW`; trace `imgui: cursor <ImGui value> -> hnui <value>`.
- Set: trace `imgui: clipboard write requested, <n> bytes`, then `hnui_write_clipboard(w, text)`. Every cursor and clipboard trace precedes its `hnui_*` call, so it is ordered before the host's trace of applying the command.
- Get: `hnui_read_clipboard_sync(w, 500, &out)`; `HNUI_OK`: copy into a `static std::string`, `hnui_free(out)`, trace `imgui: clipboard get: ok, <n> bytes`, return its `c_str()`; anything else: trace `imgui: clipboard get: status <n>`, return `nullptr`.
- Chords are injected with `SAPP_MODIFIER_CTRL` on Linux and `SAPP_MODIFIER_SUPER` on macOS (ImGui's shortcut modifier follows `ConfigMacOSXBehaviors`); take the bit values from `test/ui-events.js`.

- [ ] **Step 1: Write `test/ui-imgui-clipboard.js`.** It carries the pasteboard guard of `test/ui-commands.js:138-146` verbatim, and that test's backend argument and prefixes for the host's per-backend trace. Cases, drawing `widgets`:
  - First, with nothing ever copied in this run: focus the field, type `q`, paste chord: `EMPTYPASTE text=q`; stderr `imgui: clipboard get: status` (Review Focus 2). (On X11 under a fresh Xvfb no selection owner exists; on macOS the run's pasteboard is new.)
  - Clear the field (select-all chord, Backspace), type `hello`, select-all, copy chord, End key, paste chord: `PASTE text=hellohello`. stderr: `imgui: clipboard write requested, 5 bytes`, the host's write read-back line (`TRACE-gl: command clipboard: owns_selection=1 bytes=5`, `TRACE-metal: command clipboard: pasteboard_bytes=5 bytes=5`), `imgui: clipboard get: ok, 5 bytes`.
  - Cut chord after select-all: `CUT text=`, then paste: `CUTPASTE text=hellohello`.
- [ ] **Step 2: Write `test/ui-imgui-cursor.js`.** Drawing `widgets`: move over the field: `CURSOR 1` (`ImGuiMouseCursor_TextInput`); stderr `imgui: cursor 1 -> hnui 2` and the host's `command cursor 2: sapp_mouse_cursor=2`. Move to (600,440): `CURSOR 0`; `command cursor 1: sapp_mouse_cursor=1`. A third phase sets `ImGui::GetIO().MouseDrawCursor` through `scene('widgets', 'softcursor')`: stderr `imgui: cursor hidden` and `command show cursor: sapp_mouse_shown=0`; a fourth phase turns it off again (`scene('widgets', 'hardcursor')`): `imgui: cursor shown` and `command show cursor: sapp_mouse_shown=1`. The pointer stays at (600,440) through both, so the shape does not change: that is the case a single cache gets wrong.
- [ ] **Step 3: Run both; expect FAIL.**
- [ ] **Step 4: Implement.**
- [ ] **Step 5: Run both; expect PASS**, and `test/ui-stderr-rule.js`.
- [ ] **Step 6: Mutations.** (a) Return `""` from get on failure and skip the trace: `EMPTYPASTE`'s stderr check fails. (b) Do not `hnui_write_clipboard`: `PASTE` fails. (c) Skip modifier events in the listener: `PASTE` fails. (d) Map TextInput to `ARROW`: the cursor test fails on both lines. (e) Handle `HNUI_EVENT_PASTE` by inserting its text as well: check whether `PASTE` shows the text three times; record the result.
- [ ] **Step 7: Commit:** `Give ImGui the cursor and the clipboard`.

### Task 9: Resize and HiDPI

**Files:**
- Create: `test/ui-imgui-resize.js`
- Modify: whatever the test shows wrong (expected: nothing)

- [ ] **Step 1: Write `test/ui-imgui-resize.js`.**
  - Run 1 (`highDpi` unset): draw `glyphs` 0 and `fill`-free frames; capture one frame and require it matched; install the `resize` listener; `driver.send('resize 800 600')` through `window_driver.js`; wait for the `resize` event, yield one `setImmediate`, then `until` `state().displayWidth === 800`: `RESIZED 800x600`; set `h.generation` from `state().targetGeneration` (print `GENERATION 2`) and `resync(h)`; `liveTextures` unchanged across it: `TEXTURES same`; capture a `fill` frame at the new size: `FILL800 ok` at points up to (790,590). stderr: `generation 2: started at`, no second `imgui: pipeline created`, no second `imgui: texture <n> created`.
  - Run 2 (`open({highDpi: true})`): `SCALE ok` when `state().scaleX === win.framebufferWidth / win.width`; a `clip` capture sampled at logical points times `win.dpiScale`: `CLIP ok`; `widgets` with a click on the button at logical coordinates: `CLICK clicks=1`. On a 1x display this repeats run 1's geometry; on the Mac it is the 2x proof. Print `SCALEVALUE <n>` to stdout, unchecked, so a Mac run's log shows 2.
- [ ] **Step 2: Run; expect PASS** if Tasks 4-8 are right. If it fails, fix the cause in the unit that owns it (a wrong fix here would be scaling the style or the font: the spec forbids both).
- [ ] **Step 3: Mutations.** (a) Hard-code `DisplayFramebufferScale` to 1: run 2 cannot fail under Xvfb; record that only the Mac can see it. (b) Cache `DisplaySize` at context creation: `RESIZED` fails.
- [ ] **Step 4: Commit:** `Test the ImGui addon across a resize and with highDpi`.

### Task 10: The demo window and the example

**Files:**
- Create: `examples/imgui-demo/demo.js`, `examples/imgui-demo/README.md`, `test/ui-imgui-demo.js`
- Modify: `examples/README.md` (one entry)

- [ ] **Step 1: Write `test/ui-imgui-demo.js`.** Window 1280x800. 30 frames of `newFrame(); showDemoWindow(); render()`, then one captured. The demo window's first-use rectangle is (650,20)-(1200,700). Assertions: at least 1,000 pixels inside (660,30)-(1190,690) differ from `CLEAR`; every pixel in the column x = 300 and in the row y = 760 equals `CLEAR`: `DEMO ok`. `state().vertices > 500`: `DEMO vertices ok`. Then inject a click on the demo window's first collapsing header (take a point from a captured-pixel scan is fragile; instead inject `MOUSE_SCROLL` over the window and three more frames) and print `DEMO scrolled`: the point is that input over a real, large window raises no Sokol line. `process.exit(0)`.
- [ ] **Step 2: Run; expect PASS** (everything it needs exists). If ImGui asserts inside `ShowDemoWindow`, the assertion text names the missing backend piece; fix it in the owning unit.
- [ ] **Step 3: Write `examples/imgui-demo/demo.js`**: the program in the spec's "JS surface", plus a `close` listener is not needed (an uncancelled close ends the process). **README:** what it shows, the command (`<build dir>/bin/hermes-node-ui examples/imgui-demo/demo.js`), that it needs a build with `-DHERMES_NODE_ENABLE_UI=ON`, and that it is not part of `check-hermes-node-examples`.
- [ ] **Step 4: Run the example by hand under the wrapper** to see it start and draw: `python3 utils/with-display.py 20 cmake-build-asan/bin/hermes-node-ui examples/imgui-demo/demo.js; echo $?` and expect 124 (it runs until the time limit) with no `[sapp:error` and no `[sokol-` line on stderr (a `[sapp:warning` about DPI is the healthy Xvfb line).
- [ ] **Step 5: Mutation.** Skip every `sg_draw_ex`: `DEMO ok` fails while `DEMO vertices ok` still passes.
- [ ] **Step 6: Commit:** `Show ImGui's demo window from a JS example`.

### Task 11: Documentation and the full suite

**Files:**
- Create: `addons/imgui/README.md`
- Modify: `CLAUDE.md`, `docs/README.md` if it indexes plans/specs, `docs/superpowers/plans/progress-imgui-addon.md`, `docs/superpowers/specs/2026-09-18-ui-compositor-design.md` (its "Not in v1: an ImGui addon" passages gain one sentence pointing at the new spec)

- [ ] **Step 1: `addons/imgui/README.md`:** what the addon is and is not (spec: Intent, Non-goals), how it is found, the JS surface with every rule from the spec's "JS surface", the trace lines, the test hooks, how to bump ImGui.
- [ ] **Step 2: `CLAUDE.md`:** in Conventions, `addons/<name>/` for product addons built by the main CMake; under the UI compositor section, a short subsection "Dear ImGui addon (`hermes-node:imgui`)" stating: loaded by name from a `.node` beside the binary and why it is not linked in; public API only; `render()` takes the target; append-only buffers and replaced textures and why; logical units; the `ui-imgui` feature and its forcing test; test names `ui-imgui-*.js`. Describe what is, not the history.
- [ ] **Step 3: Full verification on Linux.** `./utils/format.sh -f`; `cmake --build cmake-build-asan --target check-hermes-node`; then configure-and-build check that a build with the UI off is unaffected: `cmake --build cmake-build-asan-noui --target check-hermes-node`. Record the three result lines in the progress file. A red run naming only `test-inspect.js` or `test-repl-history.js` is the known flake: rerun that test alone six times before treating it as real.
- [ ] **Step 4: Tracker.** `dz list`; file an issue for each thing recorded as unverifiable during the build (for example: the HiDPI mutation only the Mac can see, until the Mac round has run), and for any `hnui` gap not fixed. Stage `dz/` with this commit.
- [ ] **Step 5: Commit:** `Document the ImGui addon`.

### Task 12 (controller): the Mac

Not an implementer task. The controller pushes the branch to the channel branch and asks the Mac agent, through the gist channel, to build `cmake-build-asan-ui` and run: every `ui-imgui-*.js` and `imgui-*.js` test with `-a` output for `ui-imgui-draw.js`, `ui-imgui-textures.js` and `ui-imgui-resize.js`; then the whole suite. Specific things the reply must report: whether Sokol compiled the MSL source (any `[sokol-` line verbatim), `SCALEVALUE`, and mutation 9(a) run there (hard-code the scale to 1; `CLIP`/`CLICK` in run 2 must fail). Failures are fixed by implementer subagents on Linux and retested there.

Because nothing in the repository has compiled MSL from source before, the controller runs a first, smaller round as soon as Task 5 is committed (build, `ui-imgui-frame.js`, `ui-imgui-draw.js`), without waiting for the remaining tasks.
