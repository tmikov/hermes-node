/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_NODE_COMPAT_COMPOSITOR_GPU_GL_PRESENT_H
#define HERMES_NODE_COMPAT_COMPOSITOR_GPU_GL_PRESENT_H

#include <hermes/node-compat/compositor/surface_table.h>

#include <cstdint>
#include <string>

namespace hermes {
namespace node_compat {

/// The presenting half of the GL backend: everything that runs on the
/// platform thread and touches the **`sgp_*`** instance of `sokol_gfx`.
///
/// # Why this is a separate translation unit, and must stay one
///
/// The two `sokol_gfx` instances differ only by a rename applied **before
/// the header's declarations** (`sgp_rename.h`, and see
/// `sokol_gfx_present_impl.cpp` for why "before the declarations" rather
/// than "before the implementation"). A rename implemented as
/// `#define sg_x sgp_x` rewrites every `sg_*` call in the translation unit
/// that includes it -- all of them or none. So one file cannot call both
/// instances: `gl_backend.cpp` needs the producing `sg_*` names (it
/// allocates the surfaces the JS thread draws into, and holds the
/// `static_assert`s that tie `hnui_gpu_view` to the real `sg_view`), and
/// this file needs the presenting `sgp_*` ones. Splitting them is not a
/// tidiness choice; a single file would have to spell one instance's
/// functions by hand, which is precisely the hand-maintained list
/// `gen-sokol-rename.py` exists to abolish.
///
/// Nothing in this header names a Sokol type, so `gl_backend.cpp` can hold
/// one of these and call it without inheriting the other instance's
/// declarations. That is the same rule `backend.h` follows for the same
/// reason, one level up.
///
/// # Threading
///
/// **Every method belongs to the platform thread**, which owns both
/// `sokol_app` and the `sgp_*` instance. The one piece of state shared
/// with the worker is the generation-to-texture map, which lives in
/// `gl_backend.cpp` under its own mutex and is passed in to `wrap()`
/// rather than read from here.
class GlPresenter {
 public:
  GlPresenter() = default;
  ~GlPresenter();

  GlPresenter(const GlPresenter &) = delete;
  GlPresenter &operator=(const GlPresenter &) = delete;

  /// `sgp_setup()` plus the blit pipeline: the full-screen-triangle
  /// shader, a linear-filtering sampler, and the pipeline state matched to
  /// the swapchain. Called from `init_cb`, with `sokol_app`'s own context
  /// current.
  bool setup(std::string &error);

  /// The swapchain's colour format and sample count, as
  /// `sg_pixel_format`/int values, read from `sapp_get_environment()`
  /// during `setup()`.
  ///
  /// The **producer** needs these for its own `sg_setup()`: its surfaces
  /// must be in the swapchain's colour format (spec: Surfaces), and it
  /// cannot ask `sokol_app` itself, both because that would put a
  /// windowing dependency in the producing translation unit and because
  /// the answer must be the one this instance was actually set up with.
  /// Carried as plain ints so this header names no Sokol type.
  int colorFormat() const {
    return colorFormat_;
  }
  int sampleCount() const {
    return sampleCount_;
  }

  /// Give this instance its own handles for \p gen's surfaces, by texture
  /// injection (`sg_image_desc.gl_textures[0]`), and a texture view over
  /// each. \p textures is `kSlotsPerGeneration` GL texture names created
  /// by the producer; they are visible here because the two contexts share
  /// a share group (measured on llvmpipe and on AMD radeonsi -- see
  /// `docs/notes/2026-09-21-linux-hardware-spike-results.md`, question E).
  ///
  /// Idempotent: wrapping a generation that is already wrapped does
  /// nothing and succeeds, so the frame callback may call it
  /// unconditionally for whatever it is about to present.
  bool wrap(
      GenerationId gen,
      const uint32_t *textures,
      int count,
      int width,
      int height,
      std::string &error);

  /// Destroy this instance's wrappers for \p gen. The caller decides
  /// *when* (`SurfaceTable::wrappersReleasable`); this only carries it
  /// out.
  void releaseWrappers(GenerationId gen);

  /// Whether \p gen has wrappers here right now -- so the frame callback
  /// can ask `wrappersReleasable()` about the generations that actually
  /// have something to release, rather than about every id it has ever
  /// seen.
  bool isWrapped(GenerationId gen) const;

  /// `sapp_acquire_swapchain()`, exactly once per frame callback. False
  /// means this frame has no usable swapchain.
  bool acquireSwapchain();

  int swapchainWidth() const {
    return swapchainWidth_;
  }
  int swapchainHeight() const {
    return swapchainHeight_;
  }

  /// Fill the swapchain with the clear colour and commit. Used while
  /// nothing has ever been published.
  void clear();

  /// Blit \p ref's surface into the swapchain with linear filtering,
  /// stretching if the sizes differ, then commit and insert a fence.
  ///
  /// \return the consumer fence (a `GLsync`) for the caller to poll, or
  ///   nullptr if the surface could not be drawn -- in which case the
  ///   caller must release its read reference itself, since no completion
  ///   will ever arrive to do it.
  void *blitAndFence(SurfaceRef ref);

  /// `glReadPixels` over the just-presented back buffer, flipped to
  /// top-row-first RGBA8. See `CompositorBackend::readPresentedPixels`.
  bool readPixels(int width, int height, uint8_t *out);

 private:
  struct Wrapped;

  /// Opaque to this header for the same reason the class comment gives:
  /// the wrappers are `sg_image`/`sg_view` handles, and naming those here
  /// would drag Sokol's declarations into `gl_backend.cpp`.
  struct State;
  State *state_ = nullptr;

  int colorFormat_ = 0;
  int sampleCount_ = 1;
  int swapchainWidth_ = 0;
  int swapchainHeight_ = 0;
};

} // namespace node_compat
} // namespace hermes

#endif // HERMES_NODE_COMPAT_COMPOSITOR_GPU_GL_PRESENT_H
