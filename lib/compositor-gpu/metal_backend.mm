/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/// The Metal backend (Task 16): `CompositorBackend` on macOS.
///
/// This is the PRODUCING unit -- it includes `sokol_gfx.h` without the
/// rename, so every `sg_*` written here reaches the JS thread's instance --
/// and it holds the presenter, whose `sgp_*` calls live in
/// `metal_present.mm` for the reason `metal_present.h` gives.
///
/// # What exists in Task 16b
///
/// The route refusals, the device, the presenter's setup and the clear: a
/// window that shows the presenter's clear colour. **The worker's drawing
/// half refuses** -- `makeCurrentOnWorker` fails with a sentence saying so,
/// which rejects `createWindow` -- and every method below that the drawing
/// half owns is a stub naming the task that fills it, so that nothing reads
/// as finished.

#include "backend.h"

#include "metal_present.h"
#include "sokol_config.h"
#include "ui_trace.h"

// The producing instance's declarations: no rename. Included here, as in
// gl_backend.cpp, for the ABI checks below and for the drawing half.
#include <sokol_gfx.h>

#import <Metal/Metal.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>

namespace hermes {
namespace node_compat {

//===========================================================================
// RULING 45: hnui_gpu_view really is sg_view
//===========================================================================
//
// The same checks gl_backend.cpp makes, and they must be made here too: that
// file is not compiled on macOS, and Sokol's public structs change shape
// with the backend define, so the Metal build is a separate place for the
// ABI header and sokol_gfx.h to disagree. See gl_backend.cpp for why each
// one matters.

static_assert(
    sizeof(hnui_gpu_view) == sizeof(sg_view),
    "hnui_gpu_view must be the same size as Sokol's sg_view: hnui_target "
    "carries Sokol view handles across the C ABI and addons convert between "
    "the two by value. Update hnui_abi.h to match the vendored sokol_gfx.h.");

static_assert(
    alignof(hnui_gpu_view) == alignof(sg_view),
    "hnui_gpu_view and sg_view must have the same alignment.");

static_assert(
    offsetof(hnui_gpu_view, id) == offsetof(sg_view, id),
    "hnui_gpu_view::id must sit at the same offset as sg_view::id.");

static_assert(
    sizeof(hnui_gpu_view::id) == sizeof(sg_view::id),
    "hnui_gpu_view::id and sg_view::id must be the same width.");

static_assert(
    static_cast<long long>(_SG_PIXELFORMAT_FORCE_U32) ==
        static_cast<long long>(INT32_MAX),
    "sg_pixel_format's terminator is no longer INT32_MAX, so the enum's "
    "values may no longer fit in hnui_target::color_format's int32_t. "
    "Widening that field is an ABI break (HNUI_ABI_VERSION).");

namespace {

/// The GL backend's two variables. Read here only to be refused by name.
constexpr const char *kGlRouteEnvVar = "HERMES_NODE_UI_GL_ROUTE";
constexpr const char *kGlCompletionEnvVar = "HERMES_NODE_UI_GL_COMPLETION";

class MetalBackend final : public CompositorBackend {
 public:
  bool selectContextRoute(std::string &error) override;
  bool createSharedContext(std::string &error) override;
  bool makeCurrentOnWorker(std::string &error) override;

  bool createGenerationSurfaces(
      GenerationId gen,
      int width,
      int height,
      std::string &error) override;
  void destroyGenerationSurfaces(GenerationId gen) override;
  bool frameTarget(SurfaceRef ref, hnui_target *out) override;
  bool generationSize(GenerationId gen, int *width, int *height) override;
  void submitAndTrackCompletion(
      SurfaceRef ref,
      void (*onComplete)(void *user, SurfaceRef ref, bool ok),
      void *user) override;
  void setConsumerCompletion(
      void (*onComplete)(void *user, SurfaceRef ref, bool ok),
      void *user) override;

  bool wrapForPresenting(GenerationId gen, std::string &error) override;
  bool acquireFrameSwapchain() override;
  void presentClear() override;
  void blit(SurfaceRef ref) override;
  void releaseWrappers(GenerationId gen) override;
  void presentInvalid() override;
  void pollCompletions() override;
  bool readPresentedPixels(int width, int height, uint8_t *out) override;

  void workerLoopTurn() override;

 private:
  /// The device selectContextRoute() found, held strongly. sokol_app
  /// creates its layer on MTLCreateSystemDefaultDevice() too, and the
  /// presenter checks the two are one object.
  id<MTLDevice> device_ = nil;
  MetalPresenter presenter_;

  void (*consumerComplete_)(void *user, SurfaceRef ref, bool ok) = nullptr;
  void *consumerUser_ = nullptr;
};

/// The refusal for one of the GL variables, or false if it is unset.
bool refuseGlVariable(const char *name, const char *what, std::string &error) {
  const char *value = std::getenv(name);
  if (value == nullptr || value[0] == '\0')
    return false;
  error = std::string(name) + " is set ('" + value +
      "') but does not apply to this build, which presents through Metal "
      "and has no GL " +
      what + ". Unset it.";
  return true;
}

bool MetalBackend::selectContextRoute(std::string &error) {
  // There is no route to choose on Metal. What stage 1 does instead, in
  // this order and before anything touches AppKit, so that a machine with
  // no window server or no GPU gets a rejected Promise rather than a crash:
  //
  // (1) A GL variable that is set is refused by name. A setting that
  //     silently does nothing is the failure shape this codebase refuses,
  //     and the person who set it did so because something was wrong.
  if (refuseGlVariable(kGlRouteEnvVar, "context route", error))
    return false;
  if (refuseGlVariable(kGlCompletionEnvVar, "completion mode", error))
    return false;

  // (2) A machine with no Metal device is refused. The trace is inside the
  // pool too: this runs before sapp_run(), where no pool is in place, and
  // the name's UTF-8 buffer is autoreleased.
  @autoreleasepool {
    device_ = MTLCreateSystemDefaultDevice();
    if (device_ == nil) {
      error =
          "no Metal device is available (MTLCreateSystemDefaultDevice "
          "returned nil), so there is nothing to open a window on.";
      return false;
    }
    uiTrace("route: metal (device=%s)", [device_.name UTF8String]);
  }
  return true;
}

bool MetalBackend::createSharedContext(std::string &error) {
  // Nothing to create: on Metal the device is what both threads share
  // (spikes 2 and 5). What init_cb sets up here is the presenting instance.
  return presenter_.setup((__bridge void *)device_, error);
}

bool MetalBackend::makeCurrentOnWorker(std::string &error) {
  // Task 16c: install the worker's first autorelease pool, then sg_setup()
  // the producing instance on device_ with the presenter's colour format.
  // Until then the worker's half refuses, and createWindow rejects with the
  // sentence below.
  //
  // Nothing may have set the producing instance up: sg_setup() is this
  // method's alone, and the refusal is only safe because no addon's sg_*
  // call can then reach a half-initialised instance. Checked rather than
  // assumed. (Until 16c this is also the one reference this object makes to
  // the producing instance, which check-sgp-isolation requires of a
  // producing backend object.)
  if (sg_isvalid()) {
    error =
        "the producing sokol_gfx instance was set up by something other "
        "than the Metal backend.";
    return false;
  }
  error = "the Metal backend's drawing half is not built yet";
  return false;
}

bool MetalBackend::createGenerationSurfaces(
    GenerationId gen,
    int width,
    int height,
    std::string &error) {
  // Task 16c. Unreachable until then: makeCurrentOnWorker() refuses.
  (void)gen;
  (void)width;
  (void)height;
  error = "the Metal backend's drawing half is not built yet";
  return false;
}

void MetalBackend::destroyGenerationSurfaces(GenerationId gen) {
  // Task 16c. Nothing was created, so nothing is destroyed.
  (void)gen;
}

bool MetalBackend::frameTarget(SurfaceRef ref, hnui_target *out) {
  // Task 16c.
  (void)ref;
  (void)out;
  return false;
}

bool MetalBackend::generationSize(GenerationId gen, int *width, int *height) {
  // Task 16c.
  (void)gen;
  (void)width;
  (void)height;
  return false;
}

void MetalBackend::submitAndTrackCompletion(
    SurfaceRef ref,
    void (*onComplete)(void *user, SurfaceRef ref, bool ok),
    void *user) {
  // Task 16c: a completion handler on Sokol's frame command buffer
  // (hnui_sg_mtl_command_buffer()) before sg_commit(). Unreachable until
  // then; if it were reached, completion could not be established, which
  // is what ok=false says and what the shared caller treats as terminal.
  if (onComplete != nullptr)
    onComplete(user, ref, false);
}

void MetalBackend::setConsumerCompletion(
    void (*onComplete)(void *user, SurfaceRef ref, bool ok),
    void *user) {
  consumerComplete_ = onComplete;
  consumerUser_ = user;
}

bool MetalBackend::wrapForPresenting(GenerationId gen, std::string &error) {
  // Task 16c: sgp_make_image with mtl_textures[0].
  (void)gen;
  error = "the Metal backend's presenting wrappers are not built yet";
  return false;
}

bool MetalBackend::acquireFrameSwapchain() {
  return presenter_.acquireSwapchain();
}

void MetalBackend::presentClear() {
  presenter_.clear();
}

void MetalBackend::blit(SurfaceRef ref) {
  // Task 16c. Nothing is drawn, so no completion will ever arrive for the
  // read reference the caller took: release it here, ok=true because
  // nothing samples the surface -- the same answer GL gives for a blit it
  // could not draw. Unreachable until then: nothing is ever published.
  if (consumerComplete_ != nullptr)
    consumerComplete_(consumerUser_, ref, true);
}

void MetalBackend::releaseWrappers(GenerationId gen) {
  // Task 16c. No wrapper is ever created before then.
  (void)gen;
}

void MetalBackend::presentInvalid() {
  // Submits nothing and commits nothing, which is the whole of it in 16b.
  // Task 16d traces each transition into and out of the invalid state.
}

void MetalBackend::pollCompletions() {
  // Empty, and that is the whole body on Metal: completion arrives on
  // handlers Metal runs, not on anything polled here.
}

bool MetalBackend::readPresentedPixels(int width, int height, uint8_t *out) {
  // Task 16c: the drawable copied into a staging buffer in the blit's own
  // command buffer, BGRA swapped to RGBA.
  (void)width;
  (void)height;
  (void)out;
  return false;
}

void MetalBackend::workerLoopTurn() {
  // Task 16c: drain and replace the worker's autorelease pool, then run
  // maintenance if due. Never called until then: the shared code starts
  // the loop-turn check only after makeCurrentOnWorker() succeeds.
}

} // namespace

CompositorBackend::~CompositorBackend() = default;

CompositorBackend *createCompositorBackend() {
  return new MetalBackend();
}

const char *compositorBackendName() {
  return "metal";
}

} // namespace node_compat
} // namespace hermes
