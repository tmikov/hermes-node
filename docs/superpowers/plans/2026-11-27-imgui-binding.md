# Dear ImGui Addon, Part B: the JS Binding and the Application. Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `hermes-node:imgui` exposes about 150 ImGui functions to JavaScript under one calling convention, and `examples/imgui-app/` is a docked, Node-backed application written against them, on GL/X11 and Metal.

**Architecture:** Each bound function is a short C++ N-API function in `addons/imgui/bind/`, built from shared argument readers in `bind_util.h` in one fixed shape (enter, read and check arguments, check frame and preconditions, call ImGui, check for a usage error, write back). `bind_guards.cpp` is the only unit that includes `imgui_internal.h`: it holds ImGui's error callback and the reads of ImGui state that turn plain assertions into `ERR_IMGUI_USAGE`. The application is plain CommonJS with one file per panel.

**Tech Stack:** C++17, Dear ImGui `v1.93.0-docking` (vendored), Node-API, CMake + Ninja, lit + FileCheck, the part A test harness.

**Spec:** `docs/superpowers/specs/2026-11-27-imgui-binding-design.md`. Read it whole before any task; this plan does not repeat its reasoning.

**Surface table:** `docs/superpowers/plans/2026-11-27-imgui-binding-surface.md` is part of this plan. It has one entry per bound function: the JavaScript signature, the C++ declaration with its `imgui.h` line, the metadata, and the preconditions with the source lines they come from. A task that says "bind group X" means: implement every entry of that section exactly as written there. Where an entry and ImGui's source disagree, the source wins: fix the entry in the same commit and note it in the progress file.

**Progress file:** `docs/superpowers/plans/progress-imgui-binding.md` (created in Task 1; update it at the end of every task: task table, Mutations table, Notes).

## Global Constraints

- Part A's constraints stand (`docs/superpowers/plans/2026-11-27-imgui-addon-part-a.md`, "Global Constraints"): public headers only, vendored files untouched, everything under `HERMES_NODE_ENABLE_UI`, the copyright header, ASCII commit messages ending with `Co-Authored-By: Claude Fable 5.2 <noreply@anthropic.com>`, no commit hash in any committed file, `./utils/format.sh -f` and `cmake --build cmake-build-asan --target check-hermes-node` before every commit, never two lit invocations over one build directory.
- `imgui_internal.h` is included by `addons/imgui/bind/bind_guards.cpp` and by no other file. Every other unit reaches internal state through the functions `bind_guards.h` declares. The binding's one write to internal state is the reset of `g.BeginComboDepth` in `newFrame()` (Task 5 verifies the facts it rests on first).
- The addon's export table does not change: `test/imgui-symbols.js` passes unmodified after every task.
- Error codes are exactly: `ERR_INVALID_ARG_TYPE`, `ERR_INVALID_ARG_VALUE`, `ERR_OUT_OF_RANGE` (a `TypeError` for the first two, a `RangeError` for the third, as in Node and in the design's "Errors"), `ERR_IMGUI_NO_FRAME`, `ERR_IMGUI_NO_WINDOW`, `ERR_IMGUI_USAGE`, `ERR_IMGUI_REENTRANT` (on `Error`). Messages start `imgui.<name>(): ` and name the argument.
- A rejected call does not reach ImGui: `__test.bindCalls()[name]` is incremented immediately before the ImGui call and nowhere else.
- Window tests are named `test/ui-imgui-bind-*.js` and `test/ui-imgui-app.js`, `// REQUIRES: ui, ui-gpu, ui-imgui` and `// REQUIRES: display`, under `%with-display 300 sh -c '... > %t.out 2> %t.err'`, stderr read with `--implicit-check-not='[sapp:error' --implicit-check-not='[sokol-'`. Model the RUN block on `test/ui-imgui-input.js`.
- Assertions come from read-backs (`getItemRectMin/Max`, reference arrays, return values, `__test.state()`, `__test.bindCalls()`, captured pixels, trace lines), never from elapsed time. Waiting is `harness.until` (600-frame bound).
- **Every test must be shown able to fail:** after it passes, break the code it tests, see it fail, restore; record the mutation and its result in the progress file. For a group test, at least: one function's ImGui call removed while its `enterImGui()` stays (a **behavioural** assertion must fail: the counter still moves, so coverage cannot catch this), one function's `enterImGui()` removed (the coverage check must fail), one write-back removed, one argument check removed.
- Every group test ends with `harness.checkGroupCoverage(h, '<group>')` (Task 1), which fails unless every function of that group in `test/fixtures/imgui/bind-groups.js` has a non-zero `bindCalls()` count. That count proves the wrapper reached its ImGui call, not that the call did anything: every function also has a behavioural assertion (a rectangle, a value, a pixel, a window in `state().windows`) in its group's test, and the progress file lists any for which none could be written.
- **Every rejection is tested.** Each `can<Function>` guard registers its name; `__test.guards()` returns, per guard, how many times it has refused. `ui-imgui-bind-errors.js` ends with `harness.checkGuardCoverage(h)`, which fails unless every registered guard has refused at least once, and each task that adds a guard adds its case there and a mutation with that guard disabled. Each "Rejected" and (a) line of a surface entry likewise has a case in `ui-imgui-bind-args.js` or the group's test.
- Tests of the binding open their windows through the harness, without `highDpi` (scale 1 on both platforms). Pixel assertions sample only flat interiors at least 3 px from any edge of a shape; nothing asserts the exact colour of an anti-aliased edge or a 1 px stroke.
- Trace lines as in part A: only under `HERMES_NODE_DEBUG_NATIVE=UI`, `[ui] imgui: <text>`.
- An `hnui` or host gap is fixed in the host in its own commit with its own test.

## Review Focus

Inputs the spec implies and no group's listed test would otherwise exercise; each has its test in the task named.

1. **A reference array that is not what it claims**: a hole (`[, 1]`), a wrong element type (`['x']` for `ref<float>`), too short, a `Float32Array` where an `Array` is required, an array whose length changes in an index accessor: the documented `TypeError` / `RangeError`, ImGui not called. Task 1.
2. **The application thrown at mid-frame**: a panel throws inside nested `begin` / `beginChild` / `beginTable` / `listClipper` with an `uncaughtException` listener installed: the next frame draws every panel and traces no usage error beyond the discarded frame's. Task 9.
3. **Files the viewer cannot show plainly**: an empty file, one of exactly 1 MiB and one a byte over, invalid UTF-8, valid UTF-8 containing U+FFFD, a character cut by the cap, a file with one 1 MiB line, a file deleted between listing and opening, a directory that cannot be read: a message in the panel and a log line, never a throw. Task 9.
4. **A window with no area**: the display size zero or tiny (a resize to 1x1 through the driver) while the application runs: no usage error, no throw, frames resume at a normal size. Task 9.
5. **Strings at the edges**: an empty label where an ID is needed, a label of only `##x`, a 1 MiB text, a string with an unpaired surrogate, a string with U+0000: the documented result for each (usage error, accepted, accepted, replacement character, `ERR_INVALID_ARG_VALUE`). Task 4.

## File Structure

| Path | Responsibility |
|---|---|
| `addons/imgui/bind/bind_util.h`, `bind_util.cpp` | Argument readers and writers, error throwing, the call scope (re-entrancy, frame check, call counting, usage-error check), registration. |
| `addons/imgui/bind/bind_guards.h`, `bind_guards.cpp` | Everything needing `imgui_internal.h`: error callback, recovery configuration, precondition reads, clipper scope snapshot, window list for the test hook. |
| `addons/imgui/bind/bind_constants.cpp` | Exported enumerators and the per-type valid masks / value sets. |
| `addons/imgui/bind/bind_<group>.cpp` | One per group: `frame`, `windows`, `layout`, `text`, `widgets`, `trees_tabs`, `menus`, `popups`, `tables`, `queries`, `style`, `plots`, `drawlist`, `clipper`, `docking`. |
| `addons/imgui/imgui_module.cpp` | Frame protocol; gains `ensureContext`, the usage-error path of `render()`, and the group registration calls. |
| `addons/imgui/imgui_test_hooks.cpp` | Gains `bindCalls()` and `state().windows`. |
| `test/fixtures/imgui/bind-groups.js` | The function names by group, and the constant samples: the checked-in surface. |
| `test/fixtures/imgui/harness.js` | Gains the helpers below. |
| `test/imgui-bind-surface.js`, `test/ui-imgui-bind-*.js`, `test/ui-imgui-app.js`, `test/imgui-app-viewer.js` | Tests. |
| `test/fixtures/imgui/app-tree/` | The application's test-mode fixture tree. |
| `examples/imgui-app/app.js`, `layout.js`, `panels/{files,viewer,widgets,monitor,canvas,log}.js`, `README.md` | The application. |
| `addons/imgui/README.md`, `CLAUDE.md`, `examples/README.md` | Documentation. |

**`bind_util.h`** (namespace `hnimgui::bind`). Later tasks use exactly these names:

```cpp
// One per binding function, constructed first. Throws ERR_IMGUI_REENTRANT
// (ok() false) if another binding function is on the stack.
class Call {
 public:
  Call(napi_env env, napi_callback_info info, const char *name);
  ~Call();                       // lowers the inside-a-call flag
  bool ok() const;               // false once anything has thrown
  napi_env env() const;
  size_t argc() const;
  napi_value arg(size_t i) const;      // undefined when i >= argc
  bool isMissing(size_t i) const;      // undefined, or beyond argc

  // Frame requirement; each throws and returns false when unmet.
  bool needFrame();              // ERR_IMGUI_NO_FRAME
  bool needContext();            // creates it; ERR_IMGUI_NO_WINDOW with no window
  bool needNoFrame();            // ERR_IMGUI_USAGE inside a frame

  // Readers: return false after throwing. `what` is the parameter's name.
  bool boolean(size_t i, const char *what, bool *out);
  bool int32(size_t i, const char *what, int *out);            // signed 32-bit
  bool intIn(size_t i, const char *what, int lo, int hi, int *out);
  bool u32(size_t i, const char *what, ImU32 *out);            // -2^31..2^32-1, mod 2^32
  bool f32(size_t i, const char *what, float *out);            // finite, float range
  bool f64(size_t i, const char *what, double *out);
  bool string(size_t i, const char *what, std::string *out);   // no U+0000
  bool stringOrNull(size_t i, const char *what, std::string *out, bool *isNull);
  bool vec2(size_t i, const char *what, ImVec2 *out);
  bool vec4(size_t i, const char *what, ImVec4 *out);
  bool flags(size_t i, const char *what, const EnumInfo &type, int *out);
  bool enumValue(size_t i, const char *what, const EnumInfo &type, int *out);
  bool format(size_t i, const char *what, FormatKind kind, std::string *out);
  // Reference arrays: read n elements now, remember the array for write-back.
  bool refBool(size_t i, const char *what, bool nullable, RefBool *out);
  bool refInt(size_t i, const char *what, int n, bool nullable, RefInt *out);
  bool refFloat(size_t i, const char *what, int n, bool nullable, RefFloat *out);
  bool refString(size_t i, const char *what, size_t capacity, RefString *out);
  bool floats(size_t i, const char *what, FloatSpan *out);     // Array (copied) or Float32Array (borrowed); call LAST

  // After the arguments are read: counts the call, then the caller calls ImGui.
  void enterImGui();
  // After the ImGui call: throws ERR_IMGUI_USAGE if ImGui reported an error
  // during it (returns false then, and nothing is written back).
  bool leaveImGui();
  // Writers, used only after leaveImGui() returned true.
  bool write(RefBool &r); bool write(RefInt &r); bool write(RefFloat &r); bool write(RefString &r);
  napi_value result(bool v); napi_value result(double v); napi_value resultU32(ImU32 v);
  napi_value resultVec2(size_t outArg, ImVec2 v);   // fills arg(outArg) if given, else a new Array
  napi_value resultVec4(size_t outArg, ImVec4 v);
  napi_value undefined();
  // Throwing helpers.
  napi_value throwUsage(const char *fmt, ...);
  napi_value throwType(const char *code, const char *fmt, ...);
  napi_value throwRange(const char *fmt, ...);
};
// Around a call from the binding into a JavaScript callback (the list
// clipper's): lowers the inside-a-call flag so the callback may call
// binding functions, raises the callback depth, and restores both in its
// destructor, also when a JavaScript exception is pending. Nests.
class CallbackScope { public: CallbackScope(); ~CallbackScope(); };
// True while any CallbackScope is alive; newFrame() and render() throw
// ERR_IMGUI_USAGE then.
bool insideCallback();
enum class FormatKind { Float, Int };
struct EnumInfo { const char *name; bool isFlags; int validMask; const int *values; int valueCount; };
bool registerFunction(napi_env env, napi_value target, const char *name, napi_callback cb);
// Each group: bool register<Group>(napi_env env, napi_value exports);
```

**`bind_guards.h`** (namespace `hnimgui::guard`):

```cpp
void configureContext();              // recovery flags + error callback; call from context creation
void beginFrame();                    // re-assert the recovery flags; reset BeginComboDepth
const char *takeUsageError();         // first message since the last take, or nullptr; clears it
bool labelIdIsWindowId(const char *label);   // the "label is an ID" check
// One per precondition of kind (b) in the surface table, named can<Function>:
// returns nullptr when the call is safe, else the message to throw.
const char *canEndGroup(); const char *canEndChild(); const char *canEndTooltip();
const char *canEndListBox(); const char *canEndMenuBar(); const char *canEndTabItem();
const char *canEndCombo(); const char *canTreePop(); const char *canTableRowCall();
const char *canTableSetColumnIndex();
// One per (b) precondition in the surface table: tasks add the rest
// (canBeginMenuBar, canTableSetupScrollFreeze, canBeginPopupContextItem,
// ...), each registered by name for __test.guards().
std::vector<std::pair<std::string, int>> guardRefusals();   // name, times refused
struct ScopeSnapshot { void *window; int windowStackSize; void *table; };
ScopeSnapshot snapshotScope(); bool sameScope(const ScopeSnapshot &s);
struct WindowInfo { std::string name; bool active; bool docked; };
std::vector<WindowInfo> windowsOfLastFrame();
```

**Harness additions, `test/fixtures/imgui/harness.js`:**

- `rect(imgui)`: `{min: imgui.getItemRectMin(), max: imgui.getItemRectMax()}` of the last item, for use inside a draw function; `center(r)`.
- Injected events arrive some frames later (the harness says so beside `inject`), so every helper waits on a read-back with `until`, as `test/ui-imgui-input.js:124-137` does, never on a frame count:
  - `moveTo(h, drawFn, [x, y])`: inject a mouse move, `until` `state().mouseX/mouseY` equal it.
  - `clickAt(h, drawFn, [x, y], button = 0)`: `moveTo`; inject down, `until` `state().mouseDown[imguiButton]`; inject up, `until` it is false; then one more frame.
  - `dragFromTo(h, drawFn, from, to)`: `moveTo(from)`, down with its barrier, `moveTo` the midpoint, `moveTo(to)`, up with its barrier, one more frame.
  - `typeText(h, drawFn, text, done)` and `pressKey(h, drawFn, sappKeyCode, modifiers, done)`: inject, then `until(done)`, where `done(state)` is the test's own predicate on what the input must have changed.
  - A widget's return value is true for one frame somewhere inside these waits, so a test's `drawFn` **accumulates** what it needs (`clicked = imgui.button('b') || clicked`) and the test reads the accumulated value afterwards.
- `expectThrow(fn, code)`: calls `fn`, returns the error if its `code` matches **and** it is an instance of the class that code belongs to (`TypeError`, `RangeError`, `Error`), else prints `NOTHROW <code>`, `WRONGCODE <got>` or `WRONGCLASS <got>` and exits 1.
- `checkGuardCoverage(h)`: prints `GUARDS ok`, or `UNREFUSED <names...>` and exits 1.
- `adopt({ui, win, imgui})`: a harness handle for a window the harness did not open (the application's), with `published` set by `resync`.
- `calls(h, name)`: `h.imgui.__test.bindCalls()[name] || 0`.
- `checkGroupCoverage(h, group)`: prints `COVERAGE <group> ok`, or `UNCOVERED <group> <names...>` and exits 1.
- `rawTick(h, fn)`: one frame callback run without the harness's `render()` bookkeeping, for cases where `render()` throws; the caller follows it with `resync(h)`.

---

### Task 1: Binding core, the error model, and the Frame group

**Files:**
- Create: `addons/imgui/bind/bind_util.h`, `bind_util.cpp`, `bind_guards.h`, `bind_guards.cpp`, `bind_frame.cpp`, `test/fixtures/imgui/bind-groups.js`, `test/imgui-bind-surface.js`, `test/ui-imgui-bind-args.js`, `test/ui-imgui-bind-errors.js`, `docs/superpowers/plans/progress-imgui-binding.md`
- Modify: `addons/imgui/CMakeLists.txt`, `imgui_module.cpp`, `imgui_addon.h`, `imgui_test_hooks.cpp`, `test/fixtures/imgui/harness.js`

**Interfaces:**
- Produces: everything in the three blocks above; `hnimgui::ensureContext(napi_env) -> bool` (creates the context for the window, calls `guard::configureContext()`, false with no window); `__test.bindCalls()`; `__test.guards()` (name -> times refused, from `guard::guardRefusals()`); `__test.state().windows`; `bind-groups.js` exporting `{groups: {<group>: [names]}, constants: {<name>: value}, all()}`.
- To have something to test the core with, this task also binds, from the surface table: the whole **Frame** section, and from **Windows** `begin`, `end`, `beginChild`, `endChild`, `setNextWindowPos`, `setNextWindowSize`, from **IDs** `pushID`, `popID`, from **Text** `text`, from **Widgets** `button`, `checkbox`, `sliderFloat3`, from **Layout** `beginGroup`, `endGroup`, `setCursorScreenPos`, from **Item and input queries** `getItemRectMin`, `getItemRectMax`, from **Style** `pushStyleColor`, `popStyleColor`, `pushStyleVar`, `popStyleVar` (later tests need a flat window background and zero padding before Task 7). Until Task 2 exists, a flags or enum parameter is read with `int32` and a `// Task 2: EnumInfo` comment; Task 2 Step 3 replaces every one, and its test is what checks them. Their groups' other functions come in later tasks; `bind-groups.js` lists only what is bound so far, and each later task appends.

- [ ] **Step 1: Write `test/imgui-bind-surface.js`** (`// REQUIRES: ui, ui-imgui`, no display: it only loads the module under `%hermes-node-ui` and compares). It prints `EXPORTS ok` when the sorted own property names of the module (functions, minus `__test`, plus `drawList.*` and the constants) equal `bind-groups.js`'s `all()`, else `EXTRA <names>` / `MISSING <names>`; and `CONSTANTS ok` when every sample in `constants` matches. Run it; expect FAIL.
- [ ] **Step 2: Write `test/ui-imgui-bind-args.js`**, cases printed as `<CASE> ok`, each checking the error's `code` with `expectThrow` and that `calls(h, name)` did not move:
  - `ARGTYPE`: `begin(1)`, `checkbox('a', true)`, `text()`, `sliderFloat3('s', [1, 2, 3], 'x', 1)` -> `ERR_INVALID_ARG_TYPE`.
  - `REFARRAY` (Review Focus 1): `checkbox('a', [])`, `checkbox('a', [1])`, `sliderFloat3('s', [1, 2], 0, 1)`, `sliderFloat3('s', [, 1, 2], 0, 1)`, `sliderFloat3('s', new Float32Array(3), 0, 1)` -> `ERR_INVALID_ARG_TYPE` or `ERR_OUT_OF_RANGE` as the spec's table says (too short is `ERR_OUT_OF_RANGE`); and an array of three whose index-0 accessor sets `length = 1` when read: the binding reads the three elements it needs through `napi_get_element` (the later ones then read `undefined`) -> `ERR_INVALID_ARG_TYPE`, no call.
  - `FLOAT`: `sliderFloat3('s', [NaN, 0, 0], 0, 1)`, `Infinity` as a bound, `1e300` -> `ERR_OUT_OF_RANGE`.
  - `NUL`: `text('a\0b')` -> `ERR_INVALID_ARG_VALUE`.
  - `NOFRAME`: each bound frame-requiring function outside a frame -> `ERR_IMGUI_NO_FRAME`.
  - `EITHER`: `getTime()`, `getFrameRate()`, `getDeltaTime()`, `getDisplaySize()`, `wantCaptureMouse()`, `wantCaptureKeyboard()` return values of the right type before the first `newFrame()`; `getDisplaySize(out)` returns `out` itself, filled.
  - `REENTRANT`: an array whose index 0 is an accessor calling `imgui.render()`, passed to `checkbox` -> the accessor's call throws `ERR_IMGUI_REENTRANT`; with an accessor that throws `Error('boom')` on read, `checkbox` propagates it and its count does not move; with one that throws on write, `checkbox` propagates it and its count moved by one.
  - `OPTIONAL`: `begin('w')`, `begin('w', undefined, 0)`, `begin('w', null)` all draw.
  Run; expect FAIL.
- [ ] **Step 3: Write `test/ui-imgui-bind-errors.js`**, cases `<CASE> ok` plus TRACE checks of `[ui] imgui: usage error: `:
  - `UNMATCHED_END`: `end()` with only the implicit window -> `ERR_IMGUI_USAGE` from the call; the frame continues and `render()` returns true.
  - `LEFT_OPEN`: `begin('w')` with no `end()` -> `render()` throws `ERR_IMGUI_USAGE`; `state().frameOpen` is false afterwards; nothing was published (compare `h.published` after `resync`); the next ordinary frame draws.
  - `PUSHID_LEFT`: `pushID('a')` unpopped inside a window -> thrown by that window's `end()`.
  - `MIXED_TICK`: one tick with two callbacks, the first a complete frame drawing part A's `__test.scene('fill')`, the second a frame whose `render()` throws -> the capture of that tick matches the first callback's picture.
  - `NO_TARGET`: `newFrame(); begin('w'); render()` outside a frame callback -> throws `ERR_IMGUI_USAGE` (not the `false` return).
  - `GUARD_GROUP`: `endGroup()` with no group -> `ERR_IMGUI_USAGE`, `calls(h, 'endGroup')` unchanged; inside `begin('w')`: `beginGroup(); end()` (ImGui recovers the group inside `end()` and the call throws), catch the error, then `endGroup()` -> `ERR_IMGUI_USAGE` from the guard, not an abort.
  - `GUARD_CHILD`: `endChild()` in a top-level window -> `ERR_IMGUI_USAGE`; a child that ImGui clips (inside a scrolling parent, placed with `setCursorScreenPos` far below the parent's visible area) whose `beginChild` returned false, followed by `endChild()` -> accepted. The case first prints `SETUP child clipped` from the read-back of that false return, and fails if it was true: the forcing geometry is the test's to get right.
  - `DISCARD`: `newFrame(); begin('w');` then a second `newFrame()` -> no throw, trace `frame discarded`, the frame that follows renders.
  - `CLOSING`, a second RUN line: from an `'exit'` listener, `newFrame(); begin('w'); render()` -> throws `ERR_IMGUI_USAGE`; print the code; exit status preserved.
  Run; expect FAIL.
- [ ] **Step 4: Implement `bind_guards.cpp`.** `configureContext()` sets `io.ConfigErrorRecoveryEnableAssert = false`, `io.ConfigErrorRecoveryEnableTooltip = false`, and `GImGui->ErrorCallback` to a function that keeps the first message and traces `usage error: <msg>`. `beginFrame()` sets the two flags again. Read `ErrorLog()` and `ErrorRecoveryTryToRecoverState()` in `imgui.cpp` first and write, in the progress file's Notes, what `ErrorLog` does when the callback is set and asserts are off.
- [ ] **Step 5: Implement `bind_util.cpp`** to the header above. The inside-a-call flag and the callback depth are file-statics there; `Call`'s constructor throws `ERR_IMGUI_REENTRANT` when the flag is set, and `CallbackScope` saves, lowers and restores it. `newFrame()` and `render()` throw `ERR_IMGUI_USAGE` when `insideCallback()`. Element reads use `napi_get_element` and check `napi_is_exception_pending` after each. `floats()` copies an `Array`, and for a `Float32Array` calls `napi_get_typedarray_info` (the borrowed pointer is valid until the next call into the engine, which is why it is read last).
- [ ] **Step 6: Change `imgui_module.cpp`:** `createContext` becomes `ensureContext` and calls `guard::configureContext()` before returning; `newFrame()` and `render()` construct a `Call` (so they take part in re-entrancy), `newFrame()` calls `guard::beginFrame()` and, on the discard path, calls `guard::takeUsageError()` after `EndFrame()` and drops the result; `render()` calls `ImGui::Render()`, sets the frame closed, then if `guard::takeUsageError()` is non-null throws `ERR_IMGUI_USAGE` **before** `hnui_frame_target`. Register the groups.
- [ ] **Step 7: Implement `bind_frame.cpp` and the pilot functions** from their surface entries. Add `bindCalls()` and `state().windows` to the test hooks.
- [ ] **Step 8: Add the harness helpers.**
- [ ] **Step 9: Run the three tests and the whole suite; expect PASS.** Mutations (record each): `configureContext()` not called from `ensureContext` (ERRORS must abort or fail); the usage-error check moved after `hnui_frame_target` in `render()` (`LEFT_OPEN`'s publication check must fail); the re-entrancy flag never set (`REENTRANT` fails); `enterImGui()` counting moved before argument reading (every "did not move" check fails); `canEndGroup()` returning nullptr always (`GUARD_GROUP` aborts); `canEndChild()` requiring a true `beginChild` (`GUARD_CHILD`'s accepted half fails); one export removed from `bind-groups.js` (`EXTRA`); a guard registered and never refused by any case (`UNREFUSED`: do it by adding a dummy guard, then remove it).
- [ ] **Step 10: Create the progress file and commit:** `Add the ImGui binding core and its error model`.

### Task 2: Constants, and flag and enum checking

**Files:**
- Create: `addons/imgui/bind/bind_constants.cpp`, `test/ui-imgui-bind-constants.js`
- Modify: `bind-groups.js` (constants), `addons/imgui/CMakeLists.txt`

**Interfaces:**
- Produces: `bool registerConstants(napi_env, napi_value exports)`; one `extern const EnumInfo k<Type>;` per enum type in the surface table's "Constants" section (`kWindowFlags`, `kCol`, `kKey`, ...), declared in `bind_util.h`; `imgui.FLT_MIN`, `imgui.FLT_MAX`.

- [ ] **Step 1: Write the test.** `CONSTANTS ok`: for every type in the surface table's "Constants" section, the exported count equals the table's count, three named samples per type equal the values written in `bind-groups.js` (copied by hand from `imgui.h`, with the line in a comment), and none of the table's excluded names is exported. `FLT ok`: `Math.fround(imgui.FLT_MAX) === imgui.FLT_MAX`, `imgui.FLT_MIN > 0`, `Math.fround(-imgui.FLT_MIN) !== 0`. `REJECT ok`: `begin('w', null, 1 << 30)` (not a public window flag) and `begin('w', null, 2.5)` -> `ERR_OUT_OF_RANGE`; `begin('w', null, imgui.WindowFlags_NoTitleBar | imgui.WindowFlags_NoResize)` draws; a negative value produced by `|` with a high public bit is accepted where such a bit exists. Run; expect FAIL.
- [ ] **Step 2: Implement.** Each constant is emitted by a macro taking the C++ enumerator itself (`HNIMGUI_CONST(ImGuiWindowFlags_NoTitleBar)` exports `WindowFlags_NoTitleBar` with that value), so no value is typed. Each `EnumInfo`'s mask or value list is built from the same macro list, so "valid to pass" and "exported" cannot differ.
- [ ] **Step 3: Give `begin`, `beginChild`, `checkbox`... every flags or enum parameter bound so far its `EnumInfo`.**
- [ ] **Step 4: Run; expect PASS.** Mutations: one constant's macro line deleted (count fails); a private enumerator added to a list (excluded-name check fails); the mask check removed from `Call::flags` (`REJECT` fails).
- [ ] **Step 5: Commit:** `Export ImGui's constants and check flag arguments`.

### Task 3: Windows, Layout, IDs, Text, Item and input queries

**Files:**
- Create: `addons/imgui/bind/bind_windows.cpp`, `bind_layout.cpp`, `bind_text.cpp`, `bind_queries.cpp`, `test/ui-imgui-bind-windows.js`, `test/ui-imgui-bind-layout.js`
- Modify: `bind-groups.js`, `CMakeLists.txt`, `ui-imgui-bind-args.js`

**Interfaces:**
- Consumes: Task 1's `Call`, guards, harness helpers; Task 2's `EnumInfo`s.
- Produces: the surface table's sections **Windows**, **Layout**, **IDs**, **Text**, **Item and input queries**, complete.

- [ ] **Step 1: Write `ui-imgui-bind-windows.js`.** A window placed with `setNextWindowPos([40, 50])` and `setNextWindowSize([300, 200])`: `getWindowPos()` and `getWindowSize()` read those back exactly, into a new array and into a passed one (`=== out`); `begin('w', open)` with a click on the close button (its rectangle from the title bar's right end, as part A's `clip` scene locates things) sets `open[0]` to false; `isWindowHovered()` / `isWindowFocused()` follow an injected mouse position and click; a child window taller than its parent has `getScrollMaxY() > 0`, and `setScrollHereY(1)` on the last item makes `getScrollY() === getScrollMaxY()` on the next frame; `setNextWindowBgAlpha(1)` with a flat style colour gives exact pixels inside the window and `CLEAR` outside.
- [ ] **Step 2: Write `ui-imgui-bind-layout.js`.** Geometry from item rectangles: `sameLine()` puts the second item's `min[1]` equal to the first's; `indent(20)` moves `min[0]` by 20 and `unindent(20)` back; `dummy([30, 40])` has that size; `setNextItemWidth(123)` gives a slider of width 123; `pushItemWidth` / `popItemWidth` likewise; `setCursorScreenPos([200, 150])` then an item puts its `min` there and `getCursorScreenPos()` reads it; `calcTextSize('abc')` equals the size of `text('abc')`'s rectangle; `getTextLineHeight`, `getTextLineHeightWithSpacing`, `getFrameHeight` are positive and ordered; the text variants each produce an item (rectangle non-empty) and `textColored([0, 1, 0, 1], ...)` gives a pure green pixel inside a glyph column found by scanning the captured row; `pushID`/`getID`: the same label under two different `pushID` values gives two different `getID` results, and `pushID(7)` / `pushID('7')` differ. Queries: `isItemHovered`, `isItemClicked`, `isItemActive`, `isMouseDown`, `isMouseClicked`, `isMouseDoubleClicked`, `getMousePos`, `getMouseDragDelta`, `isKeyDown`, `isKeyPressed(imgui.Key_A)` follow injected events; `setKeyboardFocusHere()` before an `inputText`-free item is exercised with a `button` (Task 4 adds the text-field half); `isItemDeactivatedAfterEdit` is covered in Task 4 and listed in that group's coverage there.
- [ ] **Step 3: Run both; expect FAIL. Implement the five sections. Run; expect PASS,** with `checkGroupCoverage` for each group (functions whose observable test is in Task 4 are called here at least once so the count is non-zero; their behaviour assertion is in Task 4, and the progress file says which).
- [ ] **Step 4: Mutations** per Global Constraints, plus: `getWindowPos(out)` returning a new array instead of `out`; `pushID(number)` dispatched to the string overload.
- [ ] **Step 5: Commit:** `Bind ImGui's window, layout, text and query functions`.

### Task 4: Widgets

**Files:**
- Create: `addons/imgui/bind/bind_widgets.cpp`, `test/ui-imgui-bind-widgets.js`, `test/ui-imgui-bind-input-text.js`
- Modify: `bind-groups.js`, `CMakeLists.txt`, `ui-imgui-bind-args.js`

**Interfaces:**
- Produces: the surface table's **Widgets** section, complete, including the "label is an ID" check on every entry marked so, in every group bound so far.

- [ ] **Step 1: Write `ui-imgui-bind-widgets.js`.** For each widget, built in one window, its rectangle read back, an injected interaction, then the reference array and the return value: `button`, `smallButton`, `arrowButton`, `invisibleButton` return true on the frame of the release; `checkbox` flips `v[0]`; both `radioButton` forms; `sliderFloat`, `sliderInt`, `sliderFloat2..4` after a drag to the right end hold the maximum exactly; `dragFloat`, `dragInt` change by a drag; `inputFloat`, `inputInt` take typed digits and Enter; `beginCombo` opens on a click and `selectable` inside it returns true on a click, after which the combo is closed; `selectable` with a reference array flips it; `colorEdit3` / `colorEdit4` change a component by dragging its field; `colorButton` returns true on a click and shows its exact colour at its centre pixel; `progressBar(0.5, [200, 20])` has that rectangle; `bullet` produces an item; `beginListBox` / `endListBox` contain selectables; a slider with `format` `'%.1f units'` draws (item present) and one with `'%s'` or `'%d'` on a float throws `ERR_INVALID_ARG_VALUE` without a call.
- [ ] **Step 2: Write `ui-imgui-bind-input-text.js`.** `inputText('n', name, 64)`: click, type `abc`, `name[0]` is `'abc'` after each keystroke's frame and the call returned true on those frames; with `InputTextFlags_EnterReturnsTrue` the string still follows the keystrokes while the return is true only on the Enter frame; `isItemDeactivatedAfterEdit()` is true on the frame after focus leaves; capacity 4 with the incoming string `'abcdef'` gives `'abc'` written back with no interaction; an emoji survives a round trip; a string with an unpaired surrogate comes back with U+FFFD; capacity 0, 2^24 + 1 and 1.5 -> `ERR_OUT_OF_RANGE`; each `InputTextFlags_Callback*` bit -> `ERR_OUT_OF_RANGE` (they are not exported, so the test names them by value from `imgui.h`, with the line in a comment); `inputTextWithHint` shows its hint (item present, string unchanged); `inputTextMultiline` takes Enter as a newline; `setKeyboardFocusHere()` before the field makes `state().wantTextInput` true with no click. `STRINGS` (Review Focus 5): `button('')` at a window's root throws `ERR_IMGUI_USAGE` with `calls` unchanged, `button('##x')` is accepted, inside `pushID('p')` `button('')` is accepted; `text` of a 1 MiB string draws.
- [ ] **Step 3: Run; expect FAIL. Implement. Run; expect PASS.**
- [ ] **Step 4: Mutations** per Global Constraints, plus: `inputText` writing back only when the widget returned true (the `EnterReturnsTrue` case fails); the buffer compared with the truncated string instead of the original (the capacity-4 case fails); the format check removed (ASAN or a crash on `'%s'`: record what happened); `labelIdIsWindowId` returning false always (`button('')` aborts in the Debug build).
- [ ] **Step 5: Commit:** `Bind ImGui's widgets`.

### Task 5: Trees, tabs, menus, popups and tooltips

**Files:**
- Create: `addons/imgui/bind/bind_trees_tabs.cpp`, `bind_menus.cpp`, `bind_popups.cpp`, `test/ui-imgui-bind-trees-tabs.js`, `test/ui-imgui-bind-menus.js`, `test/ui-imgui-bind-popups.js`
- Modify: `bind_guards.cpp`, `bind-groups.js`, `CMakeLists.txt`, `ui-imgui-bind-errors.js`

**Interfaces:**
- Produces: the sections **Trees and tabs**, **Menus**, **Popups and tooltips**, complete; guards `canTreePop`, `canEndTabItem`, `canEndMenuBar`, `canEndTooltip`, `canEndListBox`, `canEndCombo`, and the `BeginComboDepth` reset.

- [ ] **Step 1: Verify the combo facts** the design relies on, from `imgui_widgets.cpp` and `imgui.cpp`: where `BeginComboDepth` is incremented, decremented and reset, and whether any combo can be open across a frame boundary. Write the finding in the progress file. If zero at `newFrame()` is not always right, do not add the reset: record the left-open case in the tracker and tell the controller.
- [ ] **Step 2: Write the three tests.** Trees/tabs: `treeNode` opens on a click and its child item appears only then; `setNextItemOpen(true)` opens without one; `treeNodeEx(label, TreeNodeFlags_NoTreePushOnOpen)` returns true and a following `treePop()` throws `ERR_IMGUI_USAGE`; both `collapsingHeader` forms, the second's close button clearing `pVisible[0]`; `beginTabBar` with two `beginTabItem`s, a click on the second making it the one that returns true; a tab's `pOpen` cleared by its close button. Menus: `beginMainMenuBar`, `beginMenu` opened by a click, `menuItem(label, shortcut, selected)` returning true on a click, the reference-array form flipping its array; `beginMenuBar` in a window with `WindowFlags_MenuBar`, and false without that flag. Popups: `openPopup` + `beginPopup` showing on the next frame, `closeCurrentPopup` hiding it; `beginPopupModal` blocking a click on a button behind it; `beginPopupContextItem` on a right click; `setTooltip`, `setItemTooltip`, `beginTooltip`/`endTooltip` producing a window named in `state().windows` while the item is hovered.
- [ ] **Step 3: Extend `ui-imgui-bind-errors.js`:** `GUARD_TREE` (`treePop()` with nothing open; and with a `pushID` left unpopped inside the node), `GUARD_TABITEM` (`endTabItem()` inside a tab bar before any `beginTabItem`), `GUARD_MENUBAR`, `GUARD_TOOLTIP`, `GUARD_LISTBOX` (`endListBox()` after a `beginListBox` that returned false, forced by the same off-screen placement as `GUARD_CHILD`, with the same `SETUP` read-back), `GUARD_BEGINMENUBAR` (a second `beginMenuBar()` in one window before `endMenuBar()`), `GUARD_CONTEXTITEM` (`beginPopupContextItem()` with no id after an item that has none, e.g. `text`), `GUARD_COMBO` (`endCombo()` unmatched; then a correctly paired combo still opens under the name `state().windows` showed before), `COMBO_LEFT_OPEN` (a combo's `endCombo` omitted in one frame; next frame a combo opens under the same window name as before, if Step 1 allowed the reset), `ENDMENU`, `ENDPOPUP`, `ENDTABBAR` (ImGui's own recoverable checks).
- [ ] **Step 4: Run; expect FAIL. Implement. Run; expect PASS.** Mutations per Global Constraints, plus each new guard returning nullptr always (its case must abort or fail; record which).
- [ ] **Step 5: Commit:** `Bind ImGui's trees, tabs, menus and popups`.

### Task 6: Tables

**Files:**
- Create: `addons/imgui/bind/bind_tables.cpp`, `test/ui-imgui-bind-tables.js`
- Modify: `bind_guards.cpp`, `bind-groups.js`, `CMakeLists.txt`, `ui-imgui-bind-errors.js`

**Interfaces:**
- Produces: the **Tables** section; guards `canTableRowCall`, `canTableSetColumnIndex`.

- [ ] **Step 1: Write the test.** A 3-column table with `tableSetupColumn` fixed widths 100, 150, 200 and `TableFlags_Borders`: cell item rectangles' `min[0]` differ by exactly the column widths plus the cell padding read from two adjacent cells; `tableHeadersRow` draws a header row: with `Col_TableHeaderBg` pushed to a flat colour, the pixel at each of the three columns' centres on the header row's middle line is that colour, and the same x one row lower is not; `tableNextColumn()` returns true three times then starts a row; `tableSetColumnIndex(2)` puts the next item in column 2; with `TableFlags_ScrollY`, an outer size and `tableSetupScrollFreeze(0, 1)`, the header's rectangle is unchanged after scrolling the body with an injected wheel event while a body cell's `min[1]` moves; `beginTable('t', 0)` and a column count over the limit in the surface table -> `ERR_OUT_OF_RANGE`. Errors, each as its surface entry classifies it: `TABLE_OUTSIDE_GUARDED` (the functions whose entry has a (b) guard on a current table, e.g. `tableNextRow()`: `ERR_IMGUI_USAGE`, count unchanged), `TABLE_OUTSIDE_FALSE` (`tableNextColumn()` with no table returns false, as in ImGui, count moved), `TABLE_OUTSIDE_RECOVERED` (those ImGui itself reports recoverably, e.g. `tableSetupColumn('c')`, `tableHeadersRow()`: `ERR_IMGUI_USAGE`, count moved), `TABLE_NO_ROW` (`tableSetColumnIndex(0)` before any row), `TABLE_NEGATIVE` (`tableSetColumnIndex(-1)` straight after `tableNextRow()` -> `ERR_OUT_OF_RANGE`, count unchanged), `TABLE_INDEX_HIGH` (an index equal to the column count: ImGui's recoverable error), `TABLE_FREEZE_LATE` (`tableSetupScrollFreeze` after the first row), `TABLE_WRONG_WINDOW` (a table function called from a child window begun inside the table's cell, where the entry guards it), `ENDTABLE` unmatched (ImGui's recoverable check).
- [ ] **Step 2: Run; expect FAIL. Implement. Run; expect PASS.** Mutations per Global Constraints, plus both guards disabled.
- [ ] **Step 3: Commit:** `Bind ImGui's tables`.

### Task 7: Style, plots, the draw list and the clipper

**Files:**
- Create: `addons/imgui/bind/bind_style.cpp`, `bind_plots.cpp`, `bind_drawlist.cpp`, `bind_clipper.cpp`, `test/ui-imgui-bind-style.js`, `test/ui-imgui-bind-drawlist.js`, `test/ui-imgui-bind-plots.js`, `test/ui-imgui-bind-clipper.js`
- Modify: `bind_guards.cpp`, `bind-groups.js`, `CMakeLists.txt`, `ui-imgui-bind-args.js`

**Interfaces:**
- Produces: the sections **Style**, **Plots**, **Draw list** (on the object `imgui.drawList`), **Clipper**.

- [ ] **Step 1: Write `ui-imgui-bind-drawlist.js`.** In a borderless, padding-free window covering the display with a flat background (the `quad`-style exact-pixel method of `test/ui-imgui-textures.js`): `addRectFilled([100, 100], [200, 180], red)` gives red at (150, 140) and background at (95, 140) and (205, 140); `addRect` with thickness 9 gives the colour at the middle of each side's stroke and background at the centre; `addTriangleFilled`, `addCircleFilled` (centre pixel); `addLine` horizontal with thickness 9 (the colour on its centre row, background 8 px above and below); `addCircle` with thickness 9 (the colour at the four axis points of the radius, background at the centre); `addBezierCubic` with thickness 9 whose four control points are collinear, so it is a straight segment sampled like the line; `addPolyline` (a closed square, thickness 9, from a flat `Array` and the same from a `Float32Array`: identical captures, sampled like `addRect`); `addText` (some pixel inside the text's `calcTextSize` box is exactly the given colour, found by scanning the box, and none outside it is). No assertion is made about a 1 px stroke or an edge pixel. `getColorU32([1, 0, 0, 1]) === 0xFF0000FF`; `colorConvertFloat4ToU32` equal to it and callable with no window; a negative colour number (`0xFF000000 | 0xFF`) draws the same as its unsigned form. Limits: 16384 points and an odd-length array -> `ERR_OUT_OF_RANGE`; `numSegments` 513 -> `ERR_OUT_OF_RANGE`.
- [ ] **Step 2: Write `ui-imgui-bind-style.js`.** `pushStyleColor(imgui.Col_WindowBg, [0, 0, 1, 1])` (and the number form) gives pure blue inside a window and the previous colour after `popStyleColor()`; `pushStyleVar(imgui.StyleVar_WindowPadding, [30, 30])` moves the first item's `min` by the difference; `pushStyleVar(imgui.StyleVar_Alpha, 0.5)` is accepted and a vector for it, or a number for `WindowPadding`, throws (the surface table says which error); `styleColorsLight()` then `styleColorsDark()` change the window background pixel between two known values, and both work before the first `newFrame()`.
- [ ] **Step 3: Write `ui-imgui-bind-plots.js`.** `plotLines` and `plotHistogram` with `[0, 1, 0, 1]`, graph size `[200, 100]`, scale 0..1, on a known frame-background colour: the item rectangle is 200x100 plus the label; for the histogram, a pixel in the middle of bar 1 is the plot colour and one in the middle of bar 0's upper half is the frame background; the same picture from an `Array` and a `Float32Array`; `valuesOffset` 4 on a 4-element array, -1, and 1 on an empty one -> `ERR_OUT_OF_RANGE`; an empty array draws an empty frame; a `NaN` value -> `ERR_OUT_OF_RANGE`.
- [ ] **Step 4: Write `ui-imgui-bind-clipper.js`.** 10000 items of height `getTextLineHeightWithSpacing()` in a 300 px child: the callback's ranges cover a contiguous span of about 300 / height items starting at 0, and `calls(h, 'text')` grows by that many, not 10000; after `setScrollHereY` to the end the last range ends at 10000; `getScrollMaxY()` is greater than 9990 * height (the exact value includes item spacing and window padding, `imgui.cpp:3457` and `:8661`, and is not asserted); a nested clipper inside the callback works. Rules: `render()` and `newFrame()` from the callback -> `ERR_IMGUI_USAGE`; a callback that throws `Error('boom')` propagates it and the next frame works; a callback that calls `endChild()` (closing the owning window), one that leaves a `beginChild` open, one inside a table that calls `endTable()`, and one that leaves a non-scrolling `beginTable` open: each returning (-> `ERR_IMGUI_USAGE`) and throwing (-> its own error), and after each the next frame draws; `itemHeight` 0, -1 and `count` -1 -> `ERR_OUT_OF_RANGE`.
- [ ] **Step 5: Run; expect FAIL. Implement.** The clipper: record `guard::snapshotScope()`, construct `ImGuiListClipper` on the stack, `Begin`, loop `Step()` calling the callback inside a `bind::CallbackScope` (which lowers the inside-a-call flag and makes `newFrame()` / `render()` throw through `insideCallback()`); after each callback, if an exception is pending or `!guard::sameScope(...)`, abandon (`ItemsCount = -1; End()`) when the scope differs or `End()` normally when it matches, and propagate or throw. **Run; expect PASS.**
- [ ] **Step 6: Mutations** per Global Constraints, plus: the scope comparison dropping the table (the non-scrolling-table case must abort in the Debug build or fail: record which); `ItemsCount` not set to -1 on abandon; the clipper depth not raised (`render()` from the callback: record what happens); a `Float32Array` copied instead of borrowed is **not** detectable by these tests, so say so in the progress file's list of behaviours no test detects.
- [ ] **Step 7: Commit:** `Bind ImGui's style, plots, draw list and list clipper`.

### Task 8: Docking and settings

**Files:**
- Create: `addons/imgui/bind/bind_docking.cpp`, `test/ui-imgui-bind-docking.js`
- Modify: `bind-groups.js`, `CMakeLists.txt`, `ui-imgui-bind-errors.js`

**Interfaces:**
- Produces: the **Docking and settings** section.

- [ ] **Step 1: Write the test.** `dockSpaceOverViewport()` returns a non-zero id, the same every frame; two windows, both floating at first (`state().windows` shows `docked: false`); `saveIniSettingsToMemory()` returns a string containing both window names; an ini string written in the test that docks both into the dock space (produce it once by hand: dock them with injected drags in a scratch run, save, paste the string into the test with the dock id replaced by the one `dockSpaceOverViewport()` returns, which the scratch run shows is stable for a given window size) passed to `loadIniSettingsFromMemory` **before the first `newFrame()`** makes both `docked: true` on the first frame and their rectangles tile the display; the same load between two frames (outside any frame) re-docks windows that the test first undocked by loading a floating layout; `loadIniSettingsFromMemory` inside a frame -> `ERR_IMGUI_USAGE`, count unchanged; `saveIniSettingsToMemory()` works before the first frame; with no window (a third RUN line under plain `%timeout`, no display) the four out-of-frame functions throw `ERR_IMGUI_NO_WINDOW`.
- [ ] **Step 2: Run; expect FAIL. Implement. Run; expect PASS.** Mutations per Global Constraints, plus `needNoFrame()` removed from the load.
- [ ] **Step 3: Commit:** `Bind ImGui's dock space and settings functions`.

### Task 9: The application

**Files:**
- Create: `examples/imgui-app/app.js`, `layout.js`, `panels/files.js`, `panels/viewer.js`, `panels/widgets.js`, `panels/monitor.js`, `panels/canvas.js`, `panels/log.js`, `README.md`, `test/ui-imgui-app.js`, `test/imgui-app-viewer.js`, `test/fixtures/imgui/app-tree/**`
- Modify: `examples/README.md`

**Interfaces:**
- Consumes: the whole binding. **No function may be added to the binding for the application without its surface-table entry, its group-test case and its `bind-groups.js` line**, in a commit of its own before this task's. A function the application turns out not to need stays (it is tested).
- Produces: each panel module exports `{name, open: [true], draw(ctx)}`, where `ctx` is `{imgui, ui, win, log(text), state, time, testMode}`.
- `app.js` exports `createApp(options) -> Promise<app>` and, when it is the main module, calls it and runs the loop. `options`: `{root, fs = require('fs').promises, clock = null, driven = false}`. `app`:
  - `ui`, `win`, `imgui`: the window it opened (`highDpi: true`) and the modules.
  - `frame() -> boolean`: one whole frame, `newFrame()` to `render()`, returning what `render()` returned. In normal use `app.js` calls it from its own `requestAnimationFrame` loop; with `driven: true` it registers nothing and the caller drives it (the test does, through `harness.frame(h, ...)`-style ticks on `harness.adopt(app)`).
  - `rects: Map<string, {min, max}>`: the last drawn rectangle of every Files row, by path, and of each menu item by label.
  - `panels`: the panel modules by lower-case name.
  - `setTime(seconds)`: sets the clock when `clock` was given as `'fixed'`.
  - `resetLayout()`, `quit()`: what View > Reset Layout and File > Quit call.
  - `idle() -> Promise`: resolves when no file or directory request is in flight.
  - `scale`: `win.dpiScale` as read at creation.
- `panels/viewer.js` exports `createViewer({fs, log})` with `load(path) -> Promise` (resolves when that request has settled, whether or not it was superseded), `state` (`{status: 'empty' | 'loading' | 'text' | 'hex' | 'error', path, lines | rows | code, truncated}`), and `draw(ctx)`. It reaches the filesystem only through the `fs` it was given: `open`, and on the handle `stat`, `read`, `close`.

- [ ] **Step 1: Write the fixture tree:** `app-tree/` with `a.txt` (3 lines, 12 bytes), `dir1/b.js` (a 40-line file), `dir1/deep/c.txt`, `empty.txt` (0 bytes), `binary.bin` (256 bytes 0..255), `fffd.txt` (valid UTF-8 containing a literal U+FFFD). Large files are generated by the tests into `%t`, so the checked-in fixture stays small.
- [ ] **Step 2: Write `test/imgui-app-viewer.js`** (`%hermes-node`, no window: it requires `panels/viewer.js` alone and never calls `draw`). Cases, `<CASE> ok`:
  - `CAP`: with a recording wrapper around `fs.promises` (it records every method called and, for `read`, the requested length and position), loading a file of 1 MiB + 1 byte: the wrapper saw no `readFile`, the sum of requested lengths is at most `1 << 20`, no request starts at or beyond `1 << 20`, `close` was called once, and `state.truncated` is true. Loading a file of exactly 1 MiB: `truncated` false.
  - `CLOSE_ON_ERROR`: a wrapper whose `read` rejects with `EIO`: `state.status` is `'error'`, code `EIO`, and `close` was still called once.
  - `SUPERSEDE`: a fake `fs` whose `open` returns promises the test resolves by hand; `load('first')` then `load('second')`; resolve the second's chain, then the first's: after both `load` promises settle, `state.path` is `'second'` with the second's content. Then the same with the first resolved before the second: still `'second'`.
  - `CLASSIFY`: `a.txt` is text with 3 lines; `empty.txt` text with 0 lines; `binary.bin` hex with 16 rows; `fffd.txt` **text** (a literal U+FFFD is valid); a file with an invalid byte sequence hex; a file whose only U+0000 makes it hex; a 1 MiB + 3 byte file of valid UTF-8 in which a three-byte character straddles the cap: text, truncated, with the incomplete character dropped; a file that is one 1 MiB line: text, 1 line, cut for display at 4096 characters with the marker.
  - `MISSING`: a path that does not exist: `'error'`, `ENOENT`.
  Run; expect FAIL.
- [ ] **Step 3: Write `test/ui-imgui-app.js`.** The test copies the fixture to `%t.tree`, adds an unreadable directory `locked/` (mode 000; when running as root the case prints `SKIP locked` and its CHECK line accepts either), calls `createApp({root, clock: 'fixed', driven: true})`, and makes `h = harness.adopt(app)`. Every frame is `harness.frame`-equivalent over `app.frame`; clicks use `harness.clickAt` at `app.rects`. Cases, `<CASE> ok`:
  - `SCALE <n>`: prints `app.scale` unchecked (1 under Xvfb, 2 on a Retina Mac), as part A's `SCALEVALUE`.
  - `PANELS`: after 10 frames `state().windows` contains `Files`, `Viewer`, `Widgets`, `Monitor`, `Canvas`, `Log`, each `active` and `docked`.
  - `OPEN`: a click at `rects.get(<tree>/a.txt)`, frames until `app.idle()` has resolved, then `viewer.state` is text, 3 lines, and stdout has `APP opened <tree>/a.txt 12 lines=3`.
  - `DIR`: a click on `dir1`: `APP dir <tree>/dir1 entries=2`, and `dir1/b.js`'s rectangle appears in `rects`.
  - `EDGES` (Review Focus 3): `empty.txt`; `binary.bin` (hex); a file removed after it was listed (`APP error <path> ENOENT`); `locked/` (`APP error <path> EACCES`): each prints its line and the next frame draws. (The size and encoding edges are `imgui-app-viewer.js`'s.)
  - `CANVAS`: at `setTime(0)` the canvas's centre circle is its configured colour at its centre pixel; at `setTime(1)` the pixel at `canvas.orbitPosition(1)` (in the units the capture uses: multiplied by `app.scale`) is the orbiting circle's colour, and the pixel at `orbitPosition(0)` is no longer.
  - `THROW` (Review Focus 2): with an `uncaughtException` listener installed, the test sets `panels.widgets.throwNextFrame = true`; the panel then throws from inside a `listClipper` callback, inside a table, inside a child window. The frame is run with `harness.rawTick`; the listener sees the error; after `resync`, 10 frames later `PANELS` holds again.
  - `TINY` (Review Focus 4): the window driver resizes the window to the smallest size the window system grants (ask for 1x1, read back what `win.width`/`win.height` became and print it unchecked) and back to 1280x800, with part A's resize protocol (`h.generation` from `state().targetGeneration`, `resync`): no `APP error`, no usage error, `PANELS` holds after.
  - `RESET`: after loading a floating layout through `imgui.loadIniSettingsFromMemory` between frames (one panel then `docked: false`), `app.resetLayout()` and two frames: all docked again.
  - `QUIT`: `app.quit()`: exit status 0 (the EXITCODE line).
  - TRACE: `--implicit-check-not='imgui: usage error'` on the main run; `THROW` is a RUN line of its own that checks the discarded frame's lines.
  Run; expect FAIL.
- [ ] **Step 4: Write the application** to the spec's "The application" section. Decisions fixed here:
  - `app.js`: `createWindow({title: 'hermes-node ImGui', width: 1280, height: 800, highDpi: true})`; before the first `newFrame()`, `loadIniSettingsFromMemory(layout.DEFAULT)`; a pending-layout variable applied at the start of `frame()`, before `newFrame()`, for Reset Layout; then `newFrame()`, main menu bar, `dockSpaceOverViewport()`, each open panel's `draw`, the modals, `render()`. Quit is `win.close()`. `root` is `process.argv[2]` or the current directory when run as the main module.
  - Time: `ctx.time` is the frame callback's timestamp in seconds, or the fixed clock.
  - `files.js`: nodes `{name, path, kind, children: null | 'loading' | [nodes] | {error}}`; a directory's children are requested on first open with `fs.readdir(path, {withFileTypes: true})` (the injected `fs`), sorted directories first then by name, capped at 5000 entries with a final "... N more" row; symbolic links are shown and not followed. Prints `APP dir` / `APP error` lines only when `driven`.
  - `viewer.js`: `load` increments a request counter and remembers its value; opens, `stat`s, reads up to `1 << 20` bytes in 64 KiB `read` calls at explicit positions, closes in `finally`; if the counter has moved on, it stores nothing. Classification: when the file was longer than the cap, first drop up to three trailing bytes that form an incomplete UTF-8 sequence; then text if `require('buffer').isUtf8(bytes)` and no byte is 0, else hex (16 bytes per row). First confirm `isUtf8` exists and works in this runtime with a one-line script, and say so in the progress file; if it does not, stop and report. Lines longer than 4096 characters are cut for display with a marker. Drawn with `listClipper`.
  - `monitor.js`: a `setInterval(250)` (unref'd) samples into `Float32Array(240)` rings; JS time per frame is measured by `frame()` with `process.hrtime.bigint()` around its body.
  - `canvas.js`: exports `orbitPosition(t)` (logical units, relative to the display), used by both the drawing and the test.
  - `log.js`: a ring of the last 1000 entries.
  - `layout.js`: `DEFAULT`, produced by arranging the panels as in the spec's sketch and saving; the README documents the steps.
- [ ] **Step 5: Run both tests; expect PASS.** Mutations: the viewer using `readFile` (`CAP` fails on the recorded method); the read loop not stopping at the cap (`CAP` fails on the sum); `close` moved out of `finally` (`CLOSE_ON_ERROR`); the request counter removed (`SUPERSEDE`, second ordering); classification by searching decoded text for U+FFFD (`CLASSIFY` fails on `fffd.txt`); the trailing-byte trim removed (`CLASSIFY` fails on the straddling file); the Reset Layout load done inside the frame (`RESET` throws `ERR_IMGUI_USAGE`); the canvas drawing's orbit radius changed (`CANVAS`); the panel's throw moved outside the clipper callback is **not** a mutation of the product, so instead: `CallbackScope`'s destructor not restoring the flag (after `THROW`, every later binding call throws `ERR_IMGUI_REENTRANT` and `PANELS` fails).
- [ ] **Step 6: Measure** (spec goal 5). A script beside the test, `test/fixtures/imgui/app-measure.js`, runs the application driven for 600 frames with every panel open and prints the mean and maximum JS time per frame (from `frame()`'s own measurement) and the mean `bindCalls` total per frame. Run it by hand under `utils/with-display.py 120` in `cmake-build-asan` and in `cmake-build-noasan` (Release); record the numbers per build, the machine and the date in the progress file. No threshold, and it is not a lit test.
- [ ] **Step 7: Write `examples/imgui-app/README.md`** (what it shows, how to run it, the panels, what is Node's and what is ImGui's, how the default layout was produced) and add it to `examples/README.md`. Commit: `Add an ImGui application written in JavaScript`.

### Task 10: Documentation, the tracker and the full suite

**Files:**
- Modify: `addons/imgui/README.md`, `CLAUDE.md`, `docs/superpowers/plans/progress-imgui-binding.md`, `dz/issues/*`, `utils/format.sh` only if `addons/imgui/bind` is not already covered by `FORMAT_DIRS` (check; part A added `addons`).

- [ ] **Step 1: `addons/imgui/README.md`:** replace "JS surface" with the calling convention (the spec's table, as built), the exceptions, the error codes, the list of out-of-frame functions, one line per group naming its functions, the audited list of recoverable errors, the guarded preconditions, and "Internal state read by `bind_guards.cpp`" as the "Bumping ImGui" audit list. Update "Not supported" and "Verified, and not". Every statement is checked against the code as built, not copied from the spec.
- [ ] **Step 2: `CLAUDE.md`:** in the "Dear ImGui addon" section, a paragraph on the binding (convention in two sentences, where the surface is listed, the `bind_guards.cpp` rule, the coverage rule and `bind-groups.js`), and `examples/imgui-app` in the Conventions line about examples without npm packages.
- [ ] **Step 3: Tracker.** File one issue for the preconditions not covered (the surface table's "Not covered" section, plus anything the tasks found), one for "behaviours no test detects" from the progress file, one for the draw-list clip rectangle functions left to part C. Each names its "done when".
- [ ] **Step 4: The full suite** in `cmake-build-asan`, in `cmake-build-noasan` (Release: every `imgui-*.js` and `ui-imgui-*.js`), and in `cmake-build-asan-noui` (one invocation at a time). Record the counts.
- [ ] **Step 5: Commit:** `Document the ImGui binding`.

### Task 11 (controller): the Mac

Not for an implementer subagent. After the final review and Codex's code review: push the head to the channel branch, post a request for the build, every `imgui-*.js` and `ui-imgui-*.js` with `-a` for the pixel tests, the application's measurement of Task 9 Step 5 in the Mac's Debug build, the scale `ui-imgui-app.js` printed (2 is expected there), and the whole suite; fix what it finds through implementer subagents on Linux; record the round in the progress file. Then the two hand checks the design asks for: ask the user to run `examples/imgui-app/app.js` once on Linux and once on the Mac and say what they saw; record each with its date, or record that it was not done.
