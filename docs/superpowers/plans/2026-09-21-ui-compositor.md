# UI Compositor Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build `hermes-node-ui`, a separate embedder binary in which `sokol_app` owns the process main thread and presents, while a spawned thread runs `runHermesNode()` and performs every drawing GPU call into offscreen surfaces handed back for presentation.

**Architecture:** Two threads and five channels. The JS thread renders into one of three rotating surfaces per generation; the main thread blits the newest completed one at each `sokol_app` frame callback. `sokol_gfx` is compiled twice — once normally for the JS thread, once under renamed symbols for the presenting thread. Frames follow browser `requestAnimationFrame` semantics.

**Tech Stack:** C++17, Objective-C++ on macOS, Sokol (`sokol_app.h`, `sokol_gfx.h`) pinned at `c0db757`, N-API, libuv, CMake + Ninja, GTest, LLVM lit.

**Spec:** `docs/superpowers/specs/2026-09-18-ui-compositor-design.md`

## Global Constraints

- **Every new file carries `Copyright (c) Tzvetan Mikov.`** and the MIT licence header used elsewhere in this repo. Never "Meta Platforms".
- **All work is behind `HERMES_NODE_ENABLE_UI`, default OFF.** Develop in `cmake-build-asan` (Debug, Clang, ASAN). Always Clang, never GCC.
- **Before every commit:** `./utils/format.sh -f` then `cmake --build cmake-build-asan --target check-hermes-node`.
- **Commit messages: ASCII only, no emojis.**
- **Sokol pinned at `c0db757`**, vendored at `external/sokol/sokol`. One patch is required (Metal command-buffer accessor); two are contingent (GLX config filter, status-returning clipboard read).
- **v1 platforms:** Linux GL (X11, GLX, desktop GL 4.3 core) and macOS Metal. EGL and GLES3 are refused at configure time. No Windows/D3D11.
- **`sokol_gfx` calls happen only on the thread owning that instance's context.** JS thread owns `sg_*`; main thread owns `sgp_*`.
- **CMake conventions of this repo:** the Python interpreter variable is **`${Python_EXECUTABLE}`** (`find_package(Python COMPONENTS Interpreter)`, `CMakeLists.txt:80`). Unit tests are registered with **`add_node_compat_unittest(Name Name.cpp)`** followed by `target_link_libraries` (`unittests/CMakeLists.txt:9`).
- **lit substitutions that exist** are `%hermes-node`, `%hermes`, `%FileCheck`, `%not`, `%source_dir`, `%hello_addon`, `%kit_dir`. **`%python` does not exist**; any new substitution is added in the task that first needs it, along with the CMake `--param` that supplies it.
- **No timing-based assertions.** Assert from `HERMES_NODE_DEBUG_NATIVE=UI` tracing; the suite runs 16-way parallel.
- **Every test must fail if the invariant it names is removed.** Each test step below states the mutation that must break it; verify that by making the mutation, watching the test fail, then reverting.

## Recorded deviation from the spec

The spec says slot states and the published index "live in one atomic word" with a CAS retry loop (`spec:445`). **This plan implements them under a single mutex instead**, because the transitions are compound — displacement must read the read-count and set the state *together*, or a completion that lands in between leaves a slot stranded in RETIRING with nobody left to free it. Separate atomics cannot express that; one word could, but only by packing read counts into bitfields, which trades a provable rule for a fragile one.

The cost is not one uncontended lock: three threads reach this table — the JS producer, the presenting thread, and whichever thread a GPU completion handler runs on — so these are several short but genuinely contended critical sections per frame per thread. What is bounded is their content, not their contention: each is a handful of field updates over a fixed-size table, with no allocation, no I/O and no GPU call inside, and generation destruction is handed to the owner dispatcher after the lock is released rather than run under it. **Task 2 includes a step that corrects the spec**, so the two documents do not disagree.

---

## File Structure

| Path | Responsibility |
|---|---|
| `include/hermes/node-compat/compositor/surface_table.h`, `lib/compositor/surface_table.cpp` | **The whole shared protocol**: slot states, read counts, generations, retirement, closing. One mutex, one place. |
| `include/hermes/node-compat/compositor/owner_dispatch.h`, `lib/compositor/owner_dispatch.cpp` | Posting work to the JS owner thread from any thread (`uv_async`), used for generation destruction. |
| `include/hermes/node-compat/compositor/tick_machine.h`, `lib/compositor/tick_machine.cpp` | rAF pacing, integrated with the surface table so publication and tick state move together. |
| `include/hermes/node-compat/compositor/channels.h`, `lib/compositor/channels.cpp` | Input queue, command queue, per-request clipboard slots. |
| `include/hermes/node-compat/ui/hnui_abi.h` | The public C ABI structs and enums, POD only. Included by `hnui.h` and by addons. |
| `include/hermes/node-compat/ui/hnui.h` | The public C API functions. |
| `lib/compositor-gpu/backend.h` | The backend interface both GL and Metal implement. Defined once, before either. |
| `lib/compositor-gpu/gl_backend.cpp`, `lib/compositor-gpu/metal_backend.mm` | The two implementations. |
| `lib/compositor-gpu/test_hooks.{h,cpp}` | Frame capture, synthetic event injection, controllable completion. Built always, enabled by environment variable. |
| `tools/hermes-node-ui/main.cpp`, `ui_host.{h,cpp}`, `ui_module.cpp`, `ui_platform.cpp`, `ui_test_hooks.cpp`, `ui_internal.h`, `hnui_api.cpp` | Binary entry, thread/lifecycle host, the `hermes-node:ui` module (its JS-thread surface, its platform-thread half, its test hooks, and the declarations those three share), the C API implementation. |
| `test/fixtures/hnui-demo/` | Reference addon: visual, fixed-pattern, native-tick and conformance modes. |
| `unittests/CompositorTest.cpp` | GTest for everything in `lib/compositor/`. |
| `test/ui-*.js` | lit tests, gated on `ui` and `display`. |

## Slices

The tasks are grouped so that **each slice ends with something that runs**, and no task's gate depends on a later task.

- **Slice A (Tasks 1–4): foundations, no GPU.** Build scaffolding, the surface protocol, the owner dispatcher, the tick machine, the channels. All GTest.
- **Slice B (Tasks 5–10): one window presenting one known pattern.** Embedder modules, the binary, the window and shared context, the minimal C ABI, the minimal addon, presenting, the capture hook, the first end-to-end test.
- **Slice C (Tasks 11–14): the rest of the surface area.** Input and commands, clipboard with a real status, full rAF semantics, the full C API and conformance mode. Written out as sub-tasks 11a–11d, 12, 13, 14a–14b.
- **Slice D (Tasks 15–17): lifecycle, Metal, suite.** CLOSING on every exit path, the Metal backend against the same interface, the full gated suite. **Task 15 done (2026-11-19); Tasks 16 and 17 not started.**

**Slice A complete (2026-09-22): 45 tests in `unittests/CompositorTest.cpp`** (49 after the whole-branch review's fixes). Tasks 1–4 are ticked below. Rulings made while executing them are written into the task they belong to, marked **Ruling N (execution)**; the ones that bind later work are Ruling 16 (Task 3 Step 5, carried to Tasks 8/10) and Ruling 9 (Task 2's `take()` behaviour, which Task 4 relies on).

**Slice B complete (2026-09-25): 33 commits and 75 files before the branch was squashed.** Tasks 5–10 are ticked below. The suite stands at 414 unit tests and 201 JS tests. The slice's gate is met: `hermes-node-ui` opens a real window, the reference addon draws a fixed pattern into a compositor surface on the JS thread, the platform thread blits it, and `test/ui-present.js` compares the presented pixels **exactly** — 0 or 255, no tolerance, samples kept 8px from any edge. Obligations this slice carried forward, each written into the task that discharges it rather than into any external note: the `sapp_event` → `UiEvent` conversion's coverage and the dispatch loop's address-stability constraint (Task 11), a backend-local copy of the `sg_view` asserts (Task 16, optional — `HnuiAbiTest` already covers every platform), and the first real execution of the posted destroy callback (Task 17). Progress notes and the newcomer's orientation live in `docs/superpowers/plans/progress-ui-compositor.md`; known defects live in `dz/`, not here.

**Slice C complete (2026-11-13): eight tasks (11a-11d, 12, 13, 14a, 14b), a whole-slice review and one fix wave.** Tasks 11a-14b are ticked below. The slice's gate is met: real key, resize and close events reach native listeners and JavaScript; commands, the clipboard in both directions, full `requestAnimationFrame` semantics and the whole `hnui` C API exist; a resize starts a new generation and the old one is destroyed on the owner thread; and the reference addon has its visual and conformance modes. **What the whole-slice review found is the thing to remember:** no defect in the shipped path, but six mutations of load-bearing lines left every window test green, because every event test injected *below* Sokol and so executed nothing above the injection point. `test/ui-real-events.js`, driven by a second X client (`test/fixtures/hnui-demo/x11_window_driver.c`), is what closed that, and it is the model for Task 15's close handshake. Rulings 60-69, made during execution, are recorded where they bind: in the task text below, in the spec, or in Tasks 15-17. Known defects live in `dz/`.

**Slice D, Task 15 complete (2026-11-19): six tasks (15a-15f) in eight commits.** Written out in `docs/superpowers/plans/2026-11-19-ui-compositor-task15.md`, whose steps are ticked and whose tasks each carry an "As built" note. A window now closes: an uncancelled `close`, `win.close()`, `process.exit()`, an uncaught exception and a runtime that returns all enter CLOSING before any `'exit'` listener runs and end the process from the JS thread with the program's status. The suite stood at 452 unit tests and 224 JS tests, with 4 unsupported; after the fix wave that followed the whole-branch review of Task 15 it stands at 452 unit tests and 225 JS tests, with 4 unsupported. Rulings 70-79 are in the Task 15 paragraph below. Tasks 16 and 17 are not started.

---

### Task 1: Build scaffolding and an empty unit test that runs

**Why first:** every later task's verification command depends on this existing. Task 1 of the previous plan told the executor to build a target nobody had defined.

**Files:**
- Modify: `CMakeLists.txt` (option + subdirectories), `unittests/CMakeLists.txt`
- Create: `lib/compositor/CMakeLists.txt`, `unittests/CompositorTest.cpp`

- [x] **Step 1: Add the option and wire the subdirectory**

```cmake
# CMakeLists.txt, beside the other options
option(HERMES_NODE_ENABLE_UI "Build the hermes-node-ui compositor" OFF)
if(HERMES_NODE_ENABLE_UI)
  add_subdirectory(lib/compositor)
endif()
```

```cmake
# lib/compositor/CMakeLists.txt
# add_hermes_library, not plain add_library: every sibling under lib/ uses
# the macro (hermes/cmake/modules/Hermes.cmake:106), and it carries Hermes's
# compile flags and scope-local defines -- the GC and sanitizer ones among
# them, which miscompile rather than fail to link when they are missing.
add_hermes_library(hermesNodeCompositor STATIC
  surface_table.cpp
)
target_include_directories(hermesNodeCompositor PUBLIC ${PROJECT_SOURCE_DIR}/include)
```

- [x] **Step 2: Register the test target the way this repo does**

```cmake
# unittests/CMakeLists.txt, beside the others
if(HERMES_NODE_ENABLE_UI)
  add_node_compat_unittest(CompositorTest CompositorTest.cpp)
  target_link_libraries(CompositorTest hermesNodeCompositor)
endif()
```

- [x] **Step 3: Write a placeholder test and an empty implementation file**

```cpp
// unittests/CompositorTest.cpp
#include "gtest/gtest.h"
TEST(CompositorScaffold, TargetBuildsAndRuns) { SUCCEED(); }
```

Create `lib/compositor/surface_table.cpp` containing only the licence header and `// Task 2 fills this in.`

- [x] **Step 4: Configure, build and run — the full commands**

```bash
cmake -B cmake-build-asan -G Ninja -DHERMES_NODE_ENABLE_UI=ON
cmake --build cmake-build-asan --target CompositorTest
cmake-build-asan/unittests/CompositorTest
```

Expected: `1 test from 1 test suite ran. [  PASSED  ] 1 test.` **A run reporting zero tests is a failure**, not a pass — check the count every time.

- [x] **Step 5: Format and commit**

```bash
./utils/format.sh -f
git add CMakeLists.txt lib/compositor unittests/CMakeLists.txt unittests/CompositorTest.cpp
git commit -m "Add the compositor build scaffolding"
```

---

### Task 2: The surface protocol

**This is the task the whole design rests on.** Everything compound — displacement, publication, retirement, closing — happens here, under one lock, so no two of them can interleave badly.

**Files:**
- Create: `include/hermes/node-compat/compositor/surface_table.h`
- Modify: `lib/compositor/surface_table.cpp`, `unittests/CompositorTest.cpp`, and `docs/superpowers/specs/2026-09-18-ui-compositor-design.md` (the deviation above)

**Interfaces produced:**

```cpp
using SlotId = uint8_t;
struct GenerationId { uint32_t value = 0; };
/// Generation-qualified. A GPU callback carries this, never a bare SlotId:
/// the slot may have been reused by the time the callback runs.
struct SurfaceRef { GenerationId gen; SlotId slot; };

enum class SlotState { kFree, kWriting, kReady, kPresenting, kRetiring };

class SurfaceTable {
 public:
  /// Proof that the caller holds the lock; only SurfaceTable can make one.
  class LockToken;

  explicit SurfaceTable(std::function<void(GenerationId)> destroyOnOwnerThread);

  GenerationId newGeneration(int width, int height);  ///< retires the previous
  std::optional<SurfaceRef> acquire();                ///< FREE -> WRITING, current gen only
  bool publish(SurfaceRef ref);                       ///< WRITING -> READY; false if retired/closing
  void reclaim(SurfaceRef ref);                       ///< WRITING -> FREE, or RETIRING if GPU work is in flight
  std::optional<SurfaceRef> take();                   ///< READY -> PRESENTING, displaces atomically
  void addRead(SurfaceRef ref);
  void releaseRead(SurfaceRef ref);
  void addWrapper(GenerationId gen);                  ///< Ruling 1: an explicit counted owner
  void releaseWrappers(GenerationId gen);
  void beginGpuWork(SurfaceRef ref);                  ///< Ruling 1: acquired BEFORE submission
  void endGpuWork(SurfaceRef ref);                    ///< gates slot reuse as reads do
  bool wrappersReleasable(GenerationId gen) const;
  void beginClosing();
  bool anyFreeSlot() const;                           ///< a snapshot, not a promise
  SlotState state(SurfaceRef ref) const;              ///< test-only accessor

  /// Run several transitions as one critical section. The callback gets a
  /// token and must call only the *Locked helpers with it; calling a public
  /// method would re-enter the non-recursive mutex and deadlock.
  template <typename Fn> auto withLock(Fn &&fn);
  GenerationId newGenerationLocked(LockToken &, int width, int height);
  /* ... one *Locked helper per transition above ... */
};
```

**Ruling 1 (execution) — the interface above is what was built.** `addWrapper`/`releaseWrappers` and `beginGpuWork`/`endGpuWork` are explicit because spec:465 lists producer GPU work, pending completion handlers and presenting-copy wrappers as generation owners, and **none of them can be inferred from slot state** — a slot can be FREE while its completion handler still has work outstanding.

**`withLock()` plus the `*Locked` helpers and `LockToken` are the transaction mechanism** Task 4's integration rule needs (see Task 4's `publishFrame`/`takeFrame`), and the same mechanism composes the frame-tick gate: `anyFreeSlot()` answers about an instant, so a caller must not read it and then move tick state under a second lock. `TickMachine::shouldSendTick(SurfaceTable &)` is the composed form.

- [x] **Step 1: Write the failing tests, including the race that broke the previous plan**

```cpp
TEST(SurfaceTableTest, DisplacementAndLastReadCompletionCannotStrandASlot) {
  // The previous design read the read-count and then stored RETIRING as two
  // separate atomic operations. Interleaving: displacement observes reads==1;
  // the completion handler decrements to 0 and fails its CAS against
  // PRESENTING; displacement then stores RETIRING, and no completion is left
  // to free it. One lock makes the pair indivisible.
  SurfaceTable table([](GenerationId) {});
  auto gen = table.newGeneration(64, 64);
  auto a = *table.acquire();
  table.publish(a);
  table.take();
  table.addRead(a);

  std::atomic<bool> go{false};
  std::thread completion([&] {
    while (!go.load()) std::this_thread::yield();
    table.releaseRead(a);
  });
  auto b = *table.acquire();
  table.publish(b);
  go.store(true);
  table.take();                      // displaces a, concurrently with release
  completion.join();

  // Whichever order won, a must end FREE and be acquirable again.
  EXPECT_EQ(table.state(a), SlotState::kFree);
  auto reacquired = table.acquire();
  EXPECT_TRUE(reacquired.has_value());
}
// Mutation that must break it: split the take()/releaseRead() critical
// sections into separate locks. Run 1000 iterations; it strands a slot.

TEST(SurfaceTableTest, PublishRefusesARetiredGeneration) {
  SurfaceTable table([](GenerationId) {});
  table.newGeneration(64, 64);
  auto a = *table.acquire();
  table.newGeneration(128, 128);     // retires a's generation
  EXPECT_FALSE(table.publish(a)) << "a retired generation may not publish";
  table.reclaim(a);
}
// Mutation: drop the generation check in publish(). Test fails.

TEST(SurfaceTableTest, PublishRefusesWhileClosing) {
  SurfaceTable table([](GenerationId) {});
  table.newGeneration(64, 64);
  auto a = *table.acquire();
  table.beginClosing();
  EXPECT_FALSE(table.publish(a));
}

TEST(SurfaceTableTest, RetiredGenerationStillTakesNewReads) {
  SurfaceTable table([](GenerationId) {});
  table.newGeneration(64, 64);
  auto a = *table.acquire();
  table.publish(a);
  table.take();
  table.newGeneration(128, 128);     // retires, but a is still on screen
  table.addRead(a);                  // the main thread re-presents it
  EXPECT_EQ(table.state(a), SlotState::kPresenting);
  table.releaseRead(a);
  EXPECT_EQ(table.state(a), SlotState::kPresenting)
      << "releasing a read never frees a PRESENTING slot";
}
// Mutation: make retirement forbid addRead. Test fails, and so would the
// screen -- it would go blank at every resize.

TEST(SurfaceTableTest, WrappersHoldUntilRetiringReadsDrain) {
  SurfaceTable table([](GenerationId) {});
  auto gen = table.newGeneration(64, 64);
  auto a = *table.acquire();
  table.publish(a);
  table.take();
  table.addRead(a);                  // outstanding blit on the old surface
  table.newGeneration(128, 128);
  auto b = *table.acquire();
  table.publish(b);
  table.take();                      // a -> RETIRING with one read
  EXPECT_FALSE(table.wrappersReleasable(gen))
      << "a RETIRING slot still needs its wrapper";
  table.releaseRead(a);
  EXPECT_TRUE(table.wrappersReleasable(gen));
}
// Mutation: count only PRESENTING, not RETIRING. Test fails -- this is the
// defect finding 7 caught.

TEST(SurfaceTableTest, DestructionIsPostedNotCalledInline) {
  std::thread::id destroyThread;
  SurfaceTable table([&](GenerationId) { destroyThread = std::this_thread::get_id(); });
  auto gen = table.newGeneration(64, 64);
  auto a = *table.acquire();
  table.publish(a);
  table.take();
  table.newGeneration(128, 128);
  std::thread other([&] {
    auto b = *table.acquire();
    table.publish(b);
    table.take();                    // drops the last reference to gen
  });
  other.join();
  EXPECT_NE(destroyThread, std::thread::id{})
      << "destruction must be handed to the owner dispatcher, not skipped";
}

TEST(SurfaceTableTest, ThreeSlotsExhaustAndAcquireReportsIt) {
  SurfaceTable table([](GenerationId) {});
  table.newGeneration(64, 64);
  auto a = *table.acquire();
  table.publish(a);
  table.take();
  table.addRead(a);
  auto b = *table.acquire();
  table.publish(b);
  auto c = table.acquire();
  ASSERT_TRUE(c.has_value());
  EXPECT_FALSE(table.acquire().has_value());
  EXPECT_FALSE(table.anyFreeSlot()) << "the tick must be withheld here";
}

TEST(SurfaceTableTest, ProducerConsumerStressCompletesRealCycles) {
  // The previous stress test could pass having done almost nothing. Count
  // completed cycles and require progress.
  SurfaceTable table([](GenerationId) {});
  table.newGeneration(64, 64);
  std::atomic<bool> stop{false};
  std::atomic<int> taken{0}, violations{0};

  std::thread consumer([&] {
    std::optional<SurfaceRef> held;
    while (!stop.load(std::memory_order_acquire)) {
      if (auto t = table.take()) {
        table.addRead(*t);
        if (table.state(*t) == SlotState::kWriting) violations.fetch_add(1);
        taken.fetch_add(1);
        // Hold the read across the NEXT take, which is what exercises
        // displacement concurrent with completion.
        if (held) table.releaseRead(*held);
        held = t;
      }
    }
    if (held) table.releaseRead(*held);
  });

  int published = 0;
  for (int i = 0; i < 200000; ++i) {
    if (auto w = table.acquire()) {
      if (table.state(*w) == SlotState::kPresenting) violations.fetch_add(1);
      if (table.publish(*w)) ++published;
    }
  }
  stop.store(true, std::memory_order_release);
  consumer.join();
  EXPECT_EQ(violations.load(), 0);
  EXPECT_GT(published, 1000) << "the producer must have made real progress";
  EXPECT_GT(taken.load(), 100) << "the consumer must have taken real frames";
}
```

- [x] **Step 2: Run to verify they fail**

```bash
cmake --build cmake-build-asan --target CompositorTest && \
  cmake-build-asan/unittests/CompositorTest --gtest_filter='SurfaceTableTest.*'
```

Expected: compilation failure — `surface_table.h` does not exist. (The build must be part of the command; running a stale binary proves nothing.)

- [x] **Step 3: Implement** — one `std::mutex` guards **a slot array per generation** (state, read count, GPU-work count), the ready/presenting references, the generation table (reference counts, current flag) and the closing flag.

  **Ruling 6 (execution) — per-generation slot storage, not one global `Slot[3]`.** The resize tests require an old PRESENTING or RETIRING slot to coexist with three slots of the new generation (spec:464), which a single flat array cannot express. This plan's original "one `std::mutex` guards a `Slot[3]` array" described the lock correctly and the arity wrongly; the arity follows the spec. Generations are held in a `std::map`, whose node stability is load-bearing — several transitions hold a `Generation *` across a call that can erase a *different* generation.

  **Ruling 9 (execution) — `take()` still accepts a retired generation's READY frame.** A frame published just before a resize is shown rather than dropped: the producer's work is already done and the presenter stretches it. The cost is paid in `wrappersReleasable()`, which must therefore count **READY** as well as PRESENTING and RETIRING, or the presenting instance destroys a wrapper the very next `take()` needs. Refusing the frame instead would drop finished work to solve a wrapper-lifetime problem that a conservative count solves for free. Task 4 may rely on this behaviour. Every public method takes that lock for its whole compound transition. `take()` moves READY→PRESENTING **and** displaces the previous slot in the same critical section. `releaseRead()` frees only from RETIRING. Destruction is never called with the lock held and never inline: it is handed to the owner dispatcher (Task 3), which Task 2 accepts as a `std::function` so it can be tested with a fake.

- [x] **Step 4: Run to verify they pass**

```bash
cmake --build cmake-build-asan --target CompositorTest && \
  cmake-build-asan/unittests/CompositorTest --gtest_filter='SurfaceTableTest.*'
```

Expected: PASS, 8 tests, `[  PASSED  ] 8 tests.`

- [x] **Step 5: Verify each mutation actually breaks its test** — make the mutation named under each test, confirm that test fails, revert. A test that passes under its mutation is not a test.

  **Ruling 13 (execution) — timing sweeps do not work in this suite; assert end states under load instead.** Measured, not assumed: a sweep that fires one release at one displacement caught a *yield-widened* split twice and then let the identical mutation through after unrelated timing shifts, and never caught a bare split (two critical sections back to back, tens of nanoseconds) in 4000 rounds. Thread wake-up latency is orders of magnitude larger than the window being swept, so where a round lands is luck rather than aim. The replacement — let threads drift under load and assert an **end state** that a defect makes permanent (a stranded slot stays stranded; a releasable generation stays releasable) — catches the bare split. The two sweeps stay as cheap single-shot regression checks, with comments saying they pin nothing and naming the load test that does, and with round counts trimmed to match what they actually buy. **Write no new test that tries to hit a narrow window by aiming at it.**

- [x] **Step 6: Correct the spec's atomic-word paragraph** — replace `spec:445`'s "one atomic word ... CAS retry loop" with the mutex rule and the reason (compound transitions), citing this plan's "Recorded deviation" section.

- [x] **Step 7: Format and commit**

```bash
./utils/format.sh -f
git add include/hermes/node-compat/compositor/surface_table.h lib/compositor \
        unittests/CompositorTest.cpp docs/superpowers/specs/2026-09-18-ui-compositor-design.md
git commit -m "Add the compositor surface protocol under one lock"
```

---

### Task 3: Owner-thread dispatcher

**Files:** create `include/hermes/node-compat/compositor/owner_dispatch.h`, `lib/compositor/owner_dispatch.cpp`; modify `lib/compositor/CMakeLists.txt`, `unittests/CompositorTest.cpp`.

**Interfaces produced:** `class OwnerDispatch { OwnerDispatch(uv_loop_t *loop); bool post(std::function<void()> fn); void drainForTest(); void close(); }` — `post` is callable from **any** thread (`uv_async_send`), the function runs on the loop thread.

**Ruling 4 (execution) — `post()` returns `bool`, not `void`.** It returns false, having enqueued nothing, once `close()` has run: queued work is GPU-resource destruction, so a caller whose work is destruction-critical has to be able to find out that it will not run rather than have it silently dropped. `close()` is owner-thread-only, and **drains to a fixpoint rather than discarding** — a function it runs may itself post another (a cascading generation destruction), and that post must still be accepted. The extraction rule applies here exactly as in `SurfaceTable`: take the lock, move the work out, unlock, *then* invoke — never call a callback with a lock held (precedent: `lib/event-loop/uv_event_loop.cpp:241`).

- [x] **Step 1: Write the failing test** — post from three threads, assert every function ran **on the loop thread** (compare `std::this_thread::get_id()` captured inside against the loop thread's id), that none ran inline on the posting thread, and that `close()` after posts still drains or discards deterministically without leaking.
  *Mutation that must break it:* call `fn()` directly inside `post()`.
- [x] **Step 2: Run to verify it fails** (full build + run command, as Task 1).
- [x] **Step 3: Implement** with `uv_async_t` plus a mutex-protected queue; `uv_async_send` is the one libuv call that is safe from any thread.
- [x] **Step 4: Run to verify it passes.**
- [x] **Step 5: Wire it into `SurfaceTable`** — Task 2's `destroyOnOwnerThread` becomes `[&](GenerationId g){ dispatch.post([g]{ ... }); }` in production code; the unit tests keep their fake.

  **Ruling 16 (execution) — this step was deliberately left UNDONE, and its obligation transfers to Slice B.** Nothing in this repository constructs a `SurfaceTable` outside `unittests/CompositorTest.cpp`; the first production call site arrives with the UI module (Task 8/10). Writing the wiring here would have meant fabricating a production site — dead code that later has to be found and undone. **The Slice B task that first constructs a `SurfaceTable` owns this step**, and must pass `dispatch.post(...)` as the destruction callback rather than a fake. Destruction frees GPU textures, and the thread that drops a generation's last reference is whichever one happened to run a completion handler, so an inline callback would free textures off the owner thread.
- [x] **Step 6: Format and commit** (`git commit -m "Post generation destruction to the owner thread"`).

---

### Task 4: Tick machine and channels

**Files:** create `tick_machine.{h,cpp}`, `channels.{h,cpp}`; modify `CompositorTest.cpp`.

**The integration rule finding 5 caught:** publication and tick state must move together, or the main thread can take a surface and call `taken()` before `published()` runs, leaving the machine waiting for a take that already happened. So **the tick machine does not expose `published()`/`taken()` as free-standing calls**. It exposes:

```cpp
/// Called by the producer with the table, so publication and the tick
/// transition happen under the table's lock, in one step.
bool publishFrame(SurfaceTable &table, SurfaceRef ref);   // false -> dropped
/// Called by the main thread from frame_cb, likewise.
std::optional<SurfaceRef> takeFrame(SurfaceTable &table);
```

**Ruling 5 (execution) — "the seven from the previous plan" was an unrecoverable reference and is replaced by the list below.** That text lived only in the first version of this plan, which the slice-based rewrite replaced, and no fresh executor could reach it. The seven are: an idle app receives no ticks; requests coalesce; the tick is withheld while no slot is free; an empty tick returns to idle immediately; a dropped publication returns to idle rather than awaiting a take; a callback registered inside a callback runs next tick; no ticks while minimized. They were reproduced in Task 4's dispatch brief and are implemented as the `TickMachineTest` cases of those names.

- [x] **Step 1: Write the failing tests** — the seven listed in Ruling 5 above **plus**: a dropped publication with a request recorded during GPU_PENDING still ticks afterwards (*mutation: clear pending requests in `publicationDropped()`*); a request made during `kTickQueued` and during `kGpuPending` both survive (the old test covered only `kCallbacksRunning`); `publishFrame` followed immediately by `takeFrame` on another thread never leaves the machine in `kPublishedAwaitingTake`.
- [x] **Step 2–4: fail, implement, pass** as before.
- [x] **Step 5: Channels** — `InputQueue` with mouse-move coalescing (position latest, deltas summed, non-move events break the run); `CommandQueue` carrying a `Command` variant whose payloads are owned (`std::string` for title, RGBA vector for cursor images); **`ClipboardRequest` as a retained, reference-counted per-request slot** carrying an id, so a late reply to a timed-out request A cannot satisfy a later request B and cannot touch freed storage.
  *Test that must exist:* A requests and times out, B requests, A's reply arrives — B must still be pending, and nothing must leak or crash under ASAN.

**Ruling 21 (execution) — the composition rule is pinned by a deterministic test, not by the threaded one.** `PublishThenImmediateTakeNeverStrandsAwaitingTake` does **not** catch its own named mutation: splitting `publishFrame` into `table.publish()` plus a separate state store was applied, confirmed present in the source, and survived all iterations — the window between the two halves is a few instructions while `std::thread` startup is tens of microseconds, so the consumer always arrives after both. (Same dead end as Ruling 13.) What pins the rule is `PublishFrameUpdatesTheMachineWithinTheTablesTransaction`, which uses no threads at all: `SurfaceTable` hands a dead generation to the destroy function at the *end* of a transaction, so that callback is a deterministic observation point sitting exactly where a split implementation leaves its gap — composed observes `kPublishedAwaitingTake`, split observes `kGpuPending`. The threaded test stays for its end-state invariant over both orderings; its comment says what it is and is not worth.
- [x] **Step 6: Format and commit.**

---

### Task 5: Embedder modules in the runtime

**Files:** modify `include/hermes/node-compat/runtime/hermes_node_runtime.h`, `lib/runtime/hermes_node_runtime.cpp`, `libjs/loader.js`, `lib/bundle/bundle_resolve.cpp`, `libjs/bundle-loader.js`, `tools/hermes-node/hermes-node.cpp`; create `test/embedder-module.js`, `test/bundle-embedder-module.js`.

- [x] **Step 1: Add the CLI flag first** — `--test-embedder-module`, guarded by `HERMES_NODE_ENABLE_UI`, registering a module named `hermes-node:test` that exports `{ ok: true }`. Without this step the test's first failure is "unknown option", not module resolution, which is the defect finding 21 caught.
- [x] **Step 2: Write the failing lit test** — `require('hermes-node:test')` returns `{ok:true}`; `require('hermes-node:missing')` throws with `code === 'MODULE_NOT_FOUND'`.
- [x] **Step 3: Run to verify it fails** with `Cannot find module 'hermes-node:test'`.
- [x] **Step 4: Implement** `EmbedderModule{name, init}` plus `HermesNodeConfig::embedderModules`, resolved in the loader before any filesystem lookup.
- [x] **Step 5: Bundle passthrough** — `isBuiltinSpecifier` (`lib/bundle/bundle_resolve.cpp:406`) and `libjs/bundle-loader.js` both pass `hermes-node:*` through; test producer and consumer.
- [x] **Step 6: Run the whole suite, format, commit.**

---

### Task 6: The binary, the host, and the windowless path

**Files:** create `tools/hermes-node-ui/{main.cpp,ui_host.h,ui_host.cpp,CMakeLists.txt}`; create `test/ui-windowless.js`; modify `test/lit.cfg` and the test CMake target.

- [x] **Step 1: Add the `ui` lit feature and the substitution** — `%hermes-node-ui`, supplied by a CMake `--param`, with the feature decided by **running** `hermes-node-ui --version`. Add the binary to the test target's dependencies. Without this, every later `REQUIRES: ui` test is silently UNSUPPORTED rather than failing for its stated reason (finding 16).
- [x] **Step 2: Write the failing test** — a script that never opens a window sets `process.exitCode = 7` and the binary exits 7.
- [x] **Step 3: Implement** — `main()` parses only what v1 needs (script path, `--bundle=`, `--version`; hermes-node's own parser stays in `main()` in `tools/hermes-node/hermes-node.cpp` — line 636 as of this writing, so search for the symbol rather than trusting the number — and is **not** reused), spawns the JS thread with an explicit stack size via `uv_thread_create_ex`, and waits on a condition variable whose predicate is **"a window was requested OR the runtime finished"**.
- [x] **Step 4: Run to verify it passes. Step 5: format and commit.**

---

### Task 7: The C ABI header

**Before any addon or API implementation exists**, because Task 8's minimal addon and Task 14's full API both need these definitions, and the spec deliberately gives prototypes only (finding 12).

**Files:** create `include/hermes/node-compat/ui/hnui_abi.h`.

- [x] **Step 1: Define the POD layouts** — `hnui_target`, `hnui_event`, `hnui_window_state`, `hnui_cursor`, `hnui_status`, the callback typedefs, and `HNUI_ABI_VERSION`. Every struct starts with `uint32_t size`, and the header defines each struct's **minimum accepted size** as a named constant. **`UiEvent` from Task 4 is internal** — it holds `std::string`/`std::vector` and cannot cross the C boundary; the conversion to `hnui_event` (borrowed `const char *` valid for the callback's duration) is written here and unit-tested.
- [x] **Step 2: Write the conversion test** — an internal event with text and paths converts to a `hnui_event` whose pointers are valid during dispatch; a `size` below the minimum is rejected.
- [x] **Step 3: Implement, run, format, commit.**

---

### Task 8: Window, shared context, and the startup requirements

**Files:** create `lib/compositor-gpu/{CMakeLists.txt,backend.h,gl_backend.cpp}`, `tools/hermes-node-ui/ui_module.cpp`; modify `CMakeLists.txt`; create `test/ui-create-window.js`. Also, per **Ruling 33 (execution)**, create `external/sokol/{README.md,CMakeLists.txt}` and vendor upstream into `external/sokol/sokol`; per **Ruling 34 (execution)**, modify `test/lit.cfg`.

**Ruling 33 (execution): this task vendors Sokol.** The Global Constraints assume `external/sokol/sokol` exists, but no task created it and none did — `external/` holds ten other dependencies and nothing in CMake references sokol. This is the first task that needs it, so it vendors it here, at the pinned `c0db757`, following this repo's convention for unmodified dependencies: outer `external/sokol/` holds a README and the wrapper CMake, inner `external/sokol/sokol/` is untouched upstream source. Network reachability was confirmed before this ruling was made.

**Ruling 34 (execution): the `display` lit feature is introduced HERE, not in Task 17.** Step 4 is the first test that needs a window, and this plan's own Global Constraints say a new substitution is added in the task that first needs it. Deferring it to Task 17 would make `ui-create-window.js` silently UNSUPPORTED rather than failing for its stated reason — the same defect finding 16 names for the `ui` feature, and the same class of hole as the CI gap that let Slice A's 45 tests never run. The RUN line must actually start Xvfb rather than merely detect that `xvfb-run` exists. Verified available on the development machine: `Xvfb`, `xvfb-run`, `glxinfo`, and llvmpipe reporting GL 4.5 core — above the 4.3 core v1 requires.

**Ruling 35 (execution): Step 3 writes the rename generator from scratch.** It says "fix and write"; there is nothing to fix. No such generator exists in `utils/` (which holds only `build-portable.sh`, `format.sh` and `make-kit.py`).

**Ruling 45 (execution), carried from Task 7: this task must PROVE that `hnui_gpu_view` matches Sokol's `sg_view`.** Task 7 had to define that struct before Sokol existed in the tree, so it is a hand-written inference from Sokol's universal `typedef struct sg_X { uint32_t id; } sg_X;` convention — not a copy of anything. It could not be checked at the time: no `sg_view` was available anywhere to compare against. An ABI struct that silently disagrees with the real upstream type is the worst failure this header can produce, because addons compile against it. **The moment Sokol is vendored in Step 2, add to a translation unit that includes BOTH `sokol_gfx.h` and `hnui_abi.h`** (`gl_backend.cpp` is the natural home): `static_assert` on `sizeof(hnui_gpu_view) == sizeof(sg_view)`, on `alignof` equality, and on `offsetof(hnui_gpu_view, id) == offsetof(sg_view, id)` with the two `id` members the same size. A comment will not survive a Sokol bump; a `static_assert` will. Also assert the range assumption behind `hnui_target::color_format` being `int32_t`: Sokol terminates `sg_pixel_format` with `_SG_PIXELFORMAT_FORCE_U32 = 0x7FFFFFFF`, so the values fit, but nothing currently says so.

- [x] **Step 1: Define the backend interface** (`backend.h`) that **both** GL and Metal implement: `createSharedContext`, `makeCurrentOnWorker`, `createGenerationSurfaces`, `wrapForPresenting`, `blit`, `submitAndTrackCompletion`, `releaseWrappers`, `presentInvalid`. Metal fills it in Task 16; defining it now prevents the previous plan's defect of a "Metal task" that implemented only completion (finding 15).
- [x] **Step 2: Define the real GPU CMake target** — `hermesNodeCompositorGpu`, its sources, the generated rename header, `${Python_EXECUTABLE}`, X11/GL libraries, and the `check-sgp-isolation` target. (Finding 10: the previous plan consumed this target without creating it.)
- [x] **Step 3: Fix and write the rename generator** — sorted output, and the lit test checks **`CHECK-DAG`** for two representative names plus a count of the whole file, not ordered `CHECK`s that contradict the sort (finding 11). The count assertion is `>= 156` so Task 16's added accessor does not break it.
- [x] **Step 4: Write the failing window test** — `createWindow` resolves, `framebufferWidth > 0`, a second `createWindow` **throws**, and the process exits 0.
- [x] **Step 0 (carried from Task 3, Ruling 16): wire `OwnerDispatch` into the `SurfaceTable` this task constructs.** If this is the first task to construct a production `SurfaceTable`, its `destroyOnOwnerThread` argument **must** be `[&](GenerationId g){ dispatch.post([g]{ ...free the generation's textures... }); }` — never a fake, never an inline free. Task 3 Step 5 was deliberately left undone because no production call site existed then; this is where that debt is paid. If Task 10 constructs one first, the step belongs there instead. **It did.** Task 8 deliberately constructed none — a window needs no surfaces, and building one purely to carry the wiring would have fabricated exactly the call site Ruling 16 told Task 3 not to fabricate — so this was discharged in Task 10, whose `onSharedContextReady()` constructs the first production `SurfaceTable` with its destroy callback going through `dispatch.post(...)`. Ticked here because the obligation is closed, not because this task closed it. Note that the callback has still never *fired*: this build allocates one generation and never retires it, which is why Task 17 carries the requirement to execute it.
- [x] **Step 5: Implement the startup requirements in full** (finding 13): take the referenced libuv handle **when `createWindow` is called**; install the Sokol logger that **terminates non-zero** on a panic item; stage 1 decides the context route **before `sapp_run()`** (Sokol ranks its config and creates its context there, `sokol_app.h:14272-14274`); on shared-context failure, post the rejection and **keep the main loop running until JS has observed it**; refuse an unsupported route with a message rather than degrading.
- [x] **Step 6: Run, format, commit.**

---

### Task 9: Test hooks

**Before their consumers** (finding 18). **Files:** create `lib/compositor-gpu/test_hooks.{h,cpp}`; modify `backend.h`.

- [x] **Step 1: Define and implement three hooks**, compiled always and enabled only by environment variable: **frame capture** (request carries generation, **the publication id** (Ruling 49, below), target size, framebuffer size, DPI scale **and the input-state serial the frame was drawn from**; the capture is taken only when all agree, and is acknowledged); **synthetic event injection** into `event_cb`; **controllable completion**, which lets a test hold and release producer/consumer completion deterministically.
- [x] **Step 2: Unit-test controllable completion against `SurfaceTable`** — hold producer completion, assert nothing publishes; release, assert publication; hold consumer completion across a displacement, assert RETIRING, release, assert FREE. This is the GPU-ordering coverage the spec asks for, obtained without a GPU.
- [x] **Step 3: Run, format, commit.**

---

### Task 10: Present a known pattern end to end

**Files:** create `test/fixtures/hnui-demo/{demo.js,demo_addon.c}`, `tools/hermes-node-ui/hnui_api.cpp` (minimal subset: `hnui_get_window`, `hnui_frame_target`, `hnui_request_animation_frame`); modify `gl_backend.cpp`; create `test/ui-present.js`.

**This is Slice B's payoff: the first task whose gate is a picture.**

- [x] **Step 1: Implement the minimal C API subset** — only what the fixed-pattern addon needs. The full surface comes in Task 14. **If Task 8 did not already construct the production `SurfaceTable`, Ruling 16's carried obligation lands here**: the destruction callback is `dispatch.post(...)`, not a fake. Gate the frame tick through `TickMachine::shouldSendTick(SurfaceTable &)` — the composed overload — rather than `anyFreeSlot()` followed by `shouldSendTick(bool)`.
- [x] **Step 2: Write the fixed-pattern addon and `demo.js`.**
- [x] **Step 3: Implement surfaces and presenting** — immutable storage (`glTexStorage2D`), wrappers created once per generation via `gl_textures[0]`, a full-screen triangle with linear filtering, stretching when sizes differ, and a clear colour while nothing has ever been published.
- [x] **Step 4: Write the capture test** using Task 9's hook, with the agreement rule and tolerance policy. **Carried from Task 9 (Ruling 49): this task must PRODUCE the publication id, or the capture key ships as a constant zero.** Task 9 added `publicationId` to `CaptureRequest` and compares it, but nothing in the tree assigns one — `SurfaceTable` has generation ids and no publication counter. The failure mode is silent: leave it unset and request and observation are both 0, they agree, matching degrades to exactly the pre-fix behaviour, and every test stays green. So: increment a monotonic counter at the one place a publication is linearized — the success branch of `TickMachine::publishFrame()`, already inside the table's lock — and hand that value to whatever fills a `CaptureRequest` at present time. **Start it at 1**, exactly as `SurfaceTable`'s generation ids do and for the same reason, so 0 remains distinguishable from "never set" and a forgotten wiring fails loudly instead of matching everything. Then assert the case spec:977 exists for: a re-blit of the still-presenting surface must **not** satisfy a request for the next publication. Without that assertion the field is still untested against the only scenario it was added for.
- [x] **Step 5: Add GL completion** — `glFenceSync` + **`glFlush`** (GL `sg_commit` does not flush), ownership transferring to the main thread, `GL_WAIT_FAILED` quarantining the slot and ending the session, and the **stage-2 startup probe** selecting polling or blocking, with an environment override and tracing.
- [x] **Step 6: Run both completion modes** and assert from tracing, never timing. **Step 7: format and commit.**

---

## Slice C, written out (Tasks 11–14)

Written out 2026-11-13, against the tree as Slice B left it (baseline green: 414 unit tests, 203 JS tests, 3 unsupported). Each of the four outlined tasks turned out to hold more than one reviewable deliverable, so they are split into lettered sub-tasks. **"Task 11" elsewhere in this plan and in the progress file means the group 11a-11d**, and likewise 14a-14b; the numbers are kept so the carried obligations that name them still resolve.

### Slice C pre-flight rulings

Decisions made while expanding the slice, each from something read in the tree rather than assumed. They bind every task below.

- **Ruling 50 — touch events are cut from v1, and the spec is corrected.** `sokol_app` produces `TOUCHES_*` only on iOS, Android and Emscripten (`sokol_app.h:7200`, `:8143`, `:10910`); neither v1 platform can emit one. Carrying `touchstart`/`touchmove`/`touchend`/`touchcancel` through the converter, the ABI and the JS API would be four event names with no possible source and no possible test. Task 11a Step 6 removes them from the spec's JS API list and adds the line to "What this does not do".
- **Ruling 51 — the public event ABI is extended in place.** `HNUI_ABI_VERSION` 1 has never shipped, so the rule in `hnui_abi.h` that a minimum-size constant is never redefined does not yet bind: there is no addon compiled against the old one. Task 11a appends five event types and one field and moves `HNUI_EVENT_MIN_SIZE` to the new last field. **This is the last task allowed to do that**; the header's rule applies from the first release.
- **Ruling 52 — every `UiEvent` carries a `WindowState` snapshot**, sampled on the platform thread when the event is produced. The JS thread assigns that snapshot to its mirror before any listener for the event runs. This is how "window state travels with the events that change it" (spec: Input and platform services) is met without a second channel, and it is what makes the mirror correct even for events that arrived before the window's Promise resolved.
- **Ruling 53 — the window object is built natively, from `require('events')`.** No JavaScript file is added to the embedded module set for it: that set is compiled into every `hermes-node`, and a command-line build must not carry UI code. `globalThis.require('events')` is reachable in every mode, bundles included (`libjs/shims/domain.js` depends on the same route).
- **Ruling 54 — `QUIT_REQUESTED` is cancelled and forwarded as `close` in Task 11b; what an uncancelled `close` does is Task 15's.** Until Task 15, clicking the window's close button emits `close` and the window stays open; a script ends itself with `process.exit()` from that listener. That replaces today's behaviour (the window closes and the process hangs, dz `01a0d88a-6518`) with one that is at least observable, and the issue stays open for Task 15. *(Superseded by Task 15: an uncancelled `close` now ends the process, and the issue is closed.)*
- **Ruling 55 — the clipboard read is our own X11 read, not a Sokol patch.** The spec allows either. A patch needs a patch-carrying mechanism, which does not exist and which the spec says is settled by the first *required* patch (the Metal accessor, Task 16). The read needs only `sapp_x11_get_display()` and `sapp_x11_get_window()`, both public (`sokol_app.h:2290-2292`). On macOS `sapp_get_clipboard_string()` reads `NSPasteboard` directly and has no stale-buffer path, so Task 16 uses it as is.
- **Ruling 56 — the frame timestamp's origin is `process.uptime()`'s, and the spec is corrected.** The spec says "sharing `performance.now()`'s origin", but this runtime has no `performance` global at all (measured: `typeof performance === 'undefined'`). `process.uptime()` is the monotonic clock the runtime does have, with a defined origin (`lib/process/node_process.cpp:509`). Task 13 Step 7 corrects the spec.
- **Ruling 57 — native and JS frame callbacks share one id space and one list.** A single registration-ordered list is what makes "run in registration order among them" (spec: The native C API) definable at all; two lists with two counters cannot express it.
- **Ruling 58 — entry points land with the mechanism they expose, not in a final sweep.** `hnui_add_event_listener` and `hnui_get_window_state` arrive in 11b, `hnui_read_clipboard_sync` and `hnui_free` in 12, `hnui_cancel_animation_frame` in 13. Task 14a adds what is left. A mechanism with no caller cannot be tested, and Task 10 set the precedent.
- **Ruling 59 — every lit test in this slice ends through `process.exit()` and every RUN line keeps `timeout <n>`.** Closing the window still hangs (dz `01a0d88a-6518`) and lit has no per-test timeout (dz `01a0d797-e326`). *(Since Task 15 a window test may also end by `win.close()` or a real close request; `timeout` stays, for the second reason.)*

### Slice C review focus

The failure modes the spec implies that are most likely to reach a person, each pinned by a named test in the task that owns the code:

1. A listener that causes more events to be dispatched reads freed memory in a release addon — 11b, `EventDispatcherTest.NestedDispatchLeavesABorrowedEventInPlace`.
2. A clipboard read that fails returns the previous clipboard's text as though it were current — 12, `ui-clipboard.js` case MUTE.
3. An exception in one frame callback silently stops the animation, or exits 0 — 13, `ui-raf.js` cases HANDLED and FATAL.
4. A resize leaves the addon drawing into a surface of the old size, or frees textures off the owner thread — 11d, `ui-resize-generation.js`.
5. An addon built against a smaller struct has bytes past its `size` overwritten — 14a, `ui-abi-sizes.js`.

### Commands used by every task in this slice

Build and run one unit-test binary (the build is part of the command; a stale binary proves nothing):

```bash
cmake --build cmake-build-asan --target <TestName> && cmake-build-asan/unittests/<TestName> --gtest_filter='<Suite>.*'
```

Run named lit tests with exactly the params CMake passes — `.superpowers/sdd/2026-09-21-ui-compositor/runlit.sh <absolute test path>...`, after `cmake --build cmake-build-asan --target hermes-node-ui hnui_demo_addon`. A new lit `--param` is added to `test/CMakeLists.txt`, `test/lit.cfg` **and** `runlit.sh` in the task that introduces it.

Before every commit: `./utils/format.sh -f` then `cmake --build cmake-build-asan --target check-hermes-node`. A run reporting fewer tests than the previous task's total is a failure.

---

### Task 11a: The event model and the platform converter

**Files:**
- Modify: `include/hermes/node-compat/compositor/channels.h`, `lib/compositor/channels.cpp`, `include/hermes/node-compat/ui/hnui_abi.h`, `lib/compositor/hnui_event_convert.cpp`, `unittests/HnuiAbiTest.cpp`, `unittests/HnuiAbiCProbe.c`, `unittests/CompositorTest.cpp`, `unittests/CMakeLists.txt`, `tools/hermes-node-ui/CMakeLists.txt`, `docs/superpowers/specs/2026-09-18-ui-compositor-design.md`
- Create: `tools/hermes-node-ui/sapp_event_convert.h`, `tools/hermes-node-ui/sapp_event_convert.cpp`, `unittests/UiEventsTest.cpp`

**Interfaces produced:**

```cpp
// channels.h
struct WindowState {
  int width = 0, height = 0;                       // logical pixels
  int framebufferWidth = 0, framebufferHeight = 0; // physical pixels
  double dpiScale = 1.0;
  bool focused = false, fullscreen = false, minimized = false;
};
// UiEventType gains, appended after kFileDrop in this order:
//   kMouseEnter, kMouseLeave, kMinimize, kRestore, kPaste
// UiEvent gains:  bool repeat = false;  WindowState window;
// UiEvent::text now carries kTextInput's character AND kPaste's text.

// hnui_abi.h -- same five appended to hnui_event_type, same order:
//   HNUI_EVENT_MOUSE_ENTER, _MOUSE_LEAVE, _MINIMIZE, _RESTORE, _PASTE
// hnui_event gains `uint32_t repeat;` after path_count;
// HNUI_EVENT_MIN_SIZE ends at `repeat`.
#define HNUI_MOD_SHIFT 0x1
#define HNUI_MOD_CTRL  0x2
#define HNUI_MOD_ALT   0x4
#define HNUI_MOD_META  0x8
#define HNUI_MOD_LMB   0x100
#define HNUI_MOD_RMB   0x200
#define HNUI_MOD_MMB   0x400
#define HNUI_BUTTON_LEFT   0   /* DOM numbering */
#define HNUI_BUTTON_MIDDLE 1
#define HNUI_BUTTON_RIGHT  2
#define HNUI_DROP_MAX_FILES      32
#define HNUI_DROP_MAX_PATH_BYTES 4096

// sapp_event_convert.h  (namespace hermes::node_compat)
struct PlatformSource {
  /// The caller's tracked state as of this event. The converter copies it
  /// into the result and applies what the event itself changes.
  WindowState window;
  /// False means the read failed; the paste is then not forwarded.
  std::function<bool(std::string &)> readClipboard;
  std::function<std::vector<std::string>()> droppedPaths;
};
/// \return false for an event type this layer does not forward.
bool convertSappEvent(const sapp_event &in, const PlatformSource &src, UiEvent *out);
/// The KeyboardEvent.code name for one of our key codes, or nullptr.
const char *domCodeForKey(int keyCode);
```

`hnui_event::key_code` and `UiEvent::keyCode` use GLFW key numbering, which is what `sapp_keycode` mirrors; the converter passes the value through and `static_assert`s three anchors (`SAPP_KEYCODE_SPACE == 32`, `SAPP_KEYCODE_A == 65`, `SAPP_KEYCODE_ESCAPE == 256`). Modifiers are mapped bit by bit to `HNUI_MOD_*`, never cast, so a Sokol renumbering breaks the converter's test rather than every addon. `sapp_event` positions are framebuffer pixels; `UiEvent` positions are logical, so `x`, `y`, `dx`, `dy` are divided by `src.window.dpiScale`.

The converter and its helpers go in a new static library **`hermesNodeUiEvents`** (`tools/hermes-node-ui/CMakeLists.txt`; links `hermesNodeCompositor` and the `sokol` INTERFACE target for declarations only), which `hermes-node-ui` links and which the unit test links without dragging in the `sokol_app` implementation. It is not Linux-only: it must build wherever `HERMES_NODE_ENABLE_UI` is on.

- [x] **Step 1: Write the failing tests** in `unittests/UiEventsTest.cpp`, registered as `add_node_compat_unittest(UiEventsTest UiEventsTest.cpp)` linking `hermesNodeUiEvents`, under `if(HERMES_NODE_ENABLE_UI)`:
  - `SappEventConvertTest.EveryEventTypeIsEitherForwardedOrRefused` — loops `0 .. _SAPP_EVENTTYPE_NUM`; each value must appear in exactly one of the test's two tables. Forwarded: `KEY_DOWN→kKeyDown`, `KEY_UP→kKeyUp`, `CHAR→kTextInput`, `MOUSE_DOWN→kMouseDown`, `MOUSE_UP→kMouseUp`, `MOUSE_SCROLL→kMouseWheel`, `MOUSE_MOVE→kMouseMove`, `MOUSE_ENTER→kMouseEnter`, `MOUSE_LEAVE→kMouseLeave`, `RESIZED→kResize`, `ICONIFIED→kMinimize`, `RESTORED→kRestore`, `FOCUSED→kFocus`, `UNFOCUSED→kBlur`, `QUIT_REQUESTED→kCloseRequested`, `CLIPBOARD_PASTED→kPaste`, `FILES_DROPPED→kFileDrop`. Refused (returns false): `INVALID`, the four `TOUCHES_*`, `SUSPENDED`, `RESUMED`. *Mutation: return true for `TOUCHES_BEGAN`.* A Sokol bump that adds a type fails this test until someone decides which table it belongs in.
  - `...ButtonsUseDomNumbering` — `LEFT→0`, `RIGHT→2`, `MIDDLE→1`. *Mutation: pass `mouse_button` through.*
  - `...ModifiersMapBitByBit` — each of the seven `SAPP_MODIFIER_*` bits alone, then all together.
  - `...PositionsAreLogicalPixels` — `dpiScale = 2`, `mouse_x = 200, mouse_dx = 10` gives `x == 100, dx == 5`. *Mutation: drop the division.*
  - `...CharCodeBecomesUtf8` — `char_code = 0x20AC` gives `text == "\xE2\x82\xAC"`; `0x1F600` gives the four-byte form.
  - `...WheelCarriesScrollInDxDy`, `...KeyRepeatIsCarried`.
  - `...StateEventsUpdateTheSnapshot` — with `src.window.minimized == false`, `ICONIFIED` yields `out.window.minimized == true`; `RESTORED`, `FOCUSED`, `UNFOCUSED` likewise for their fields; every other event copies `src.window` unchanged.
  - `...PasteCarriesTheClipboardAndAFailedReadIsRefused` — `readClipboard` returning true with `"clip"` gives `kPaste`, `text == "clip"`; returning false makes `convertSappEvent` return false.
  - `...DropCarriesPathsAndPosition`.
  - `DomCodeTest.RepresentativeNames` — `A→"KeyA"`, `1→"Digit1"`, `ESCAPE→"Escape"`, `LEFT→"ArrowLeft"`, `LEFT_SHIFT→"ShiftLeft"`, `KP_ENTER→"NumpadEnter"`, `F12→"F12"`, `-1→nullptr`.
  - `DomCodeTest.EveryKeyHasAName` — counts non-null results over `0..400` and compares against the number of real enumerators in `sapp_keycode` (**120** at `c0db757`: 122 lines carrying `SAPP_KEYCODE_`, less `INVALID` and `_SAPP_KEYCODE_FORCE_U32`. This plan first said 121; the implementer recounted, which is why the step said to).
  - In `CompositorTest.cpp`: `InputQueueTest.CoalescedMoveKeepsTheLatestWindowSnapshot`.
  - In `HnuiAbiTest.cpp`: the enum-mirroring test extended to the five new values, `repeat` round-trips, and `kPaste` exposes its text through `hnui_event::text`.
- [x] **Step 2: Run to verify they fail** — compilation failure, `sapp_event_convert.h` does not exist.
- [x] **Step 3: Implement** the types, the converter, `domCodeForKey` and the library.
- [x] **Step 4: Run to verify they pass**, then the `HnuiAbiTest` and `CompositorTest` binaries too.
- [x] **Step 5: Make each named mutation, watch its test fail, revert.**
- [x] **Step 6: Correct the spec** — remove the four touch events from "The JS API" and add "**No touch events.** `sokol_app` produces them only on iOS, Android and Emscripten." to "What this does not do". The other event names are already listed there and stay.
- [x] **Step 7: Format, run `check-hermes-node`, commit** (`Add the full event model and the sapp_event converter`).

---

### Task 11b: Delivering events to native listeners and to JavaScript

**Files:**
- Create: `include/hermes/node-compat/compositor/event_dispatcher.h`, `lib/compositor/event_dispatcher.cpp`, `tools/hermes-node-ui/event_injector.h`, `tools/hermes-node-ui/event_injector.cpp` (in `hermesNodeUiEvents`), `tools/hermes-node-ui/ui_events.h`, `tools/hermes-node-ui/ui_events.cpp`, `test/ui-events.js`
- Modify: `tools/hermes-node-ui/ui_module.cpp`, `ui_module.h`, `hnui_window.h`, `hnui_api.cpp`, `CMakeLists.txt`; `include/hermes/node-compat/ui/hnui.h`; `lib/compositor-gpu/test_hooks.{h,cpp}` and `unittests/TestHooksTest.cpp` (remove `EventInjector`); `lib/compositor/CMakeLists.txt`; `unittests/CompositorTest.cpp`, `unittests/UiEventsTest.cpp`; `test/fixtures/hnui-demo/demo_addon.c`

**Interfaces consumed:** `convertSappEvent`, `PlatformSource`, `WindowState`, `domCodeForKey` (11a); `InputQueue`, `HnuiEventConverter`, `OwnerDispatch::post`, `TickMachine::setMinimized`, `handleCallbackException` (`include/hermes/node-compat/bindings/node_errors.h`).

**Interfaces produced:**

```cpp
// event_dispatcher.h -- owner thread only, no lock of its own
class EventDispatcher {
 public:
  using Sink = std::function<void(const UiEvent &)>;
  explicit EventDispatcher(InputQueue &queue);
  /// Deliver everything queued, in order, and keep going until the queue
  /// is empty. A call made while one is already running returns at once:
  /// the running one will pick the new events up.
  void dispatch(const Sink &sink);
  bool dispatching() const;
};

// event_injector.h
struct InjectedPlatformEvent {
  sapp_event event{};
  bool hasClipboard = false;  std::string clipboard;
  std::vector<std::string> paths;
  bool hasWindow = false;     WindowState window;
};
class PlatformEventInjector {   // inject(): any thread; takeNext(): platform thread
 public:
  void inject(InjectedPlatformEvent event);
  bool takeNext(InjectedPlatformEvent *out);
};

// hnui.h
int  hnui_add_event_listener(hnui_window *w, hnui_event_cb cb, void *user);  // 0 for NULL w or cb
void hnui_get_window_state(hnui_window *w, hnui_window_state *out);

// ui_module.h -- the one entry both real and injected events go through
void uiHandlePlatformEvent(const sapp_event &event, const PlatformSource &source);

// ui_events.h -- process._tickCallback(), with an escaping exception routed
// through handleCallbackException. Task 13 calls the same function.
void uiRunTickCallback(napi_env env);
```

`hnui_window` gains what `ui_events.cpp` needs so that file reaches nothing through `ui_module.cpp`'s statics: `napi_env env`, `napi_ref jsWindow`, `WindowState state`, the native listener list with `nextListenerId`, and the `EventDispatcher`.

**The JS surface.** `createWindow` resolves with an `EventEmitter` instance (Ruling 53) carrying the eight state properties, kept current from `hnui_window::state`. Events and their payloads:

| `emit` name | From | Payload fields |
|---|---|---|
| `keydown`, `keyup` | kKeyDown, kKeyUp | `code` (`domCodeForKey`, `''` if unknown), `keyCode`, `repeat`, `shiftKey`, `ctrlKey`, `altKey`, `metaKey` |
| `char` | kTextInput | `char`, plus the four modifier booleans |
| `mousedown`, `mouseup` | kMouseDown, kMouseUp | `x`, `y`, `button`, modifiers |
| `mousemove` | kMouseMove | `x`, `y`, `dx`, `dy`, modifiers |
| `wheel` | kMouseWheel | `x`, `y`, `deltaX`, `deltaY`, modifiers |
| `mouseenter`, `mouseleave` | kMouseEnter, kMouseLeave | `x`, `y` |
| `resize` | kResize | `width`, `height`, `framebufferWidth`, `framebufferHeight`, `dpiScale` |
| `focus`, `blur`, `minimize`, `restore`, `close` | their kinds | `{}` |
| `paste` | kPaste | `text` |
| `drop` | kFileDrop | `paths`, `x`, `y` |

**Order of work for one event, on the JS thread:** assign `event.window` to `hnui_window::state` and to the JS object's properties; run native listeners in registration order, each through its own `HnuiEventConverter`; `emit` to JavaScript; then call `process._tickCallback()`, so `nextTick` and promise callbacks queued by one event's listeners run before the next event is delivered. An exception from `emit` or from the tick callback goes through `handleCallbackException` — handled means carry on with the next event, unhandled does not return.

**On the platform thread** `onEvent` builds a `PlatformSource` (tracked `WindowState` refreshed from `sapp_width`/`sapp_height`/`sapp_dpi_scale`/`sapp_is_fullscreen`; `readClipboard` over `sapp_get_clipboard_string` until Task 12 replaces it; `droppedPaths` over `sapp_get_dropped_file_path`) and calls `uiHandlePlatformEvent`, which converts, stores `out.window` back as the tracked state, calls `TickMachine::setMinimized` for kMinimize/kRestore, pushes to the `InputQueue`, and posts the dispatch — **at most one post outstanding**, through an `std::atomic<bool>` the posted function clears *before* it drains. For `QUIT_REQUESTED` it calls `sapp_cancel_quit()` first (Ruling 54). `onFrame` drains the `PlatformEventInjector`, immediately after `pollCompletions()`, into the same function, substituting the injected clipboard, paths and window where present. `sapp_desc` gains `enable_dragndrop = true`, `max_dropped_files = HNUI_DROP_MAX_FILES`, `max_dropped_file_path_length = HNUI_DROP_MAX_PATH_BYTES`.

`ui.__injectEvent(desc)` exists only under `HERMES_NODE_UI_TEST_HOOKS`. `desc.type` is the `SAPP_EVENTTYPE_` name without the prefix; the rest are `keyCode`, `repeat`, `modifiers`, `mouseX`, `mouseY`, `mouseDx`, `mouseDy`, `scrollX`, `scrollY`, `button` (Sokol numbering — it is a platform event), `charCode`, `clipboard`, `paths`, `window`. **This discharges Ruling 48 the preferred way**: a synthetic event is now the converter's input rather than its output. `EventInjector` in `lib/compositor-gpu/test_hooks` is deleted, with its tests.

- [x] **Step 1: Write the failing unit tests.**
  - `EventDispatcherTest.DeliversInPushOrder`.
  - `EventDispatcherTest.NestedDispatchLeavesABorrowedEventInPlace` — **the address-stability obligation carried from Task 7 (Ruling 45).** Push a kTextInput with `text = "a"` (short, so the bytes live inside the `UiEvent`) and one more event. The sink, on the first event, builds an `HnuiEventConverter`, keeps `const char *p = converter.event().text`, pushes 64 more events onto the queue, calls `dispatch()` again, and then reads `*p`. Expect `'a'`, no ASAN report, and all 66 events delivered once each in push order. *Mutation: delete the already-dispatching guard, so the nested call drains into the buffer the outer call is iterating. It must report heap-use-after-free under ASAN.* If it does not, the test is not pinning the constraint; fix the test, not the claim.
  - `EventDispatcherTest.DispatchingIsTrueOnlyInsideASink`.
  - In `UiEventsTest.cpp`: `PlatformEventInjectorTest.PreservesInjectionOrder`.
- [x] **Step 2: Run to verify they fail.**
- [x] **Step 3: Implement `EventDispatcher` and `PlatformEventInjector`; run; make the mutation; revert.**
- [x] **Step 4: Extend the addon** with an ordered in-memory log shared by both sides — `startEventLog()` registers a native listener that appends `native:<type>` (plus the fields under test), `logLine(s)` appends a string from JavaScript, `takeEventLog()` returns and clears it. One log, because `printf` and `console.log` reach the pipe by different routes and their relative order is not preserved.
- [x] **Step 5: Write the failing lit test** `test/ui-events.js` (`REQUIRES: ui, ui-gpu` and `display`; RUN line modelled on `ui-present.js`, test hooks on). Inject, in one script: `KEY_DOWN` A with shift, `CHAR` `a`, three `MOUSE_MOVE`s with `mouseDx` 1, 2, 3, `MOUSE_DOWN` right, `MOUSE_SCROLL`, `UNFOCUSED`, `FOCUSED`, `CLIPBOARD_PASTED` with `clipboard: 'clip'`, `FILES_DROPPED` with two paths, `QUIT_REQUESTED`. Assert:
  - for every event, `native:<type>` precedes `js:<name>`, and the sequence across events is the injection order. *Mutation: emit to JavaScript before the native listeners.*
  - `keydown` reports `code === 'KeyA'`, `shiftKey === true`; `mousedown` reports `button === 2`; the `dx` of all `mousemove`s received sums to 6 (their count is not asserted — whether they coalesce depends on which thread runs first); `paste.text === 'clip'`; `drop.paths.length === 2`.
  - inside the `blur` listener `win.focused === false`, and inside `focus` it is `true` again. *Mutation: update the mirror after `emit`.*
  - a promise callback queued by the `keydown` listener runs before the `char` listener. *Mutation: remove the `_tickCallback` call.*
  - `close` is emitted and the process is still running afterwards (the script then exits 0 itself).
  - `hnui_get_window_state` from the addon agrees with `win.width`/`win.framebufferWidth`.
  - **Minimized means no ticks:** inject `ICONIFIED`; in the `minimize` listener call `requestAnimationFrame(cb)` and inject a marker `KEY_DOWN`; on that marker inject a second; on the second assert `cb` has not run, then inject `RESTORED`; `cb` runs after `restore`. Two marker round trips guarantee a platform frame went past with the request pending. *Mutation: remove the `setMinimized` call.*
- [x] **Step 6: Run to verify it fails**, then implement the platform half, `ui_events.cpp`, the window object and the two entry points.
- [x] **Step 7: Run to verify it passes; make each mutation; revert.**
- [x] **Step 8: Record the interim close behaviour** — `dz comment 01a0d88a -m "..."` saying that the close button now emits `close` and no longer closes the window, and that the default action is still Task 15's.
- [x] **Step 9: Format, run `check-hermes-node`, commit** (`Deliver input events to native listeners and to JavaScript`).

---

### Task 11c: Platform commands

**Files:**
- Modify: `include/hermes/node-compat/compositor/channels.h`, `lib/compositor/channels.cpp`, `unittests/CompositorTest.cpp`, `include/hermes/node-compat/ui/hnui_abi.h`, `tools/hermes-node-ui/ui_module.cpp`, `ui_module.h`, `ui_events.cpp`
- Create: `test/ui-commands.js`

**Interfaces produced:**

```cpp
// Command::Kind gains kSetFullscreen, kSetIcon, kSetCursor, kShowCursor,
// kLockPointer, kWriteClipboard. Factories, each owning its payload:
static Command setFullscreen(bool on);
static Command setIcon(std::vector<uint8_t> rgba, int width, int height);
static Command setCursor(int cursor);          // an hnui_cursor value
static Command showCursor(bool show);
static Command lockPointer(bool lock);
static Command writeClipboard(std::string utf8);
// accessors: flag(), cursor(), text()

// ui_module.h -- what the JS surface and, later, hnui_api.cpp both call
void uiPushCommand(Command command);
```

JS surface: `win.title` (accessor; the getter returns the last value set), `win.setFullscreen(bool)`, `win.setIcon({width, height, pixels})`, `win.setCursor(name)` with names `default`, `arrow`, `ibeam`, `crosshair`, `pointer`, `ew-resize`, `ns-resize`, `nwse-resize`, `nesw-resize`, `move`, `not-allowed` (an unknown name throws a `TypeError` listing them), `win.setCursorImage({width, height, pixels, hotspotX, hotspotY})` (binds `SAPP_MOUSECURSOR_CUSTOM_0` and selects it), `win.showCursor(bool)`, `win.lockPointer(bool)`, `ui.writeClipboard(text)`. `win.close()` is Task 15's and `ui.readClipboard()` is Task 12's; neither is declared here.

`onFrame` drains the `CommandQueue` **before** it drains the event injector and applies each command with the matching `sapp_*` call. `hnui_abi.h` gains `#define HNUI_CLIPBOARD_MAX_BYTES (1u << 20)` here, where the limit is first enforced. `sapp_desc` gains `enable_clipboard = true` and `clipboard_size = HNUI_CLIPBOARD_MAX_BYTES`; a `writeClipboard` whose text does not fit throws a `RangeError` on the JS thread and never reaches Sokol, which would otherwise take selection ownership with an empty buffer (`sokol_app.h:13205-13215`).

**Each application traces the value read back, not the value requested** — `sapp_is_fullscreen()`, `sapp_mouse_shown()`, `sapp_mouse_locked()`, `sapp_get_mouse_cursor()`, and for the title an `XFetchName` on `sapp_x11_get_window()` (Linux; the trace is the only caller). A trace that prints the request passes with the `sapp_*` call deleted, which is the hole Slice B found three times.

- [x] **Step 1: Write the failing unit tests** — `CommandTest.EachKindOwnsItsPayload` (build from a temporary, destroy the temporary, read back) for the six new kinds.
- [x] **Step 2: Fail, implement, pass.**
- [x] **Step 3: Write the failing lit test** `test/ui-commands.js` — issue every command, then inject a marker `KEY_DOWN` and exit 0 when it arrives (commands are drained before the injector, so the marker cannot overtake them). FileCheck the trace: title readback equals what was set, fullscreen readback, cursor readback equals the mapped `sapp_mouse_cursor`, cursor-shown readback, pointer-lock readback (assert what Xvfb actually reports and say so in the test if the grab is refused), custom cursor selected, clipboard written. Plus: an unknown cursor name throws, an oversized clipboard write throws.
  *Mutations: delete the `sapp_set_window_title` call; map `pointer` to `SAPP_MOUSECURSOR_IBEAM`.*
- [x] **Step 4: Fail, implement, pass, mutate, revert.**
- [x] **Step 5: Format, run `check-hermes-node`, commit** (`Apply platform commands from JavaScript`).

---

### Task 11d: A resize starts a new generation

**Files:**
- Modify: `tools/hermes-node-ui/ui_module.cpp`, `ui_events.cpp`, `lib/compositor-gpu/backend.h`, `gl_backend.cpp` (only if a generation's size is not already answerable by `GenerationId`), `test/fixtures/hnui-demo/demo_addon.c`, `docs/superpowers/plans/2026-09-21-ui-compositor.md` (Task 17's wording)
- Create: `test/ui-resize-generation.js`

**What it does.** When an event's `window.framebufferWidth`/`Height` differ from the current generation's size and both are positive, the JS thread — before that event's listeners run — calls `SurfaceTable::newGeneration` and `createGenerationSurfaces` at the new size. A failure ends the session through `terminateGraphicsSession`. The old generation retires by the rules already in `SurfaceTable`; nothing here frees it.

`gGeneration`, `gGenerationWidth` and `gGenerationHeight` are today written once before `gPresentReady` and read by the platform thread afterwards. They now change, so **the platform thread must take a presented surface's size from its `SurfaceRef::gen`** — through the backend or the table — never from a variable the JS thread rewrites. `maybeCapture()` is the reader that matters.

**This is where Ruling 16's destroy callback first executes**, so the obligation Task 17 carried lands here: the destroy trace must say which thread ran it (`owner=1` when `std::this_thread::get_id()` equals `hnui_window::ownerThread`), and the test asserts it. Task 17 keeps the case this task cannot reach — a resize of the real framebuffer, which event injection does not perform.

- [x] **Step 1: Extend the addon** — `lastTarget()` returns the `generation`, `width` and `height` of the most recent `hnui_frame_target`.
- [x] **Step 2: Write the failing lit test** `test/ui-resize-generation.js` (window 640x480, test hooks and tracing on). Draw and capture one frame of generation 1. Inject `RESIZED` with `window.framebufferWidth: 320, framebufferHeight: 240`. Assert:
  - the `resize` event reports 320x240 and `win.framebufferWidth === 320`;
  - the next frame's `lastTarget()` is generation 2, 320x240. *Mutation: skip `newGeneration`.*
  - a capture requesting generation 2, target 320x240, framebuffer 640x480 matches, and interior samples at least 16 px from any pattern edge are exact — the stretched case the spec asks to be asserted on its own;
  - the trace shows generation 1 destroyed with `owner=1`, after generation 2 was created. *Mutation: call `destroyGenerationSurfaces` inline from the `SurfaceTable` callback instead of posting it; the trace must then say `owner=0`.* If it still says `owner=1`, the last reference was dropped on the owner thread in this run and the test is not exercising the case — arrange the sequence so a platform-thread take drops it.
  - a `RESIZED` to `0x0` starts no generation;
  - rendering continues: a further frame is published in generation 2.
- [x] **Step 3: Fail, implement, pass, mutate, revert.**
- [x] **Step 4: Amend Task 17's paragraph** to say the first execution and the owner-thread assertion were discharged here, and that the real-framebuffer resize remains.
- [x] **Step 5: Format, run `check-hermes-node`, commit** (`Start a new surface generation on resize`).

---

### Task 12: Clipboard with a real status

**Files:**
- Modify: `include/hermes/node-compat/compositor/channels.h`, `lib/compositor/channels.cpp`, `unittests/CompositorTest.cpp`, `include/hermes/node-compat/ui/hnui_abi.h`, `include/hermes/node-compat/ui/hnui.h`, `tools/hermes-node-ui/hnui_api.cpp`, `ui_module.cpp`, `ui_events.cpp`, `CMakeLists.txt`, `test/CMakeLists.txt`, `test/lit.cfg`, `.superpowers/sdd/2026-09-21-ui-compositor/runlit.sh`, `test/fixtures/hnui-demo/demo_addon.c`
- Create: `tools/hermes-node-ui/x11_clipboard.h`, `x11_clipboard.cpp`, `test/fixtures/hnui-demo/x11_selection_owner.c`, `test/ui-clipboard.js`

**Interfaces produced:**

```cpp
// channels.h
enum class ClipboardStatus { kOk, kUnavailable, kTimeout, kTooLarge, kError };
struct ClipboardResult { ClipboardStatus status = ClipboardStatus::kError; std::string text; };
// ClipboardRequest::wait(int timeoutMs) -> ClipboardResult   (kTimeout on expiry)
// ClipboardRequests::answer(uint64_t id, ClipboardResult)    (was std::string)
// ClipboardRequests::failAll(ClipboardStatus)   // completes every outstanding waiter
// Command::readClipboard(uint64_t requestId, bool replyThroughSlot, uint64_t deadlineNs)
// UiEventType::kClipboardReply (internal: never reaches a listener, never
//   converted to hnui_event) carrying requestId, status and text.
//   UiEvent gains `uint64_t requestId` and `ClipboardStatus clipboardStatus`.
//   It is appended AFTER every value hnui_event_type mirrors and has no
//   public counterpart: the mirroring test asserts the mirrored prefix,
//   and that converting a kClipboardReply yields HNUI_EVENT_INVALID.

// hnui_abi.h -- appended to hnui_status
//   HNUI_UNAVAILABLE = 4   nothing owns the clipboard, or it holds no text
//   HNUI_TOO_LARGE   = 5      over HNUI_CLIPBOARD_MAX_BYTES (defined in 11c)

// hnui.h
int  hnui_read_clipboard_sync(hnui_window *w, int timeout_ms, char **out);
void hnui_free(void *p);

// x11_clipboard.h
ClipboardResult x11ReadClipboard(void *display, unsigned long window,
                                 int timeoutMs, size_t maxBytes);
```

`x11ReadClipboard` runs on the platform thread. If our own window owns `CLIPBOARD` it returns `sapp_get_clipboard_string()`, which is correct in exactly that case (`sokol_app.h:13221`). Otherwise: no owner is `kUnavailable`; it converts the selection to `UTF8_STRING` and waits for `SelectionNotify` up to the timeout (`kTimeout`); a `None` property is `kUnavailable`; an `INCR` reply or a property over `maxBytes` is `kTooLarge`. **Nothing on any path returns a buffer from an earlier read.**

JS: `ui.readClipboard()` returns a Promise — the request goes out as a command, the platform thread reads and replies with a `kClipboardReply` event, and the JS thread settles it: `kOk` resolves with the text, `kUnavailable` resolves with `''`, the rest reject with an `Error` whose `code` is `ERR_CLIPBOARD_TIMEOUT`, `ERR_CLIPBOARD_TOO_LARGE` or `ERR_CLIPBOARD_FAILED`. Native: `hnui_read_clipboard_sync` creates a `ClipboardRequest`, pushes the command with `replyThroughSlot`, and waits with the caller's deadline; on `HNUI_OK` `*out` is `malloc`ed for `hnui_free`, otherwise `*out` is `NULL`. The platform thread skips a request whose deadline has already passed and caps its own wait at the time remaining. The `paste` event's `readClipboard` source (11b) switches to `x11ReadClipboard`.

`x11_selection_owner <mode>` is a ~80-line C fixture built under the same condition as the GL backend, passed to lit as `%x11_selection_owner`. It takes ownership of `CLIPBOARD`, prints `READY`, and then: `text <s>` answers `UTF8_STRING` requests with `<s>`; `mute` never answers; `incr` answers with type `INCR`. It exits on stdin EOF.

- [x] **Step 1: Write the failing unit tests** in `CompositorTest.cpp`: `ClipboardRequestTest.AnswerCarriesItsStatus`; `...WaitReportsTimeoutAsAStatus`; `...FailAllCompletesEveryWaiterAndALaterAnswerIsRefused`; and the existing late-reply test (A times out, B requests, A's reply arrives, B still pending) ported to the new signatures. *Mutation: make `answer()` ignore the id and fulfil the oldest request.*
- [x] **Step 2: Fail, implement, pass.**
- [x] **Step 3: Extend the addon** — `readClipboardSync(timeoutMs)` returns `{status, text}` and frees through `hnui_free`.
- [x] **Step 4: Write the failing lit test** `test/ui-clipboard.js`. Each case reads both ways, Promise and sync:
  - OWN: `writeClipboard('first')` then read gives `'first'`.
  - FOREIGN: spawn the owner in `text second`, wait for `READY`, read gives `'second'`.
  - MUTE: replace it with a `mute` owner; the Promise rejects with `ERR_CLIPBOARD_TIMEOUT`, the sync read returns `HNUI_TIMEOUT` with a null string, **and neither yields `'first'` or `'second'`**. *Mutation: on timeout, fall back to `sapp_get_clipboard_string()`.* This is the test the spec names.
  - INCR: `ERR_CLIPBOARD_TOO_LARGE` / `HNUI_TOO_LARGE`.
  - NONE: with no owner, `''` / `HNUI_UNAVAILABLE`.
  - ABANDONED: a sync read with `timeout_ms = 0` returns `HNUI_TIMEOUT`, the trace shows the platform thread skipping or discarding that request's reply, and the following read still works.
- [x] **Step 5: Fail, implement, pass, mutate, revert.**
- [x] **Step 6: Format, run `check-hermes-node`, commit** (`Read the clipboard with a real status`).

---

### Task 13: Full `requestAnimationFrame` semantics

**Files:**
- Create: `include/hermes/node-compat/compositor/frame_callbacks.h`, `lib/compositor/frame_callbacks.cpp`, `test/ui-raf.js`
- Modify: `lib/compositor/CMakeLists.txt`, `unittests/CompositorTest.cpp`, `tools/hermes-node-ui/ui_module.cpp`, `hnui_window.h`, `hnui_api.cpp`, `include/hermes/node-compat/ui/hnui.h`, `test/fixtures/hnui-demo/demo_addon.c`, `docs/superpowers/specs/2026-09-18-ui-compositor-design.md`

**Interfaces produced:**

```cpp
// frame_callbacks.h -- owner thread only; knows nothing about napi or hnui
struct FrameCallback {
  enum class Kind { kNative, kJs };
  int id = 0;
  Kind kind = Kind::kNative;
  void *fn = nullptr;     // an hnui_frame_cb, or a napi_ref
  void *user = nullptr;
};
class FrameCallbackList {
 public:
  int add(FrameCallback::Kind kind, void *fn, void *user);   // ids from 1, one space
  /// True if the callback had not run yet -- waiting for the next batch, or
  /// later in the batch now running. \p removed lets the caller release it.
  bool cancel(int id, FrameCallback *removed);
  void beginBatch();                 // everything registered so far
  bool next(FrameCallback *out);     // the next uncancelled one of the batch
  bool pending() const;              // anything registered for a later batch
};

// hnui.h
void hnui_cancel_animation_frame(hnui_window *w, int id);
```

`hnui_window::nativeCallbacks` and `ui_module.cpp`'s `gJsCallbacks` are both replaced by one `FrameCallbackList` (Ruling 57). `cancelAnimationFrame` is exported from the module and installed as a global beside `requestAnimationFrame`.

**`runFrameTick`, rewritten around the list:** `beginBatch()`; for each `next()`, invoke it — native directly, JS through `napi_call_function` — then call `process._tickCallback()`. An exception from a JS callback or from the tick callback goes through `handleCallbackException`: handled, the batch continues; unhandled, it does not return and the process ends with status 1 after `'exit'` runs. What follows the batch is unchanged, so a handled exception cannot skip the frame's submission or its reclaim. The timestamp is `(uv_hrtime() - originNs) / 1e6`, with `originNs` computed once when the window becomes ready as `uv_hrtime() - process.uptime() * 1e9` (Ruling 56).

- [x] **Step 1: Write the failing unit tests** (`FrameCallbackListTest`): `RunsInRegistrationOrderAcrossKinds`; `RegisteredDuringABatchRunsInTheNext`; `CancellingALaterCallbackOfTheRunningBatchPreventsIt` (*mutation: have `next()` ignore cancellation*); `CancellingOneThatAlreadyRanReportsFalse`; `CancellingANextBatchCallbackRemovesIt`; `IdsAreSharedAndNeverReused`.
- [x] **Step 2: Fail, implement, pass, mutate, revert.**
- [x] **Step 3: Extend the addon** — `requestNativeFrame(tag)` registers a native callback that appends `native:<tag>` to the shared log and returns its id; `cancelNativeFrame(id)`.
- [x] **Step 4: Write the failing lit test** `test/ui-raf.js`, one RUN per group where the exit status differs:
  - ORDER: register js1, native1, js2, native2 in that order; the log reads the same. *Mutation: run all native callbacks first.*
  - CHECKPOINT: cb1 queues a `process.nextTick` and a promise callback; the log reads cb1, tick, micro, cb2. *Mutation: remove the `_tickCallback` call.*
  - TIME: every callback of one tick receives the same timestamp; with `before = process.uptime() * 1000` read before registering and `after` read inside the callback, `before - 1 <= ts <= after + 1`. *Mutation: use `uv_hrtime()` without the origin.* A callback registered inside a callback runs on a later tick with a larger timestamp.
  - CANCEL: cb1 cancels cb3, and a promise callback queued by cb1 cancels cb2; neither runs. A cancelled next-tick callback never runs. `cancelAnimationFrame` of an unknown id does nothing. After a tick whose only callback was cancelled, a new request still gets a tick.
  - HANDLED: with an `uncaughtException` listener installed, cb0 draws, cb1 throws, cb2 runs; the listener received the error; the frame cb0 drew is published (asserted through a capture); a later tick runs. Same for an exception thrown from a promise callback between callbacks.
  - FATAL (its own RUN line): no listener; a callback throws; exit status 1, the error is on stderr, and an `'exit'` listener's output is present. *Mutation: restore the print-and-continue block.*
  - GLOBALS: `typeof requestAnimationFrame` and `typeof cancelAnimationFrame` are both `'function'` once the window exists.
- [x] **Step 5: Fail, implement, pass, mutate, revert.**
- [x] **Step 6: Remove the three "not yet" comments this task discharges** in `ui_module.cpp` (the timestamp origin, native-before-JS ordering, the interim exception print) and the "id is returnable and not yet usable" note in `hnui.h`.
- [x] **Step 7: Correct the spec** — in "Frames", replace "sharing `performance.now()`'s origin" with `process.uptime()`'s and the reason (Ruling 56).
- [x] **Step 8: Format, run `check-hermes-node`, commit** (`Give requestAnimationFrame its full semantics`).

---

### Task 14a: The rest of the C API, and the version check

**Files:**
- Modify: `include/hermes/node-compat/ui/hnui.h`, `hnui_abi.h`, `tools/hermes-node-ui/hnui_api.cpp`, `hnui_window.h`, `ui_events.cpp`, `external/sokol/CMakeLists.txt` (define the pinned commit once, as a CMake variable exported as a compile definition; today it is written only in `external/sokol/README.md`), `unittests/HnuiAbiTest.cpp`, `test/fixtures/hnui-demo/demo_addon.c`
- Create: `test/ui-abi-sizes.js`, `test/ui-version-check.js`, `test/sokol-pin.js`

**Interfaces produced:**

```c
int  hnui_api_version(void);                       /* HNUI_ABI_VERSION of the host */
const char *hnui_sokol_commit(void);               /* the host's vendored Sokol commit */
void hnui_remove_event_listener(hnui_window *w, int id);
bool hnui_event_dispatch_quiescent(hnui_window *w);
void hnui_set_cursor(hnui_window *w, hnui_cursor c);
void hnui_show_cursor(hnui_window *w, bool show);
void hnui_lock_pointer(hnui_window *w, bool lock);
void hnui_write_clipboard(hnui_window *w, const char *utf8);
#define HNUI_SOKOL_COMMIT "c0db757"                /* generated, not typed */
#define HNUI_CHECK_VERSION(env) /* throws a JS error naming both pairs on a mismatch */
```

- **Removal is deferred** (spec: The native C API): a listener removed during an event's delivery still receives that event and no later one. `hnui_event_dispatch_quiescent` is true when no native listener is on the stack (`EventDispatcher::dispatching()`), which is when a removed listener's user data may be freed. The spec promises "the window reports that dispatch quiescent" without naming the call; this is that call, and Step 6 adds it to the spec's prototype list.
- The four command entry points go through `uiPushCommand` (11c). `hnui_set_cursor` with a value outside `HNUI_CURSOR_DEFAULT..HNUI_CURSOR_NOT_ALLOWED` is refused with a message on stderr and pushes nothing; `hnui_write_clipboard` over `HNUI_CLIPBOARD_MAX_BYTES` likewise.
- `HNUI_SOKOL_COMMIT` comes from one place — a variable in `external/sokol/CMakeLists.txt`, passed as a compile definition to the host and to every addon target — so the header and the host cannot disagree by a typo. `test/sokol-pin.js` (new, ungated beyond `ui-enabled`) asserts that variable equals the commit `external/sokol/README.md` names.

**Three obligations carried from Task 7 (Ruling 45), all discharged here:**

- [x] **Step 1: Write the failing lit test** `test/ui-abi-sizes.js`. Through an addon function `probeSizes()`, for each writer — `hnui_frame_target` inside a frame callback and `hnui_get_window_state`: fill a buffer larger than the struct with `0xA5`, set `size` to exactly the `HNUI_*_MIN_SIZE`, call, and assert every byte from `size` onward is still `0xA5`; then set `size` one below the minimum and assert the call refuses and **no byte** changed. *Mutation: `memcpy(out, &filled, sizeof filled)`.*
- [x] **Step 2: Write the failing checks for the enum direction** — in the same test, `hnui_set_cursor(w, (hnui_cursor)999)` produces the refusal message and a trace shows no command was pushed. *Mutation: index the cursor table without the range check — under ASAN this is a global-buffer-overflow.*
- [x] **Step 3: Write the failing lit test** `test/ui-version-check.js` — the addon built normally passes `HNUI_CHECK_VERSION`; a second build of the same source with `-DHNUI_SOKOL_COMMIT_OVERRIDE=\"deadbee\"` throws a JS error whose message contains both `deadbee` and the host's commit. (Add that second addon target beside `hnui_demo_addon`, with its own lit param.) Also `test/sokol-pin.js`: the CMake variable, passed to lit as a param, equals the commit `external/sokol/README.md` names. *Mutation: change the variable.*
- [x] **Step 4: Write the failing removal checks** in `test/ui-events.js`: listener A removes listener B during event 1; B still receives event 1 and does not receive event 2; `hnui_event_dispatch_quiescent` is false inside A and true in the next frame callback. *Mutation: erase immediately.*
- [x] **Step 5: Fail, implement, pass, mutate, revert.**
- [x] **Step 6: Add `hnui_event_dispatch_quiescent` and `hnui_sokol_commit` to the spec's prototype list**, and replace the "minimal subset" preamble of `hnui.h` with a description of the whole API.
- [x] **Step 7: Format, run `check-hermes-node`, commit** (`Complete the hnui C API and add the version check`).

---

### Task 14b: The reference addon's visual and conformance modes

**Files:**
- Create: `external/sokol/sokol/util/sokol_gl.h` (upstream, at the pinned commit; add it to `external/sokol/README.md`'s file list), `test/ui-demo-visual.js`, `test/ui-conformance.js`
- Modify: `test/fixtures/hnui-demo/demo_addon.c`, `demo.js`, `test/CMakeLists.txt`, `lib/compositor-gpu/gl_backend.cpp` (only if the resolve view is not yet usable)

**What it adds** (spec: The reference addon):

- **Visual mode**, the default of `demo.js`: a rotating triangle drawn with `sokol_gl`, sized from the target's reported dimensions, in both frame-driving modes (`js`, `native`). `sokol_gl`'s default pipeline must work with no configuration, which is the check that `sg_setup`'s environment defaults are what the spec says. A key event, received through `hnui_add_event_listener`, changes the clear colour.
- The existing fixed pattern stays as mode `pattern`, which is what the capture tests use.
- **Conformance mode**: `demo.js <addon> conformance` calls each entry point the visual demo does not and prints one line per observation — `hnui_api_version`, `hnui_cancel_animation_frame`, `hnui_remove_event_listener` and `hnui_event_dispatch_quiescent`, `hnui_get_window_state`, `hnui_set_cursor`, `hnui_show_cursor`, `hnui_lock_pointer`, `hnui_write_clipboard`, `hnui_read_clipboard_sync` (a normal read and a timeout), `hnui_free` — and renders one frame through the **resolve view** with an MSAA image of its own.

- [x] **Step 1: Vendor `util/sokol_gl.h`** at the pinned commit, unmodified, and record it in the README.
- [x] **Step 2: Write the failing lit test** `test/ui-demo-visual.js` — both `js` and `native` modes publish frames (trace); with the triangle's rotation frozen by a test-only addon call, a capture's clear-colour samples in the corners are exact; after an injected `KEY_DOWN` the next capture's corner samples are the second clear colour, exactly. *Mutation: do not register the event listener.*
- [x] **Step 3: Write the failing lit test** `test/ui-conformance.js` — FileCheck the printed lines in order, and capture the MSAA frame: a flat colour cleared into the addon's own 4x image and resolved into the slot, compared exactly. *Mutation: attach the colour view where the resolve view belongs; Sokol's validation must reject it or the capture must differ.*
- [x] **Step 4: Fail, implement, pass, mutate, revert.**
- [x] **Step 5: Update `demo.js`'s header comment** so the by-hand smoke test is described as it now is.
- [x] **Step 6: Format, run `check-hermes-node`, commit** (`Add the reference addon's visual and conformance modes`).

---

### After Slice C

Done: the whole-slice review, its one fix wave (F1-F17, nine commits) and that wave's re-review; the steps above are ticked, the completion paragraph is beside Slice B's, and `docs/superpowers/plans/progress-ui-compositor.md` and CLAUDE.md's UI Compositor section are updated.

---

### Tasks 15–17

Each follows the same shape — failing test naming its mutation, full build-and-run commands, implementation, verification, format, commit — and each is written out in the same detail as the tasks above when its slice begins. Their scope and the findings they answer:

- **Task 15 — Lifecycle and CLOSING** (finding 14). **Done 2026-11-19.** Written out to step level in `docs/superpowers/plans/2026-11-19-ui-compositor-task15.md`, whose Design section records where it departs from the spec and why; the spec was corrected in 15e.
  - [x] **15a** — split `ui_module.cpp` by thread (`Split ui_module.cpp by thread`).
  - [x] **15b** — exit hooks, and CLOSING on the `process.exit()` and fatal paths (`Enter CLOSING before 'exit' listeners on process.exit and fatal paths`).
  - [x] **15c** — what CLOSING fails and refuses (`Capture process._preExit once in the fatal path`; `Fail clipboard reads and refuse commands once the window is closing`).
  - [x] **15d** — a runtime that returns, and a rejected `createWindow` (`End a windowed process when its runtime returns`).
  - [x] **15e** — the cancelable close, `win.close()` and `hnui_cancel_close`; tracker `01a0d88a-6518` closed (`Close the window on an uncancelled close, and add win.close()`).
  - [x] **15f** — the window tests' endings and comments, and the documents (`Revisit the window tests now that a window can close`; `Record how a window closes`).

  **The eight obligations carried from Slice C, and what discharged each.** (1) Split `ui_module.cpp` by thread as the slice's first commit (Ruling 68) -- 15a, a pure move into `ui_module.cpp`, `ui_platform.cpp`, `ui_test_hooks.cpp` and `ui_internal.h`, checked line by line against the old file. (2) Fail outstanding clipboard reads of both kinds on entering CLOSING, then drain ticks -- 15c, `test/ui-exit-paths.js` READS and `ClipboardRequestTest.FailAllCompletesEveryWaiterAndALaterAnswerIsRefused`. (3) Call `FrameCallbackList::releaseAll()` while the `napi_env` is alive -- 15b, `test/ui-exit-paths.js` FRAME (`released=2`). (4) `win.close()`, and revisit every test that ends through `process.exit()` -- 15e (`win.close()`, `test/ui-close.js` WINCLOSE, the `close` listener gone from `demo.js`) and 15f (`test/ui-commands.js` and conformance mode end by `win.close()`; every other window test keeps `process.exit()`, its comments corrected). (5) Test the close handshake with a real `WM_DELETE_WINDOW` -- 15e, `test/ui-close.js` DEFAULT, CANCEL, NATIVE and LATE, all through `x11_window_driver --close`. (6) Decide how a native listener cancels a close -- 15e, `hnui_cancel_close` (Ruling 79), `test/ui-close.js` NATIVE and `test/ui-conformance.js`. (7) The refused-`post()` edges -- 15b-15d, each traced; `TickMachineTest.NoTickIsSentOnceTheTableIsClosing`, `test/ui-exit-paths.js` COMMANDS and THROW, `test/ui-close.js` LATE. (8) `process.exit()` inside an event listener letting a frame callback run after `'exit'` -- 15b, `test/ui-exit-paths.js` LISTENER.

  **Rulings made in Task 15** (Design (f) and the cancel API of the Task 15 plan):
  - **Ruling 70 — abnormal return ends in `_exit()` on the JS thread.** The window is never torn down: `sapp_run()` returning would close the X display the JS thread's GL context was created on, and on macOS cannot carry a status. `uiBeforeRuntimeCleanup` restores the terminal and calls `_exit(exitCode)`; step 16 never runs in a process that requested a window.
  - **Ruling 71 — there is no `quit` command.** Removed from the spec's command list; closing is decided and carried out on the JS thread, and the window lives "until the process ends".
  - **Ruling 72 — a rejected `createWindow` releases the referenced handle** (`rejectWith`), so a script that handles the rejection and returns ends.
  - **Ruling 73 — a `close` listener cancels with `event.preventDefault()`**, synchronously, during delivery; a later call does nothing. With no listener or none that cancels, the process ends as if `process.exit()` had been called with no argument.
  - **Ruling 74 — ticks are drained on entering CLOSING only when a clipboard Promise was rejected, and never on the fatal path**, since Node's `process.exit()` runs no queued callback and a throwing handler after an uncaught exception would replace the error being reported.
  - **Ruling 75 — "fails pending synchronous clipboard requests" means both kinds**, and what a program can observe of the synchronous kind is `hnui_read_clipboard_sync` from an `'exit'` listener returning `HNUI_ERROR` at once.
  - **Ruling 76 — in CLOSING a command is dropped and traced, never thrown**: `requestAnimationFrame` returns 0, `ui.readClipboard()` returns a Promise already rejected with `ERR_CLIPBOARD_FAILED`, and `createWindow` throws.
  - **Ruling 77 — `win.close()` emits no `close`**; it enters CLOSING and calls `process.exit()` with no argument. A replaced `process.exit` that returns leaves the window in CLOSING.
  - **Ruling 78 — CLOSED is the `_exit()`**, with no value in the state enum; the window stays on screen while `'exit'` listeners run.
  - **Ruling 79 — `bool hnui_cancel_close(hnui_window *w)`**, true only while an `HNUI_EVENT_CLOSE_REQUESTED` for `w` is being delivered, false otherwise and once CLOSING; no `HNUI_ABI_VERSION` bump, because an added export is something an already-compiled addon tolerates.

- **Task 16 — The Metal backend**, implementing **all** of `backend.h`: device and environment setup, surface allocation, `mtl_textures[0]` injection, the presenting pipeline, invalid-swapchain handling that releases read references without a blit, the required command-buffer accessor patch, completion handlers on Sokol's own buffer, `@autoreleasepool` around all worker work, and module-internal maintenance commits. Backend-aware tests: the GL polling/blocking assertions do not apply here. **Ruling 46 (execution) — add the `hnui_gpu_view` / `sg_view` static_asserts to `metal_backend.mm` as well, but they are no longer the only copy.** Task 8 first placed them in `gl_backend.cpp`, which is compiled only on Linux, while CI enables `HERMES_NODE_ENABLE_UI` on macOS too — so the check that the ABI struct still matches upstream would not have existed on the one platform this task is about. That gap is closed *before* this task rather than by it: the asserts also live in `unittests/HnuiAbiTest.cpp`, which is already UI-gated and links the `sokol` INTERFACE target, so every UI platform is covered today. Add the backend-local copy anyway — it costs nothing and keeps the check next to the code that depends on it — but the platform hole is not this task's to close. **Stated honestly, this is now a suggestion rather than an obligation:** `unittests/HnuiAbiTest.cpp` carries the same asserts under a plain `HERMES_NODE_ENABLE_UI` guard and links Sokol's declarations, so every UI platform is already covered and omitting the Metal copy fails nothing. It is listed because locality is worth having, not because anything breaks without it — do not let it read as a debt that was paid by doing nothing. **Carried from Slice C (Ruling 55):** the clipboard read on macOS is `sapp_get_clipboard_string()` as it stands -- it reads `NSPasteboard` directly and has no stale-buffer path, unlike the X11 getter that `x11_clipboard.cpp` replaces -- behind the same `ClipboardResult` interface; and `ui_module_nowindow.cpp`'s five refusing exports go away with it. The reference addon passes `depth_format = SG_PIXELFORMAT_NONE` to `sokol_gl`; check the Metal environment needs the same. **Inherited from Task 15:** the quit is always cancelled and the JS thread `_exit`s; the macOS backend must keep that, and must check that `sapp_cancel_quit()` called from `event_cb` during `windowShouldClose:` cancels the close the way it does on X11 -- `test/ui-close.js` is the model, with a real close request rather than an injected one. **Termination is a second route to quitting, and Sokol does not cancel it** (found by the whole-branch review of Task 15, checked against `external/sokol/sokol/sokol_app.h`): `[NSApp terminate:]` -- the Dock's Quit, a Cmd-Q menu item, logout -- never raises `QUIT_REQUESTED`, which comes only from the window delegate's `windowShouldClose:`. Sokol's app delegate implements `applicationShouldTerminateAfterLastWindowClosed:` and `applicationWillTerminate:` but no `applicationShouldTerminate:`, so AppKit terminates without asking: `applicationWillTerminate:` calls `_sapp_call_cleanup()` (our `cleanup_cb`), `_sapp_macos_discard_state()` and `_sapp_discard_state()`, and AppKit then exits 0 from the main thread while the JS thread is still running. That breaks three things Task 15 established -- every quit request is cancelled, `cleanup_cb` never runs, the JS thread ends the process with its status. This task must either intercept termination (for instance by adding `applicationShouldTerminate:` to Sokol's delegate class at run time with `class_addMethod` on `_sapp_macos_app_delegate`, leaving the vendored source unmodified, answering `NSTerminateCancel` and forwarding the request to the JS thread as `close`) or document the exception where those guarantees are stated -- CLAUDE.md, the spec's Lifecycle, `hnui.h` -- and test whichever it chooses. **Two tests that assume the GL backend:** `test/ui-exit-tty.js` and `test/ui-route-refusal.js` (all three runs, RET included) reach "fails before `sapp_run()`, no display needed" through `HERMES_NODE_UI_GL_ROUTE=bogus`, are gated on `ui-gpu` without `display`, and use `timeout` / `timeout --foreground`, which a stock macOS lacks. Once this task makes `ui-gpu` true on macOS they will open a real window or fail; re-gate them (a GL-route feature, or `linux`) or give them a route the Metal backend also refuses before `sapp_run()`, bounded without `timeout`. **From the Mac probes of 2026-11-19:** (i) `sokol_gfx_impl.cpp` and `sokol_gfx_present_impl.cpp` must be compiled as Objective-C++ under `SOKOL_METAL` (`LANGUAGE OBJCXX` or `.mm`); a plain C++ compile fails inside Foundation's headers without naming the cause, so `sokol_config.h` should `#error` when `SOKOL_METAL` is defined and `__OBJC__` is not. Compiled that way both files build with no warnings under Apple clang 21, `utils/check-sgp-isolation.py` reads the Mach-O objects correctly through Apple's `nm` (156 `sg_*` against 156 `sgp_*`, and an un-renamed control is refused), and the two instances ran concurrently on one `MTLDevice` under Metal API and GPU validation. That run was offscreen and clear-only: no drawable, presentation, shader or pipeline. (ii) `sokol_gl` needs `depth_format = SG_PIXELFORMAT_NONE` on Metal exactly as on GL: the rejection is `_sg_validate_pipeline_desc`, which is backend-independent. (iii) Do not use `sapp_get_clipboard_string()` as it stands after all: it has no stale-buffer path, but it truncates silently at the clipboard buffer size and returns an empty string both for empty text and for a pasteboard with no string, so it cannot fill `ClipboardResult`'s oversize and no-text cases. Read `NSPasteboard` directly on the platform thread, as `x11_clipboard.cpp` reads X11.
- **Task 17 — The full gated suite.** `display` feature that actually starts Xvfb in the RUN line rather than merely detecting `xvfb-run`; macOS gating that does not silently skip; resize-generation, lifecycle and conformance assertions. **Ruling 16's destroy callback: first execution and the owner-thread assertion were discharged in Task 11d.** An injected `RESIZED` starts generation 2 there, and `test/ui-resize-generation.js` asserts from tracing that generation 1's last reference is dropped on the platform thread (`owner=0`) and that the posted destruction then runs on the owner thread (`owner=1`), after generation 2 was created; destroying inline from the callback instead turns it red. **What remains here is a resize of the REAL framebuffer**, which event injection does not perform: an injected event overrides the reported window state while the swapchain stays its original size. Note also that a refused `post()` is reported only through `uiTrace`, which is gated on `HERMES_NODE_DEBUG_NATIVE=UI`: acceptable because a refusal can only follow `close()`, at shutdown, but it means a test looking for that report must enable tracing. Separately, do not assume Task 9's `CompletionGate` can be driven from a script: it is wired into production under `uiTestHooksEnabled()`, but `holdProducer`/`holdConsumer` have no JS surface and no C entry point, so on the real pipeline it is a pass-through. Deterministic lifecycle assertions here need one added, or must work through the unit-test level. **Changed by Slice C's fix wave:** the resize of the REAL framebuffer is now asserted by `test/ui-real-events.js` (an `XResizeWindow` from a second client, a new generation of the new size, a frame published in it), so that case is discharged; and window tests no longer use `xvfb-run -a`, whose display allocation is not atomic and collided at about 1-4% under 16-way load -- they run under `utils/with-xvfb.py` (`%with-xvfb`), which claims a display with `Xvfb -displayfd` and tears it down. What remains here: macOS gating that does not silently skip, and a sweep of the stderr checks -- `test/ui-create-window.js`, the DEMO run of `test/ui-real-events.js`, `test/ui-sokol-panic.js` and `test/ui-sokol-validation.js` do not yet exclude `[sapp:error`. Also still open: a ten-line test that `hnui_get_window()` is NULL after a late `createWindow` rejection (provokable by clearing `globalThis.require` between the call and its `await`). **The lifecycle assertions this task was waiting for exist** since Task 15: `test/ui-exit-paths.js`, `test/ui-close.js` and `test/ui-exit-tty.js`, plus the traced runs added to `test/ui-windowless.js` and `test/ui-route-refusal.js`.

---

## Self-Review

**Coverage.** Spec sections map as: Architecture → 2, 3, 4, 6; Surfaces → 2, 8, 10, 16; Frames → 4, 13; Input/platform → 11, 12; Lifecycle → 6, 8, 15; JS API → 8, 11, 13; C API → 7, 10, 14; reference addon → 10, 14; Build/integration → 1, 5, 8; Testing → 9, 17.

**Against the previous review.** Finding 1 (unreachable gates) → slices; 2, 3, 4, 5, 7 (protocol races and identity) → Task 2's single lock and `SurfaceRef`; 6 (owner thread) → Task 3; 8 (clipboard identity) → Task 4; 9 (clipboard status) → Task 12; 10, 11 (CMake and generator) → Task 8; 12 (C ABI) → Task 7; 13 (startup) → Task 8; 14 (CLOSING) → Task 15; 15 (Metal) → `backend.h` in Task 8, implemented in 16; 16 (lit features) → Task 6 and Task 17; 17 (weak tests) → mutation requirement throughout; 18 (hooks) → Task 9; 19 (rAF) → Task 13; 20, 21 (commands, prerequisites) → full commands everywhere, and Task 5 Step 1 / Task 6 Step 3.

**Known gaps, stated rather than implied.** Tasks 16–17 are outlined, not yet written to step level (Task 15 was, in its own file); each is expanded when its slice begins, and no task in Slices A–C depends on that expansion. Concurrent GL execution of both `sokol_gfx` instances is first exercised in Task 10 — if it fails there, the plan stops rather than proceeding. Task 16 has no CI: macOS runners' Metal availability is unverified.
