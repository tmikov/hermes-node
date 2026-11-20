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

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <cstdio>

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

struct MetalPresenter::State {
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

bool MetalPresenter::setup(void *mtlDevice, std::string &error) {
  state_ = new State();

  sg_desc desc = {};
  desc.environment = sglue_environment();
  // The backend's device and sokol_app's must be one object: the producer
  // will allocate the surfaces on the backend's device and this instance
  // will sample them, and a texture belongs to the device that made it.
  if (desc.environment.metal.device == nullptr ||
      desc.environment.metal.device != mtlDevice) {
    error =
        "sokol_app's Metal device is not the one the backend selected, so "
        "the presenting instance could not sample what the producer draws.";
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
    // Committed means Sokol let go of the buffer and Metal has it.
    MTLCommandBufferStatus status = commandBuffer != nil
        ? commandBuffer.status
        : MTLCommandBufferStatusNotEnqueued;
    bool committed = commandBuffer != nil &&
        hnui_sgp_mtl_command_buffer() == nullptr &&
        status >= MTLCommandBufferStatusCommitted &&
        status != MTLCommandBufferStatusError;
    if (committed)
      uiTrace("present: first clear committed");
    else
      uiTrace(
          "present: first clear NOT committed (command buffer %s, status %d)",
          commandBuffer != nil ? "present" : "absent",
          (int)status);
  }
}

} // namespace node_compat
} // namespace hermes
