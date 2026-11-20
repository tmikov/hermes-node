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
/// # What exists after Task 16d
///
/// The route refusals, the device, both instances, the surfaces, completion
/// through Metal's handlers on both instances' frame command buffers, the
/// blit and the test hooks' read-back, the worker's per-turn autorelease
/// pool, the producer's maintenance and id-pool guard, and the traces of
/// the swapchain going invalid and valid again.

#include "backend.h"

#include "metal_present.h"
#include "sokol_config.h"
#include "sokol_gfx_ext.h"
#include "sokol_log.h"
#include "test_hooks.h"
#include "ui_trace.h"

// The producing instance's declarations: no rename. Included here, as in
// gl_backend.cpp, for the ABI checks below and for the drawing half.
#include <sokol_gfx.h>

#import <Metal/Metal.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>

// The runtime's pool entry points, which ARC leaves callable: @autoreleasepool
// cannot express a pool that is pushed in one call and popped in a later one,
// which is what the worker's per-turn pool is (probe P2 confirmed both link
// and drain from an ARC unit).
extern "C" void *objc_autoreleasePoolPush(void);
extern "C" void objc_autoreleasePoolPop(void *pool);

/// A sentinel put into the worker's autorelease pool once, so that
/// workerLoopTurn() can read back that its pop really drained the pool:
/// dealloc runs only when the pool holding it is popped.
@interface HnuiPoolProbe : NSObject
- (instancetype)initWithFlag:(bool *)flag;
@end

@implementation HnuiPoolProbe {
  bool *flag_;
}
- (instancetype)initWithFlag:(bool *)flag {
  if ((self = [super init]))
    flag_ = flag;
  return self;
}
- (void)dealloc {
  *flag_ = true;
}
@end

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

/// createGenerationSurfaces() refuses below this many free id-pool slots
/// (Ruling 82): a generation's destruction queues at most nine releases
/// against a pool of 2,688, so this is far from any steady state and well
/// before Sokol's own assert.
constexpr int kIdPoolGuard = 256;

/// The most empty commits one maintenance pass issues. Four suffice from
/// an idle instance; eight leaves room without making a stuck release spin.
constexpr int kMaxMaintenanceCommits = 8;

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
  /// The Metal device, held strongly. selectContextRoute() sets it from
  /// MTLCreateSystemDefaultDevice() only to refuse a machine with none
  /// before AppKit is touched; createSharedContext() then replaces it with
  /// Sokol's, the one the layer and the presenting instance use and so the
  /// one the producer must share textures on. Two calls returning the same
  /// object is not something Metal documents, so it is adopted, not
  /// compared.
  ///
  /// Both writes are on the platform thread, before init_cb posts
  /// onSharedContextReady; the worker (the JS thread) reads it only from
  /// that posted call onward, so the dispatcher's mutex orders the read
  /// after the last write (metal_present.h, Threading).
  id<MTLDevice> device_ = nil;
  MetalPresenter presenter_;

  /// One generation's surfaces, as both halves of this backend see them.
  /// The textures are the crossing point: Sokol allocates them on the
  /// producing instance, this keeps a strong reference, and the presenting
  /// instance wraps those same objects (MetalPresenter::wrap).
  struct GenerationSurfaces {
    id<MTLTexture> textures[kSlotsPerGeneration];
    sg_image images[kSlotsPerGeneration] = {};
    sg_view colorViews[kSlotsPerGeneration] = {};
    sg_view resolveViews[kSlotsPerGeneration] = {};
    int width = 0;
    int height = 0;
  };

  /// Written by the worker, read by the platform thread
  /// (wrapForPresenting, generationSize) -- for the reason GlBackend's
  /// identical map gives, under an actual lock.
  mutable std::mutex surfacesMutex_;
  std::map<uint32_t, GenerationSurfaces> surfaces_;

  /// The worker's per-turn autorelease pool: pushed by makeCurrentOnWorker(),
  /// popped and pushed again by every workerLoopTurn(). Worker only.
  void *workerPool_ = nullptr;
  /// The first-drain read-back: whether the probe was placed, whether a pop
  /// freed it, and whether that was traced. Worker only.
  bool poolProbePlaced_ = false;
  bool poolProbeFreed_ = false;
  bool tracedFirstDrain_ = false;

  /// Producer frames committed whose handler has not finished. Incremented
  /// on the worker, decremented at the end of the handler on a Metal
  /// thread. Maintenance runs only when it is zero (workerLoopTurn() says
  /// why).
  std::atomic<int> producerInFlight_{0};

  /// Set by destroyGenerationSurfaces(), cleared by maintenance once
  /// nothing is pending. Worker only.
  bool maintenancePending_ = false;

  /// Whether the last frame found the swapchain unusable, so that each
  /// transition is traced once. Platform thread only.
  bool swapchainInvalid_ = false;

  /// Once per run each, set from whichever thread ran the path.
  std::atomic<bool> tracedProducerHandler_{false};
  std::atomic<bool> tracedInlineCompletion_{false};

  /// The consumer sink. Written on the worker, in onSharedContextReady
  /// (ui_module.cpp), before that function's release-store of the
  /// present-ready flag; read on the platform thread, in blit(), which
  /// onFrame reaches only after the acquire-load of that flag sees it set.
  /// That pair orders the read after the write; nothing writes it again.
  void (*consumerComplete_)(void *user, SurfaceRef ref, bool ok) = nullptr;
  void *consumerUser_ = nullptr;
};

/// Sokol's logger for the PRODUCING instance: the same body and instance
/// name as gl_backend.cpp's, so a message says which instance produced it.
/// A panic terminates (uiSokolLog): this instance's panics are raised on
/// the JS thread, from inside an addon's sg_* call, and Sokol's code after
/// one assumes the logger did not return.
void producerLog(
    const char *tag,
    uint32_t logLevel,
    uint32_t logItemId,
    const char *message,
    uint32_t lineNr,
    const char *filename,
    void *userData) {
  (void)tag;
  (void)userData;
  uiSokolLog(
      "sokol-produce",
      logLevel,
      logItemId,
      message,
      lineNr,
      filename ? filename : "sokol_gfx.h",
      "hermes-node-ui: the producing sokol_gfx instance -- the one addons "
      "draw with -- panicked. This is fatal: Sokol's code after a panic "
      "assumes it does not return.");
}

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
  if (!presenter_.setup(error))
    return false;
  id<MTLDevice> sokolDevice = (__bridge id<MTLDevice>)presenter_.device();
  if (sokolDevice != device_) {
    @autoreleasepool {
      uiTrace(
          "route: metal: adopting sokol's device (%s)",
          [sokolDevice.name UTF8String]);
    }
  }
  device_ = sokolDevice;
  // So a presented frame can be read back (readPresentedPixels). Test hooks
  // only: a layer that is not framebufferOnly may cost the compositor a
  // copy, and nothing outside a test reads the drawable.
  if (uiTestHooksEnabled())
    presenter_.enableCapture();
  return true;
}

bool MetalBackend::makeCurrentOnWorker(std::string &error) {
  // Nothing to bind on Metal: the device is shared. The worker's first
  // autorelease pool comes first, so that sg_setup()'s own autoreleased
  // objects land in it; workerLoopTurn() drains and replaces it.
  workerPool_ = objc_autoreleasePoolPush();
  uiTrace("worker autorelease pool: installed");

  // The precondition of the sg_setup() below, which is this method's alone:
  // a producing instance already live here was set up by something else,
  // with defaults this backend did not choose.
  if (sg_isvalid()) {
    error =
        "the producing sokol_gfx instance was set up by something other "
        "than the Metal backend.";
    objc_autoreleasePoolPop(workerPool_);
    workerPool_ = nullptr;
    return false;
  }

  sg_desc desc = {};
  desc.environment.metal.device = (__bridge const void *)device_;
  // The same defaults GL passes, for the reason gl_backend.cpp gives: an
  // addon's pipeline inherits them, and the surfaces are allocated in the
  // swapchain's colour format.
  desc.environment.defaults.color_format =
      static_cast<sg_pixel_format>(presenter_.colorFormat());
  desc.environment.defaults.depth_format = SG_PIXELFORMAT_NONE;
  desc.environment.defaults.sample_count = 1;
  desc.logger.func = producerLog;
  sg_setup(&desc);
  if (!sg_isvalid()) {
    error =
        "the producing sokol_gfx instance could not be set up on the Metal "
        "device.";
    objc_autoreleasePoolPop(workerPool_);
    workerPool_ = nullptr;
    return false;
  }
  uiTrace(
      "producing instance ready: color_format %d", presenter_.colorFormat());
  // Not a selection: Metal has one mechanism, and the evidence that it ran
  // is the "handler ran" line traced from inside the first handler.
  uiTrace("completion mode: handler (metal)");
  return true;
}

bool MetalBackend::createGenerationSurfaces(
    GenerationId gen,
    int width,
    int height,
    std::string &error) {
  // The plan's Ruling 82. A generation change is not held back until the
  // previous one's releases are collected; what is enforced is the bound
  // that rule exists for. A generation's destruction queues at most nine
  // releases against Sokol's pool, every idle loop turn collects them, and
  // this replaces the assert Sokol would hit on an empty pool (compiled out
  // in Release) with a sentence the shared code turns into a terminated
  // session.
  const int freeSlots = hnui_sg_mtl_idpool_free();
  if (freeSlots < kIdPoolGuard) {
    error = "the Metal resource pool is nearly exhausted (" +
        std::to_string(freeSlots) +
        " slots free): surface generations are being replaced faster than "
        "their GPU resources can be reclaimed.";
    return false;
  }
  @autoreleasepool {
    GenerationSurfaces surfaces;
    surfaces.width = width;
    surfaces.height = height;
    for (int i = 0; i < kSlotsPerGeneration; ++i) {
      sg_image_desc imageDesc = {};
      imageDesc.type = SG_IMAGETYPE_2D;
      imageDesc.width = width;
      imageDesc.height = height;
      imageDesc.num_mipmaps = 1;
      imageDesc.pixel_format =
          static_cast<sg_pixel_format>(presenter_.colorFormat());
      imageDesc.sample_count = 1;
      // Both usages, per the spec's Surfaces, as gl_backend.cpp: an MSAA
      // addon resolves into the surface. Sokol allocates the texture.
      imageDesc.usage.color_attachment = true;
      imageDesc.usage.resolve_attachment = true;
      imageDesc.label = "hnui-surface";
      surfaces.images[i] = sg_make_image(&imageDesc);
      if (sg_query_image_state(surfaces.images[i]) != SG_RESOURCESTATE_VALID) {
        error = "a surface image could not be created for a generation.";
        return false;
      }
      // The texture Sokol made, read back so the presenting instance can
      // wrap the same object (spike 2). An immutable image has one slot.
      surfaces.textures[i] =
          (__bridge id<MTLTexture>)sg_mtl_query_image_info(surfaces.images[i])
              .tex[0];
      if (surfaces.textures[i] == nil) {
        error = "a surface image has no Metal texture behind it.";
        return false;
      }

      sg_view_desc colorDesc = {};
      colorDesc.color_attachment.image = surfaces.images[i];
      colorDesc.label = "hnui-surface-color";
      surfaces.colorViews[i] = sg_make_view(&colorDesc);
      if (sg_query_view_state(surfaces.colorViews[i]) !=
          SG_RESOURCESTATE_VALID) {
        error = "a surface colour-attachment view could not be created.";
        return false;
      }

      sg_view_desc resolveDesc = {};
      resolveDesc.resolve_attachment.image = surfaces.images[i];
      resolveDesc.label = "hnui-surface-resolve";
      surfaces.resolveViews[i] = sg_make_view(&resolveDesc);
      if (sg_query_view_state(surfaces.resolveViews[i]) !=
          SG_RESOURCESTATE_VALID) {
        error = "a surface resolve-attachment view could not be created.";
        return false;
      }
    }

    {
      std::lock_guard<std::mutex> lock(surfacesMutex_);
      surfaces_[gen.value] = surfaces;
    }
  }
  uiTrace(
      "generation %u: %d surfaces at %dx%d",
      gen.value,
      kSlotsPerGeneration,
      width,
      height);
  return true;
}

void MetalBackend::destroyGenerationSurfaces(GenerationId gen) {
  @autoreleasepool {
    GenerationSurfaces surfaces;
    {
      std::lock_guard<std::mutex> lock(surfacesMutex_);
      auto it = surfaces_.find(gen.value);
      if (it == surfaces_.end())
        return;
      surfaces = it->second;
      surfaces_.erase(it);
    }
    // Sokol defers each Metal release to a later sg_commit(): each is due
    // up to four commits on. Maintenance (workerLoopTurn) issues those
    // commits when nothing else would -- an idle program commits nothing.
    maintenancePending_ = true;
    for (int i = 0; i < kSlotsPerGeneration; ++i) {
      sg_destroy_view(surfaces.colorViews[i]);
      sg_destroy_view(surfaces.resolveViews[i]);
      sg_destroy_image(surfaces.images[i]);
    }
  }
  uiTrace("generation %u: surfaces destroyed", gen.value);
}

bool MetalBackend::frameTarget(SurfaceRef ref, hnui_target *out) {
  std::lock_guard<std::mutex> lock(surfacesMutex_);
  auto it = surfaces_.find(ref.gen.value);
  // A stale SurfaceRef from a destroyed generation is refused, as on GL.
  if (it == surfaces_.end() || ref.slot >= kSlotsPerGeneration)
    return false;
  out->color_view.id = it->second.colorViews[ref.slot].id;
  out->resolve_view.id = it->second.resolveViews[ref.slot].id;
  out->width = it->second.width;
  out->height = it->second.height;
  out->color_format = presenter_.colorFormat();
  out->sample_count = 1;
  out->generation = ref.gen.value;
  return true;
}

bool MetalBackend::generationSize(GenerationId gen, int *width, int *height) {
  std::lock_guard<std::mutex> lock(surfacesMutex_);
  auto it = surfaces_.find(gen.value);
  if (it == surfaces_.end())
    return false;
  *width = it->second.width;
  *height = it->second.height;
  return true;
}

void MetalBackend::submitAndTrackCompletion(
    SurfaceRef ref,
    void (*onComplete)(void *user, SurfaceRef ref, bool ok),
    void *user) {
  @autoreleasepool {
    // The producing instance's frame command buffer, read BEFORE the
    // commit: sg_commit() commits it and forgets it.
    id<MTLCommandBuffer> commandBuffer =
        (__bridge id<MTLCommandBuffer>)hnui_sg_mtl_command_buffer();
    if (commandBuffer == nil) {
      // The frame began no pass, so Sokol has no buffer to commit
      // (_sg_mtl_commit commits only if it has one), and nothing will
      // complete. Nothing was drawn either -- by the addon contract, not by
      // a check: an addon never calls sg_commit(), so no earlier buffer of
      // this frame can have been committed behind our back. So this is
      // complete now, which is what GL does for such a frame, whose fence
      // signals at once.
      sg_commit();
      if (!tracedInlineCompletion_.exchange(true))
        uiTrace("completion: frame had no command buffer; completed inline");
      onComplete(user, ref, true);
      return;
    }

    // Counted before the handler exists, so it can never be seen to go
    // negative; the handler decrements it as its last act.
    producerInFlight_.fetch_add(1);
    MetalBackend *self = this;
    // Added before sg_commit(): a handler added to a committed buffer is a
    // Metal error. The block captures only the function pointer, user, ref
    // and this.
    [commandBuffer addCompletedHandler:^(id<MTLCommandBuffer> buffer) {
      const MTLCommandBufferStatus status = buffer.status;
      const bool ok = status == MTLCommandBufferStatusCompleted;
      if (status == MTLCommandBufferStatusError) {
        @autoreleasepool {
          uiStderrLine(
              "hermes-node-ui: a frame the producer committed failed on the "
              "GPU: %s",
              [[buffer.error localizedDescription] UTF8String]);
        }
      }
      if (!self->tracedProducerHandler_.exchange(true)) {
        uiTrace(
            "completion: producer handler ran (status=%s)",
            ok ? "completed" : "error");
      }
      onComplete(user, ref, ok);
      self->producerInFlight_.fetch_sub(1);
    }];
    sg_commit();
  }
}

void MetalBackend::setConsumerCompletion(
    void (*onComplete)(void *user, SurfaceRef ref, bool ok),
    void *user) {
  consumerComplete_ = onComplete;
  consumerUser_ = user;
}

bool MetalBackend::wrapForPresenting(GenerationId gen, std::string &error) {
  // Strong copies, taken under the lock, keep the textures alive through the
  // call even if the worker destroys the generation meanwhile.
  id<MTLTexture> textures[kSlotsPerGeneration];
  void *pointers[kSlotsPerGeneration] = {};
  int width = 0;
  int height = 0;
  {
    std::lock_guard<std::mutex> lock(surfacesMutex_);
    auto it = surfaces_.find(gen.value);
    if (it == surfaces_.end()) {
      error = "no surfaces exist for that generation.";
      return false;
    }
    for (int i = 0; i < kSlotsPerGeneration; ++i)
      textures[i] = it->second.textures[i];
    width = it->second.width;
    height = it->second.height;
  }
  for (int i = 0; i < kSlotsPerGeneration; ++i)
    pointers[i] = (__bridge void *)textures[i];
  return presenter_.wrap(
      gen, pointers, kSlotsPerGeneration, width, height, error);
}

bool MetalBackend::acquireFrameSwapchain() {
  const bool valid = presenter_.acquireSwapchain();
  // Traced here, on the transition and after the acquisition succeeded, so
  // the line is evidence that a usable drawable came back.
  if (valid && swapchainInvalid_) {
    swapchainInvalid_ = false;
    uiTrace("present: swapchain valid again");
  }
  return valid;
}

void MetalBackend::presentClear() {
  presenter_.clear();
}

void MetalBackend::blit(SurfaceRef ref) {
  if (!presenter_.blit(ref, consumerComplete_, consumerUser_)) {
    // Nothing was submitted, so no completion will ever arrive for the read
    // reference the caller took: release it here. ok=true because nothing
    // samples the surface -- GL's answer for a blit it could not draw.
    if (consumerComplete_ != nullptr)
      consumerComplete_(consumerUser_, ref, true);
  }
}

void MetalBackend::releaseWrappers(GenerationId gen) {
  presenter_.releaseWrappers(gen);
}

void MetalBackend::presentInvalid() {
  // Submits nothing and commits nothing: onFrame calls this only after
  // acquireFrameSwapchain() returned false -- Sokol's obscured timer is
  // running (a miniaturized window, a locked screen), or there is no
  // drawable. No read reference exists to release; the shared onFrame
  // takes one only on the blit path. Traced on the transition only.
  if (!swapchainInvalid_) {
    swapchainInvalid_ = true;
    uiTrace("present: swapchain invalid; nothing submitted");
  }
}

void MetalBackend::pollCompletions() {
  // Empty, and that is the whole body on Metal: completion arrives on
  // handlers Metal runs, not on anything polled here.
}

bool MetalBackend::readPresentedPixels(int width, int height, uint8_t *out) {
  return presenter_.readPixels(width, height, out);
}

void MetalBackend::workerLoopTurn() {
  // The caller guarantees no frame callback is on the stack and the window
  // is not CLOSING (backend.h), which is what makes the pop safe: nothing
  // live is in the pool being drained.
  if (workerPool_ == nullptr)
    return;
  if (!poolProbePlaced_) {
    // CFBridgingRetain takes the object out of ARC's hands at +1, and
    // CFAutorelease hands that reference to the pool: the pool now holds
    // the only one. Spelled through CF on purpose -- an __autoreleasing
    // local that is never used is something ARC's optimizer may turn into
    // an immediate release, which would free the probe with no pop at all.
    poolProbePlaced_ = true;
    CFAutorelease(CFBridgingRetain(
        [[HnuiPoolProbe alloc] initWithFlag:&poolProbeFreed_]));
    if (poolProbeFreed_) {
      // Freed before any pop: the read-back below would prove nothing, so
      // it is never printed, and this says why.
      uiTrace("worker autorelease pool: probe freed before the pop");
      poolProbeFreed_ = false;
      tracedFirstDrain_ = true;
    }
  }
  objc_autoreleasePoolPop(workerPool_);
  workerPool_ = objc_autoreleasePoolPush();
  // Read back, not assumed: printed only once a pop has freed the probe.
  if (poolProbeFreed_ && !tracedFirstDrain_) {
    tracedFirstDrain_ = true;
    uiTrace("worker autorelease pool: first drain");
  }

  // Maintenance (the plan's Design (a)). Sokol collects a destroyed
  // resource's release only inside sg_commit(), once the frame index has
  // passed the one it is due at, so an idle program -- one that draws
  // nothing after a resize -- would never get its generation's textures
  // back. So: commit until nothing is pending, at most eight times. From an
  // idle instance that is four commits, because _sg_mtl_commit collects
  // with the frame index before sg_commit increments it.
  //
  // **Why producerInFlight_ == 0 is a condition, and why no test fails
  // without it** (Review focus 2). Sokol's command buffers hold unretained
  // references, and an empty commit takes no semaphore: it only advances
  // the release clock. Committing past a frame whose GPU work is still in
  // flight could free a resource an addon destroyed in that frame while
  // the GPU still reads it -- a use-after-free on the GPU, which no
  // assertion sees and which shows, if at all, as a wrong pixel once in a
  // great many runs. The counter is "no producer GPU work outstanding and
  // no completion handler pending"; the loop-turn guard in the caller is
  // "no open pass". Nothing in the suite can observe the hazard, so this
  // comment and the verification's runs under GPU validation are what
  // stand for it.
  if (maintenancePending_ && producerInFlight_.load() == 0) {
    const int freeBefore = hnui_sg_mtl_idpool_free();
    int commits = 0;
    while (hnui_sg_mtl_idpool_pending() > 0 &&
           commits < kMaxMaintenanceCommits) {
      sg_commit();
      ++commits;
    }
    // Read back, both counts, after the commits.
    const int pending = hnui_sg_mtl_idpool_pending();
    if (pending == 0)
      maintenancePending_ = false;
    uiTrace(
        "maintenance: %d commit(s), idpool free %d -> %d, pending %d",
        commits,
        freeBefore,
        hnui_sg_mtl_idpool_free(),
        pending);
  }
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
