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
| `tools/hermes-node-ui/main.cpp`, `ui_host.{h,cpp}`, `ui_module.cpp`, `hnui_api.cpp` | Binary entry, thread/lifecycle host, the `hermes-node:ui` module, the C API implementation. |
| `test/fixtures/hnui-demo/` | Reference addon: visual, fixed-pattern, native-tick and conformance modes. |
| `unittests/CompositorTest.cpp` | GTest for everything in `lib/compositor/`. |
| `test/ui-*.js` | lit tests, gated on `ui` and `display`. |

## Slices

The tasks are grouped so that **each slice ends with something that runs**, and no task's gate depends on a later task.

- **Slice A (Tasks 1–4): foundations, no GPU.** Build scaffolding, the surface protocol, the owner dispatcher, the tick machine, the channels. All GTest.
- **Slice B (Tasks 5–10): one window presenting one known pattern.** Embedder modules, the binary, the window and shared context, the minimal C ABI, the minimal addon, presenting, the capture hook, the first end-to-end test.
- **Slice C (Tasks 11–14): the rest of the surface area.** Input and commands, clipboard with a real status, full rAF semantics, the full C API and conformance mode.
- **Slice D (Tasks 15–17): lifecycle, Metal, suite.** CLOSING on every exit path, the Metal backend against the same interface, the full gated suite.

**Slice A complete (2026-09-22): 45 tests in `unittests/CompositorTest.cpp`** (49 after the whole-branch review's fixes). Tasks 1–4 are ticked below. Rulings made while executing them are written into the task they belong to, marked **Ruling N (execution)**; the ones that bind later work are Ruling 16 (Task 3 Step 5, carried to Tasks 8/10) and Ruling 9 (Task 2's `take()` behaviour, which Task 4 relies on).

**Slice B complete (2026-09-25): 33 commits and 75 files before the branch was squashed.** Tasks 5–10 are ticked below. The suite stands at 414 unit tests and 201 JS tests. The slice's gate is met: `hermes-node-ui` opens a real window, the reference addon draws a fixed pattern into a compositor surface on the JS thread, the platform thread blits it, and `test/ui-present.js` compares the presented pixels **exactly** — 0 or 255, no tolerance, samples kept 8px from any edge. Obligations this slice carried forward, each written into the task that discharges it rather than into any external note: the `sapp_event` → `UiEvent` conversion's coverage and the dispatch loop's address-stability constraint (Task 11), a backend-local copy of the `sg_view` asserts (Task 16, optional — `HnuiAbiTest` already covers every platform), and the first real execution of the posted destroy callback (Task 17). Progress notes and the newcomer's orientation live in `docs/superpowers/plans/progress-ui-compositor.md`; known defects live in `dz/`, not here.

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

### Tasks 11–17

Each follows the same shape — failing test naming its mutation, full build-and-run commands, implementation, verification, format, commit — and each is written out in the same detail as Tasks 1–10 when its slice begins. Their scope and the findings they answer:

- **Task 11 — Input, window state, commands.** Native listeners before the JS `emit`; main-thread attachment of `CLIPBOARD_PASTED` text and `FILES_DROPPED` paths; drag-and-drop enabled with its limits documented. **Carried from Task 7 (Ruling 45): the dispatch loop written here must respect ADDRESS STABILITY, not merely liveness.** `hnui_event`'s `text` and `paths` borrow pointers into the source `UiEvent`'s `std::string` storage, which for short strings lives inside the `UiEvent` object itself. `InputQueue::drain` appends into a caller-reused buffer (`channels.h`), so a loop iterating that buffer where any listener triggers a second `drain()` reallocates it, moves every `UiEvent`, and hands the addon a pointer into freed storage — with no ASAN report in a release addon. Dispatch must iterate a buffer that nothing appends to while dispatching, and no source event may be moved or erased for the duration of a borrow. **This requirement gets a named test and a named mutation like every other step in this plan, or it is discharged by an executor asserting their loop is correct:** write a listener that triggers a second `drain()` into the same buffer while a borrow is live, run it under ASAN, and state the mutation — remove the guard that keeps dispatch off the appended-to buffer — that must make it report a use-after-free. **Also carried from Task 9 (Ruling 48): the synthetic-event injector must stop bypassing the converter this task writes.** Task 9 injects at the `UiEvent` level, not at `sapp_event` as spec:'s test-hook section says, because the `sapp_event` → `UiEvent` conversion did not exist yet and `lib/compositor-gpu/` links no `sokol_app`. That staging was correct, but it leaves the converter — where input bugs actually live — exercised by nothing. Discharge it one of two ways. **Preferred:** move the injection point *above* the converter, so a synthetic event produces the converter's input rather than its output and runs the same path a real event does; if that means moving `EventInjector` into `tools/hermes-node-ui/` to hold platform-shaped records without dragging `sokol_app.h` into `lib/compositor-gpu/`, moving it is the cheaper price. **Otherwise:** add a direct, table-driven unit test of the **`sapp_event` → `UiEvent`** conversion that this task writes — covering every `sapp_event` type it handles, including modifier and button mapping and the `CLIPBOARD_PASTED` / `FILES_DROPPED` payload attachment. **Name that conversion specifically, because the obvious near-miss discharges nothing:** Task 7's `UiEvent` → `hnui_event` converter already has five table-driven tests in `unittests/HnuiAbiTest.cpp`, and pointing at those satisfies the words but not the requirement. The conversion with zero coverage is the *platform-to-internal* one, which does not exist yet and is precisely what `EventInjector` bypasses by injecting at `UiEvent`.
- **Task 12 — Clipboard with a real status** (finding 9). Implements **either** the Sokol patch that returns a status **or** our own X11 read, because Sokol's getter returns its previous buffer on timeout, INCR and oversize (`sokol_app.h:13233`). Test: a successful read, then a forced failure, must **not** return the earlier text.
- **Task 13 — Full rAF semantics** (finding 19). Microtask checkpoints between callbacks; one registration-ordered batch mixing native and JS callbacks; handled versus fatal exception paths; cancellation from inside a microtask.
- **Task 14 — The full C API and the conformance mode.** Every remaining `hnui_*` entry point, the quiescence report that makes listener-userdata freeing safe, and the addon mode that calls each once. **Three obligations carried from Task 7 (Ruling 45), all of which need a writer that does not exist until this task:** (1) the size-prefix mechanism gets a BEHAVIOURAL test, not an arithmetic one — hand a writer a poisoned buffer with `size` set to exactly `HNUI_*_MIN_SIZE`, assert every byte past `size` is untouched, and assert a `size` one byte short is rejected with no write at all. Task 7 could only test `>=` on two integers, because it had no writer to test against, and nothing yet simulates an old addon against a new host. (2) The public→internal enum direction must **reject** an out-of-range value rather than index on it; Task 7's internal→public direction is an exhaustive switch with a defensive fallthrough, and the reverse direction is where an addon's bad value first enters. (3) `hnui.h` must bake in **the vendored Sokol commit** alongside `HNUI_ABI_VERSION` and provide `HNUI_CHECK_VERSION(env)`, which spec:838-841 requires and no task currently owns — `HNUI_ABI_VERSION` alone is correctly scoped to `hnui_abi.h` and does not cover it.
- **Task 15 — Lifecycle and CLOSING** (finding 14). CLOSING must be entered **before** `'exit'` listeners run on all three paths: `win.close()`, explicit `process.exit()` (listeners emit at `node_process.cpp:649`), and the fatal-exception path (`process-events.js:82`). Includes the cancelable `QUIT_REQUESTED` handshake, which no task previously owned, and the pre-cleanup lifecycle hook that closes the UI handle so `UvEventLoop::close()`'s `uv_run(UV_RUN_DEFAULT)` can finish.
- **Task 16 — The Metal backend**, implementing **all** of `backend.h`: device and environment setup, surface allocation, `mtl_textures[0]` injection, the presenting pipeline, invalid-swapchain handling that releases read references without a blit, the required command-buffer accessor patch, completion handlers on Sokol's own buffer, `@autoreleasepool` around all worker work, and module-internal maintenance commits. Backend-aware tests: the GL polling/blocking assertions do not apply here. **Ruling 46 (execution) — add the `hnui_gpu_view` / `sg_view` static_asserts to `metal_backend.mm` as well, but they are no longer the only copy.** Task 8 first placed them in `gl_backend.cpp`, which is compiled only on Linux, while CI enables `HERMES_NODE_ENABLE_UI` on macOS too — so the check that the ABI struct still matches upstream would not have existed on the one platform this task is about. That gap is closed *before* this task rather than by it: the asserts also live in `unittests/HnuiAbiTest.cpp`, which is already UI-gated and links the `sokol` INTERFACE target, so every UI platform is covered today. Add the backend-local copy anyway — it costs nothing and keeps the check next to the code that depends on it — but the platform hole is not this task's to close. **Stated honestly, this is now a suggestion rather than an obligation:** `unittests/HnuiAbiTest.cpp` carries the same asserts under a plain `HERMES_NODE_ENABLE_UI` guard and links Sokol's declarations, so every UI platform is already covered and omitting the Metal copy fails nothing. It is listed because locality is worth having, not because anything breaks without it — do not let it read as a debt that was paid by doing nothing.
- **Task 17 — The full gated suite.** `display` feature that actually starts Xvfb in the RUN line rather than merely detecting `xvfb-run`; macOS gating that does not silently skip; resize-generation, lifecycle and conformance assertions. **Carried from Task 10: this task's resize assertions are the first code that will actually EXECUTE Ruling 16's destroy callback.** Task 10 wired it correctly — the production `SurfaceTable` posts `destroyGenerationSurfaces()` through `OwnerDispatch`, never inline — but that build allocates one generation and never retires it, so the callback has never fired at runtime. A resize is what retires a generation, so assert here that the posted destroy actually runs, on the owner thread, after the last reference drops. Note also that a refused `post()` is reported only through `uiTrace`, which is gated on `HERMES_NODE_DEBUG_NATIVE=UI`: acceptable because a refusal can only follow `close()`, at shutdown, but it means a test looking for that report must enable tracing. **And the "on the owner thread" half of this obligation is currently unobservable** — the existing destroy trace does not say which thread ran it, so either that trace or the new assertion must record the thread, or the claim cannot be checked. Separately, do not assume Task 9's `CompletionGate` can be driven from a script: it is wired into production under `uiTestHooksEnabled()`, but `holdProducer`/`holdConsumer` have no JS surface and no C entry point, so on the real pipeline it is a pass-through. Deterministic lifecycle assertions here need one added, or must work through the unit-test level.

---

## Self-Review

**Coverage.** Spec sections map as: Architecture → 2, 3, 4, 6; Surfaces → 2, 8, 10, 16; Frames → 4, 13; Input/platform → 11, 12; Lifecycle → 6, 8, 15; JS API → 8, 11, 13; C API → 7, 10, 14; reference addon → 10, 14; Build/integration → 1, 5, 8; Testing → 9, 17.

**Against the previous review.** Finding 1 (unreachable gates) → slices; 2, 3, 4, 5, 7 (protocol races and identity) → Task 2's single lock and `SurfaceRef`; 6 (owner thread) → Task 3; 8 (clipboard identity) → Task 4; 9 (clipboard status) → Task 12; 10, 11 (CMake and generator) → Task 8; 12 (C ABI) → Task 7; 13 (startup) → Task 8; 14 (CLOSING) → Task 15; 15 (Metal) → `backend.h` in Task 8, implemented in 16; 16 (lit features) → Task 6 and Task 17; 17 (weak tests) → mutation requirement throughout; 18 (hooks) → Task 9; 19 (rAF) → Task 13; 20, 21 (commands, prerequisites) → full commands everywhere, and Task 5 Step 1 / Task 6 Step 3.

**Known gaps, stated rather than implied.** Tasks 11–17 are outlined, not yet written to step level; each is expanded when its slice begins, and no task in Slice A or B depends on that expansion. Concurrent GL execution of both `sokol_gfx` instances is first exercised in Task 10 — if it fails there, the plan stops rather than proceeding. Task 16 has no CI: macOS runners' Metal availability is unverified.
