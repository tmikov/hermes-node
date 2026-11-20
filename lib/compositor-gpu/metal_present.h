/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_NODE_COMPAT_COMPOSITOR_GPU_METAL_PRESENT_H
#define HERMES_NODE_COMPAT_COMPOSITOR_GPU_METAL_PRESENT_H

#include <hermes/node-compat/compositor/surface_table.h>

#include <cstdint>
#include <string>

namespace hermes {
namespace node_compat {

/// The presenting half of the Metal backend: everything that runs on the
/// platform thread and touches the **`sgp_*`** instance of `sokol_gfx`.
///
/// A separate translation unit for exactly the reason `GlPresenter` is one
/// (see `gl_present.h`): the rename that makes this file's `sg_*` calls
/// reach the presenting instance rewrites every call in the unit, so the
/// producing half, `metal_backend.mm`, cannot live beside it.
///
/// Nothing in this header names a Sokol or an Objective-C type, so
/// `metal_backend.mm` can hold one of these without inheriting the other
/// instance's declarations, and so the header stays plain C++.
///
/// # Threading
///
/// **Every method belongs to the platform thread**, which owns `sokol_app`
/// and the `sgp_*` instance -- except the three getters `device()`,
/// `colorFormat()` and `sampleCount()`. On macOS the platform thread is
/// also the main thread, which Metal does not require but AppKit and
/// `sokol_app` do.
///
/// Those three are written once, by `setup()` in `init_cb`, and read by
/// the producing half on the JS thread (the worker) for its own
/// `sg_setup()` (Task 16c). That is safe without a lock because of an
/// ordering, not by luck: `init_cb` returns from `setup()` and only then
/// posts `onSharedContextReady` through the dispatcher, whose post and
/// drain are a mutex hand-off, and the worker reads them inside that
/// posted call or later. Nothing writes them again.
///
/// # What exists after Task 16c
///
/// Setup, the per-frame swapchain acquisition, the clear, the wrappers, the
/// blit with its consumer completion handler, and the test hooks' read-back
/// of the presented drawable. Task 16d adds the invalid-swapchain traces.
class MetalPresenter {
 public:
  MetalPresenter() = default;
  ~MetalPresenter();

  MetalPresenter(const MetalPresenter &) = delete;
  MetalPresenter &operator=(const MetalPresenter &) = delete;

  /// `sgp_setup()` against `sokol_app`'s environment, plus the blit
  /// pipeline: the full-screen-triangle MSL shader, a linear clamping
  /// sampler, and the pipeline state matched to the swapchain. Called from
  /// `init_cb`.
  ///
  /// A refusal before the instance is live leaves this presenter inert:
  /// `acquireSwapchain()` then returns false on every frame, so `onFrame`
  /// presents nothing rather than reaching an instance never set up.
  bool setup(std::string &error);

  /// The `id<MTLDevice>` the presenting instance was set up on -- Sokol's,
  /// from `sokol_app`'s environment, the device its layer draws with --
  /// as a `__bridge` (not retained) pointer, or null before `setup()` made
  /// the instance live. The producer must allocate on this device: the two
  /// instances share textures, and a texture belongs to one device.
  void *device() const;

  /// The swapchain's colour format (an `sg_pixel_format`) and sample count,
  /// as the presenting instance was set up with them. The producer needs
  /// both for its own `sg_setup()` (Task 16c); plain ints so this header
  /// names no Sokol type.
  int colorFormat() const {
    return colorFormat_;
  }
  int sampleCount() const {
    return sampleCount_;
  }

  /// `sapp_acquire_swapchain()` through `sglue_swapchain()`, **exactly once
  /// per frame callback**. False means this frame has no usable swapchain:
  /// Sokol reported it invalid (an obscured window), or it carries no
  /// drawable. The second guard is ours because `_sapp_mtl_swapchain_next`
  /// only `SOKOL_ASSERT`s the drawable, so a Release build would otherwise
  /// carry nil into `sg_begin_pass`.
  bool acquireSwapchain();

  /// The size of this frame's drawable, read from the drawable's own
  /// texture -- not from `sapp_width()`, which is what was asked for rather
  /// than what is presented.
  int swapchainWidth() const {
    return swapchainWidth_;
  }
  int swapchainHeight() const {
    return swapchainHeight_;
  }

  /// Fill the swapchain with the presenter's clear colour
  /// (`kPresenterClearLevel`) and commit. Used while nothing has ever been
  /// published. No completion handler: nothing holds a reference to
  /// anything this frame reads.
  void clear();

  /// Test hooks only, from `init_cb` after `setup()`: clear
  /// `framebufferOnly` on Sokol's `CAMetalLayer` so the drawable can be
  /// copied out, read it back, and arm the staging copy `blit()` encodes
  /// only if the read-back says it is clear. Traced as `capture: staging
  /// copy enabled (framebufferOnly=0)`, or `NOT enabled` with what was read.
  void enableCapture();

  /// Wrap one generation's surfaces for this instance, by texture
  /// injection (`mtl_textures[0]`): \p mtlTextures are \p count
  /// `id<MTLTexture>` as `__bridge` pointers, the producer's, which must stay
  /// alive for the duration of the call. Idempotent: a generation already
  /// wrapped is left alone and succeeds.
  bool wrap(
      GenerationId gen,
      void *const *mtlTextures,
      int count,
      int width,
      int height,
      std::string &error);

  /// Destroy this instance's wrappers for \p gen. The caller decides when
  /// (`SurfaceTable::wrappersReleasable`).
  void releaseWrappers(GenerationId gen);

  /// Whether \p gen has wrappers here right now.
  bool isWrapped(GenerationId gen) const;

  /// Draw \p ref into this frame's swapchain and commit. \p onComplete is
  /// installed on that frame's command buffer before the commit and runs
  /// when the buffer finished, on a Metal thread, with ok=false if it ended
  /// in error. False if nothing was submitted -- no valid swapchain, no
  /// wrapper for the generation -- in which case \p onComplete is never
  /// called. Under the test hooks the drawable is also copied into a
  /// staging buffer in the same command buffer, for readPixels().
  bool blit(
      SurfaceRef ref,
      void (*onComplete)(void *user, SurfaceRef ref, bool ok),
      void *user);

  /// The last blit's drawable, as RGBA rows top first: waits for that
  /// command buffer, then converts the staging copy from BGRA. False if the
  /// capture is not armed, no blit has been encoded since, the size asked
  /// for is not the drawable's, or the buffer did not complete.
  bool readPixels(int width, int height, uint8_t *out);

 private:
  /// Opaque here for the reason the class comment gives: the members are
  /// Sokol handles and Objective-C objects.
  struct State;
  State *state_ = nullptr;

  int colorFormat_ = 0;
  int sampleCount_ = 1;
  int swapchainWidth_ = 0;
  int swapchainHeight_ = 0;
};

} // namespace node_compat
} // namespace hermes

#endif // HERMES_NODE_COMPAT_COMPOSITOR_GPU_METAL_PRESENT_H
