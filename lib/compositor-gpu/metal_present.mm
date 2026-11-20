/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "metal_present.h"

#include "present_constants.h"
#include "sokol_config.h"
#include "sokol_gfx_ext.h"
#include "sokol_log.h"
#include "ui_trace.h"

// THE RENAME COMES FIRST, before sokol_gfx.h's declarations -- see
// sokol_gfx_present_impl.cpp. Every sg_* call written in this file
// therefore compiles to the sgp_* instance, the one the platform thread
// owns, and sgp_rename.h #errors if sokol_gfx.h got here first.
#include "sgp_rename.h"

#include <sokol_gfx.h>

// Declarations only; sokol_app's implementation is compiled once, in
// tools/hermes-node-ui/sokol_app_impl.cpp.
#include <sokol_app.h>

// The one unit that compiles sokol_glue's implementation, as gl_present.cpp
// is on Linux (the two are never built together). sglue_* only build sg_*
// structs, which the rename leaves alone.
#define SOKOL_GLUE_IMPL
#include <sokol_glue.h>

#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <map>
#include <mutex>

namespace hermes {
namespace node_compat {

namespace {

/// Sokol's logger for the PRESENTING instance; the same body as
/// gl_present.cpp's, so a message says which instance produced it.
void presentLog(
    const char *tag,
    uint32_t logLevel,
    uint32_t logItemId,
    const char *message,
    uint32_t lineNr,
    const char *filename,
    void *userData) {
  (void)tag;
  (void)userData;
  // A panic terminates (uiSokolLog), as on the producing instance.
  uiSokolLog(
      "sokol-present",
      logLevel,
      logItemId,
      message,
      lineNr,
      filename ? filename : "sokol_gfx.h",
      "hermes-node-ui: the presenting sokol_gfx instance panicked. This is "
      "fatal: Sokol's code after a panic assumes it does not return.");
}

/// The blit shader, in MSL: the same full-screen triangle as
/// gl_present.cpp's GLSL, with no vertex buffer -- positions come from the
/// vertex id, `p` taking (0,0), (2,0), (0,2), so the clip-space triangle
/// (-1,-1), (3,-1), (-1,3) covers the viewport and `uv` runs 0..1 across
/// exactly the visible part.
///
/// **The one difference from the GLSL is `uv.y = 1 - p.y`.** A Metal
/// texture's row 0 is its top row while clip-space y points up, so the
/// fragment that lands at the top of the viewport (p.y = 1) must sample row
/// 0. GL's texture origin is the bottom row, which is why its shader uses
/// `p` unchanged. test/ui-present.js's quadrant pattern is asymmetric top
/// to bottom, so a wrong flip is a red test (Task 16c), not a convention.
///
/// Nothing draws with this until 16c; it is compiled now so that MSL
/// compilation through Sokol is part of the toolchain Task 16b proves.
const char *const kBlitShaderSource = R"(#include <metal_stdlib>
using namespace metal;
struct vs_out {
  float4 pos [[position]];
  float2 uv;
};
vertex vs_out vs_main(uint vid [[vertex_id]]) {
  float2 p = float2(float((vid << 1) & 2), float(vid & 2));
  vs_out out;
  out.pos = float4(p * 2.0 - 1.0, 0.0, 1.0);
  out.uv = float2(p.x, 1.0 - p.y);
  return out;
}
fragment float4 fs_main(vs_out in [[stage_in]],
                        texture2d<float> tex [[texture(0)]],
                        sampler smp [[sampler(0)]]) {
  return tex.sample(smp, in.uv);
}
)";

} // namespace

//===========================================================================
// State
//===========================================================================

/// One generation's wrappers on this instance.
struct Wrapped {
  sg_image images[kSlotsPerGeneration]{};
  sg_view views[kSlotsPerGeneration]{};
  int width = 0;
  int height = 0;
};

struct MetalPresenter::State {
  /// The device the presenting instance was set up on: Sokol's, from
  /// sokol_app's environment. Held strongly.
  id<MTLDevice> device = nil;
  /// Keyed by GenerationId::value, like SurfaceTable's own map.
  std::map<uint32_t, Wrapped> wrapped;

  /// The test hooks' read-back (enableCapture(), blit(), readPixels()).
  bool captureEnabled = false;
  /// The staging buffer the drawable is copied into. Reallocated when the
  /// drawable's size changes; the old one is kept alive by the blit that
  /// last used it (see blit()).
  id<MTLBuffer> staging = nil;
  int stagingWidth = 0;
  int stagingHeight = 0;
  /// The last blit's command buffer and what its copy describes, held
  /// strongly so readPixels() can wait on it.
  id<MTLCommandBuffer> keptBuffer = nil;
  id<MTLBuffer> keptStaging = nil;
  int keptWidth = 0;
  int keptHeight = 0;
  MTLPixelFormat keptFormat = MTLPixelFormatInvalid;
  /// Which blit the kept buffer is, and the last blit whose consumer
  /// handler has finished: readPixels() waits for the second to reach the
  /// first, so everything the handler does -- its trace and the read
  /// reference it releases -- has happened before a capture is reported.
  /// waitUntilCompleted alone does not order the handler (it runs on a
  /// Metal thread, and Metal promises nothing about which returns first).
  uint64_t blitSerial = 0;
  uint64_t keptSerial = 0;
  std::mutex handledMutex;
  std::condition_variable handledCv;
  uint64_t handledSerial = 0;

  /// Set by the first consumer handler, on a Metal thread.
  std::atomic<bool> tracedConsumerHandler{false};
  sg_shader shader{};
  sg_pipeline pipeline{};
  sg_sampler sampler{};
  sg_swapchain swapchain{};
  bool swapchainValid = false;
  /// Each of the two traces below is printed once, the first time its
  /// event happens.
  bool tracedDrawable = false;
  bool tracedClear = false;
};

MetalPresenter::~MetalPresenter() {
  // Deliberately does NOT call sg_shutdown(), for the reason GlPresenter's
  // destructor records: in hermes-node-ui this is never reached -- the
  // backend is never deleted, sapp_run() never returns, and every exit path
  // ends in _exit() on the JS thread.
  delete state_;
}

//===========================================================================
// Setup
//===========================================================================

bool MetalPresenter::setup(std::string &error) {
  sg_desc desc = {};
  desc.environment = sglue_environment();
  if (desc.environment.metal.device == nullptr) {
    error =
        "sokol_app reported no Metal device, so there is nothing to set the "
        "presenting sokol_gfx instance up on.";
    return false;
  }
  desc.logger.func = presentLog;
  sg_setup(&desc);
  if (!sg_isvalid()) {
    error =
        "the presenting sokol_gfx instance could not be set up against "
        "sokol_app's Metal device.";
    return false;
  }

  // **Only now**, with a live instance. A refused setup must leave state_
  // null: _sapp_frame calls frame_cb straight after init_cb, and
  // acquireSwapchain()/clear() take a non-null state_ to mean that sgp_*
  // calls are safe -- a state_ left behind by an early return would carry
  // the first frame into sg_begin_pass on an instance never set up.
  state_ = new State();
  state_->device = (__bridge id<MTLDevice>)desc.environment.metal.device;

  // Recorded for the producer, which must allocate its surfaces in the
  // swapchain's colour format and cannot ask sokol_app itself.
  colorFormat_ = static_cast<int>(desc.environment.defaults.color_format);
  sampleCount_ = desc.environment.defaults.sample_count;

  sg_shader_desc shaderDesc = {};
  shaderDesc.vertex_func.source = kBlitShaderSource;
  shaderDesc.vertex_func.entry = "vs_main";
  shaderDesc.fragment_func.source = kBlitShaderSource;
  shaderDesc.fragment_func.entry = "fs_main";
  shaderDesc.views[0].texture.stage = SG_SHADERSTAGE_FRAGMENT;
  shaderDesc.views[0].texture.image_type = SG_IMAGETYPE_2D;
  shaderDesc.views[0].texture.sample_type = SG_IMAGESAMPLETYPE_FLOAT;
  // The [[texture(0)]] and [[sampler(0)]] in the source above: on Metal the
  // binding is by slot number, where GL 4.1's is by name (glsl_name).
  shaderDesc.views[0].texture.msl_texture_n = 0;
  shaderDesc.samplers[0].stage = SG_SHADERSTAGE_FRAGMENT;
  shaderDesc.samplers[0].sampler_type = SG_SAMPLERTYPE_FILTERING;
  shaderDesc.samplers[0].msl_sampler_n = 0;
  shaderDesc.texture_sampler_pairs[0].stage = SG_SHADERSTAGE_FRAGMENT;
  shaderDesc.texture_sampler_pairs[0].view_slot = 0;
  shaderDesc.texture_sampler_pairs[0].sampler_slot = 0;
  shaderDesc.label = "hnui-blit-shader";
  state_->shader = sg_make_shader(&shaderDesc);
  if (sg_query_shader_state(state_->shader) != SG_RESOURCESTATE_VALID) {
    error = "the presenting blit shader failed to compile.";
    return false;
  }

  sg_pipeline_desc pipelineDesc = {};
  pipelineDesc.shader = state_->shader;
  pipelineDesc.primitive_type = SG_PRIMITIVETYPE_TRIANGLES;
  pipelineDesc.cull_mode = SG_CULLMODE_NONE;
  // Formats and sample count are left to Sokol's defaulting from the
  // environment, i.e. from the swapchain, as gl_present.cpp leaves them.
  pipelineDesc.label = "hnui-blit-pipeline";
  state_->pipeline = sg_make_pipeline(&pipelineDesc);
  if (sg_query_pipeline_state(state_->pipeline) != SG_RESOURCESTATE_VALID) {
    error = "the presenting blit pipeline could not be created.";
    return false;
  }

  // Linear and clamped, for the reasons gl_present.cpp gives: stretching
  // during a resize, an exact copy at 1:1, and no wrapped samples from the
  // off-screen part of the triangle.
  sg_sampler_desc samplerDesc = {};
  samplerDesc.min_filter = SG_FILTER_LINEAR;
  samplerDesc.mag_filter = SG_FILTER_LINEAR;
  samplerDesc.wrap_u = SG_WRAP_CLAMP_TO_EDGE;
  samplerDesc.wrap_v = SG_WRAP_CLAMP_TO_EDGE;
  samplerDesc.label = "hnui-blit-sampler";
  state_->sampler = sg_make_sampler(&samplerDesc);
  if (sg_query_sampler_state(state_->sampler) != SG_RESOURCESTATE_VALID) {
    error = "the presenting blit sampler could not be created.";
    return false;
  }

  uiTrace(
      "presenting instance ready: color_format %d, sample_count %d",
      colorFormat_,
      sampleCount_);
  return true;
}

//===========================================================================
// Per frame
//===========================================================================

void *MetalPresenter::device() const {
  return state_ != nullptr ? (__bridge void *)state_->device : nullptr;
}

bool MetalPresenter::acquireSwapchain() {
  if (state_ == nullptr)
    return false;
  // Exactly once per frame callback (`sokol_app.h:1935`): on Metal this is
  // what takes the next drawable from the layer.
  state_->swapchain = sglue_swapchain();
  id<CAMetalDrawable> drawable =
      (__bridge id<CAMetalDrawable>)state_->swapchain.metal.current_drawable;
  state_->swapchainValid = !state_->swapchain.invalid && drawable != nil;
  if (!state_->swapchainValid)
    return false;

  id<MTLTexture> texture = drawable.texture;
  swapchainWidth_ = static_cast<int>(texture.width);
  swapchainHeight_ = static_cast<int>(texture.height);
  if (!state_->tracedDrawable) {
    state_->tracedDrawable = true;
    uiTrace(
        "present: drawable acquired (%dx%d)",
        swapchainWidth_,
        swapchainHeight_);
  }
  return true;
}

void MetalPresenter::clear() {
  if (state_ == nullptr || !state_->swapchainValid)
    return;
  sg_pass pass = {};
  pass.action.colors[0].load_action = SG_LOADACTION_CLEAR;
  pass.action.colors[0].store_action = SG_STOREACTION_STORE;
  pass.action.colors[0].clear_value = sg_color{
      kPresenterClearLevel, kPresenterClearLevel, kPresenterClearLevel, 1.0f};
  pass.swapchain = state_->swapchain;
  sg_begin_pass(&pass);
  sg_end_pass();
  // The frame's command buffer, held across the commit so the trace below
  // is read back from it rather than written because the next line exists.
  id<MTLCommandBuffer> commandBuffer =
      (__bridge id<MTLCommandBuffer>)hnui_sgp_mtl_command_buffer();
  sg_commit();
  if (!state_->tracedClear) {
    state_->tracedClear = true;
    // Committed means Sokol let go of the buffer and Metal has it. Error
    // is a later status than Committed: the buffer was committed and then
    // failed on the GPU, which is a third outcome, not "not committed".
    MTLCommandBufferStatus status = commandBuffer != nil
        ? commandBuffer.status
        : MTLCommandBufferStatusNotEnqueued;
    bool committed = commandBuffer != nil &&
        hnui_sgp_mtl_command_buffer() == nullptr &&
        status >= MTLCommandBufferStatusCommitted;
    if (committed && status == MTLCommandBufferStatusError)
      uiTrace("present: first clear committed, status error");
    else if (committed)
      uiTrace("present: first clear committed");
    else
      uiTrace(
          "present: first clear NOT committed (command buffer %s, status %d)",
          commandBuffer != nil ? "present" : "absent",
          (int)status);
  }
}

//===========================================================================
// The test hooks' read-back
//===========================================================================

void MetalPresenter::enableCapture() {
  if (state_ == nullptr)
    return;
  // Sokol's layer is its view's, and the view is the window's content view
  // (_sapp_macos_mtl_init); sokol_app has no getter for the layer itself.
  NSWindow *window = (__bridge NSWindow *)sapp_macos_get_window();
  CALayer *layer = window.contentView.layer;
  if (![layer isKindOfClass:[CAMetalLayer class]]) {
    uiTrace(
        "capture: staging copy NOT enabled (the view's layer is %s, not a "
        "CAMetalLayer)",
        layer != nil ? [NSStringFromClass([layer class]) UTF8String] : "nil");
    return;
  }
  CAMetalLayer *metalLayer = (CAMetalLayer *)layer;
  // Sokol creates the layer framebufferOnly (_sapp_macos_mtl_init), and a
  // copy out of a framebufferOnly texture is an API-validation abort at
  // encode time (probe P3).
  metalLayer.framebufferOnly = NO;
  const bool framebufferOnly = metalLayer.framebufferOnly;
  state_->captureEnabled = !framebufferOnly;
  uiTrace(
      "capture: staging copy %s (framebufferOnly=%d)",
      framebufferOnly ? "NOT enabled" : "enabled",
      framebufferOnly ? 1 : 0);
}

bool MetalPresenter::readPixels(int width, int height, uint8_t *out) {
  if (state_ == nullptr || out == nullptr || !state_->captureEnabled ||
      state_->keptBuffer == nil)
    return false;
  // The size asked for must be the presented drawable's: a read-back of
  // some other size would be of something that was not presented.
  if (width != state_->keptWidth || height != state_->keptHeight)
    return false;
  // BGRA is what sokol_app gives a CAMetalLayer; anything else is refused
  // by name rather than converted by guess.
  if (state_->keptFormat != MTLPixelFormatBGRA8Unorm &&
      state_->keptFormat != MTLPixelFormatBGRA8Unorm_sRGB)
    return false;

  [state_->keptBuffer waitUntilCompleted];
  if (state_->keptBuffer.status != MTLCommandBufferStatusCompleted)
    return false;
  {
    // Bounded, because a handler that never ran is a defect to report, not
    // a reason to hang the platform thread: the capture then fails, and
    // says so.
    std::unique_lock<std::mutex> lock(state_->handledMutex);
    const uint64_t wanted = state_->keptSerial;
    if (!state_->handledCv.wait_for(lock, std::chrono::seconds(10), [&] {
          return state_->handledSerial >= wanted;
        }))
      return false;
  }

  // Rows are already top first: a Metal texture's row 0 is its top row, and
  // the copy wrote them in that order.
  const uint8_t *src =
      static_cast<const uint8_t *>([state_->keptStaging contents]);
  const size_t count = static_cast<size_t>(width) * height;
  for (size_t i = 0; i < count; ++i) {
    out[i * 4 + 0] = src[i * 4 + 2];
    out[i * 4 + 1] = src[i * 4 + 1];
    out[i * 4 + 2] = src[i * 4 + 0];
    out[i * 4 + 3] = src[i * 4 + 3];
  }
  return true;
}

//===========================================================================
// Wrappers
//===========================================================================

bool MetalPresenter::wrap(
    GenerationId gen,
    void *const *mtlTextures,
    int count,
    int width,
    int height,
    std::string &error) {
  if (state_ == nullptr) {
    error = "the presenting instance has not been set up.";
    return false;
  }
  if (state_->wrapped.count(gen.value) != 0)
    return true;
  if (count != kSlotsPerGeneration) {
    error = "a generation must have exactly kSlotsPerGeneration surfaces.";
    return false;
  }

  Wrapped wrapped;
  wrapped.width = width;
  wrapped.height = height;
  for (int i = 0; i < kSlotsPerGeneration; ++i) {
    sg_image_desc imageDesc = {};
    imageDesc.type = SG_IMAGETYPE_2D;
    imageDesc.width = width;
    imageDesc.height = height;
    imageDesc.num_mipmaps = 1;
    imageDesc.pixel_format = static_cast<sg_pixel_format>(colorFormat_);
    imageDesc.sample_count = 1;
    // Texture injection, ONE texture: the image is immutable, so it has a
    // single slot (spike 2). Sokol retains it under ARC and releases its
    // own reference when the wrapper is destroyed; the producer keeps its
    // own.
    imageDesc.mtl_textures[0] = mtlTextures[i];
    imageDesc.label = "hnui-surface-wrapper";
    wrapped.images[i] = sg_make_image(&imageDesc);
    if (sg_query_image_state(wrapped.images[i]) != SG_RESOURCESTATE_VALID) {
      error = "a presenting wrapper image could not be created.";
      return false;
    }

    sg_view_desc viewDesc = {};
    viewDesc.texture.image = wrapped.images[i];
    viewDesc.label = "hnui-surface-view";
    wrapped.views[i] = sg_make_view(&viewDesc);
    if (sg_query_view_state(wrapped.views[i]) != SG_RESOURCESTATE_VALID) {
      error = "a presenting wrapper view could not be created.";
      return false;
    }
  }

  state_->wrapped.emplace(gen.value, wrapped);
  uiTrace(
      "wrapped generation %u for presenting (%dx%d)", gen.value, width, height);
  return true;
}

void MetalPresenter::releaseWrappers(GenerationId gen) {
  if (state_ == nullptr)
    return;
  auto it = state_->wrapped.find(gen.value);
  if (it == state_->wrapped.end())
    return;
  // Sokol defers the Metal release to a later sg_commit(); this instance
  // commits a real pass every valid frame, so its clock advances on its own
  // (the plan's Maintenance, presenter half).
  for (int i = 0; i < kSlotsPerGeneration; ++i) {
    sg_destroy_view(it->second.views[i]);
    sg_destroy_image(it->second.images[i]);
  }
  state_->wrapped.erase(it);
  uiTrace("released presenting wrappers for generation %u", gen.value);
}

bool MetalPresenter::isWrapped(GenerationId gen) const {
  return state_ != nullptr && state_->wrapped.count(gen.value) != 0;
}

//===========================================================================
// The blit
//===========================================================================

bool MetalPresenter::blit(
    SurfaceRef ref,
    void (*onComplete)(void *user, SurfaceRef ref, bool ok),
    void *user) {
  if (state_ == nullptr || !state_->swapchainValid)
    return false;
  auto it = state_->wrapped.find(ref.gen.value);
  if (it == state_->wrapped.end() || ref.slot >= kSlotsPerGeneration)
    return false;

  // Taken before the pass: sg_end_pass() forgets it (it hands it to
  // presentDrawable: and nils its own pointer).
  id<CAMetalDrawable> drawable =
      (__bridge id<CAMetalDrawable>)state_->swapchain.metal.current_drawable;

  sg_pass pass = {};
  // CLEAR, as gl_present.cpp does: a blit that did not draw shows the clear
  // colour rather than undefined drawable contents.
  pass.action.colors[0].load_action = SG_LOADACTION_CLEAR;
  pass.action.colors[0].store_action = SG_STOREACTION_STORE;
  pass.action.colors[0].clear_value = sg_color{
      kPresenterClearLevel, kPresenterClearLevel, kPresenterClearLevel, 1.0f};
  pass.swapchain = state_->swapchain;
  sg_begin_pass(&pass);
  sg_apply_pipeline(state_->pipeline);
  sg_bindings bindings = {};
  bindings.views[0] = it->second.views[ref.slot];
  bindings.samplers[0] = state_->sampler;
  sg_apply_bindings(&bindings);
  sg_draw(0, 3, 1);
  sg_end_pass();

  // THIS instance's frame command buffer -- the presenting one, which is
  // what the isolation check's user-object rule holds this file to.
  id<MTLCommandBuffer> commandBuffer =
      (__bridge id<MTLCommandBuffer>)hnui_sgp_mtl_command_buffer();
  if (commandBuffer == nil) {
    // A pass always makes one, so this is not expected; if it happened,
    // nothing would complete, and false makes the caller release the read
    // reference inline.
    sg_commit();
    return false;
  }

  // The staging copy, in the same command buffer and after the pass --
  // after sg_end_pass() registered presentDrawable: -- which is the order
  // probe P3b used, and there the copy read back the pass's pixels exactly.
  id<MTLBuffer> stagingForThisBlit = nil;
  uint64_t serial = 0;
  if (state_->captureEnabled && drawable != nil) {
    id<MTLTexture> texture = drawable.texture;
    const int width = static_cast<int>(texture.width);
    const int height = static_cast<int>(texture.height);
    if (state_->staging == nil || state_->stagingWidth != width ||
        state_->stagingHeight != height) {
      state_->staging = [state_->device
          newBufferWithLength:static_cast<NSUInteger>(width) * height * 4
                      options:MTLResourceStorageModeShared];
      state_->stagingWidth = width;
      state_->stagingHeight = height;
    }
    if (state_->staging != nil) {
      id<MTLBlitCommandEncoder> encoder = [commandBuffer blitCommandEncoder];
      [encoder copyFromTexture:texture
                       sourceSlice:0
                       sourceLevel:0
                      sourceOrigin:MTLOriginMake(0, 0, 0)
                        sourceSize:MTLSizeMake(width, height, 1)
                          toBuffer:state_->staging
                 destinationOffset:0
            destinationBytesPerRow:static_cast<NSUInteger>(width) * 4
          destinationBytesPerImage:static_cast<NSUInteger>(width) * height * 4];
      [encoder endEncoding];
      stagingForThisBlit = state_->staging;
      serial = ++state_->blitSerial;
      state_->keptSerial = serial;
      state_->keptBuffer = commandBuffer;
      state_->keptStaging = state_->staging;
      state_->keptWidth = width;
      state_->keptHeight = height;
      state_->keptFormat = texture.pixelFormat;
    }
  }

  // The consumer handler, on the presenting instance's buffer and BEFORE
  // its commit -- a handler added after commit is a Metal error. Sokol's
  // command buffers hold unretained references, so the block holds the
  // staging buffer it copied into until the GPU is done with it; the
  // sampled surface is kept alive by the read reference this handler
  // releases.
  // state_ outlives every handler: the presenter is never destroyed (see
  // the destructor).
  State *state = state_;
  [commandBuffer addCompletedHandler:^(id<MTLCommandBuffer> buffer) {
    (void)stagingForThisBlit;
    const MTLCommandBufferStatus status = buffer.status;
    const bool ok = status == MTLCommandBufferStatusCompleted;
    if (status == MTLCommandBufferStatusError) {
      @autoreleasepool {
        std::fprintf(
            stderr,
            "hermes-node-ui: a presented frame failed on the GPU: %s\n",
            [[buffer.error localizedDescription] UTF8String]);
      }
    }
    if (!state->tracedConsumerHandler.exchange(true)) {
      uiTrace(
          "completion: consumer handler ran (status=%s)",
          ok ? "completed" : "error");
    }
    if (onComplete != nullptr)
      onComplete(user, ref, ok);
    if (serial != 0) {
      {
        std::lock_guard<std::mutex> lock(state->handledMutex);
        if (serial > state->handledSerial)
          state->handledSerial = serial;
      }
      state->handledCv.notify_all();
    }
  }];
  sg_commit();
  return true;
}

} // namespace node_compat
} // namespace hermes
