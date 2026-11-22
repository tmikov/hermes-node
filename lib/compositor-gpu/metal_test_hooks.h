/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_NODE_COMPAT_COMPOSITOR_GPU_METAL_TEST_HOOKS_H
#define HERMES_NODE_COMPAT_COMPOSITOR_GPU_METAL_TEST_HOOKS_H

namespace hermes {
namespace node_compat {

/// macOS only (`metal_backend.mm`), for the test hooks: hold the producer's
/// frames in flight on the GPU, so that a test can make maintenance meet a
/// frame that has not completed -- which otherwise lasts as long as the
/// GPU takes to draw it, and no test can aim at that.
///
/// Any thread (the window driver calls them on the main thread). Effective
/// only with the test hooks on: submitAndTrackCompletion() consults the
/// hold only then.
///
/// Three limits. The hold catches whatever frame is committed next --
/// including one whose callback has already run, if a script asks from
/// that callback's own continuation -- so hold only once the frame before
/// has been seen presented (measured). A frame held for more than a few
/// seconds fails: the GPU's watchdog ends it with
/// `kIOGPUCommandBufferCallbackErrorTimeout`, and the graphics session
/// ends with it (measured); a hold is for milliseconds. And at most two
/// frames can be held: the producing instance's first pass of a frame
/// waits on Sokol's in-flight semaphore, created with
/// SG_NUM_INFLIGHT_FRAMES (2), so a third frame blocks the JS thread in
/// sg_begin_pass until a held one completes -- which, with the JS thread
/// blocked, nothing will release.

/// From now on, every producer frame committed waits on the GPU, after its
/// passes, for a shared event that nothing signals yet.
/// \return false if there is no backend.
bool metalTestHoldProducerGpu();

/// Stop holding, and signal the event -- not now, but from another thread
/// once the worker's next loop turn has finished, so that the held frames
/// complete while the worker's loop has nothing to do (the case the
/// maintenance wake is for). Returns at once.
/// \return the producer frames in flight when called, or -1 if there is
/// no backend.
int metalTestReleaseProducerGpu();

} // namespace node_compat
} // namespace hermes

#endif // HERMES_NODE_COMPAT_COMPOSITOR_GPU_METAL_TEST_HOOKS_H
