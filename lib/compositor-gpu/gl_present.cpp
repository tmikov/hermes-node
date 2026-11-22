/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "gl_present.h"

#include "present_constants.h"
#include "sokol_config.h"
#include "sokol_log.h"
#include "ui_trace.h"

// THE RENAME COMES FIRST, and "first" means before sokol_gfx.h's
// DECLARATIONS rather than before its implementation -- see
// sokol_gfx_present_impl.cpp for the measurement behind that. Every sg_*
// call written in this file therefore compiles to the sgp_* instance: the
// one the platform thread owns. sgp_rename.h #errors if sokol_gfx.h got
// here first, so getting this backwards is a compile error rather than a
// run-time mystery.
#include "sgp_rename.h"

#include <sokol_gfx.h>

// Declarations only: sokol_app's implementation is compiled once, in
// tools/hermes-node-ui/sokol_app_impl.cpp, and these symbols resolve
// against that binary at link time. This is the one file in
// lib/compositor-gpu/ that needs them, because presenting means drawing
// into sokol_app's swapchain and only sokol_app knows what that is.
#include <sokol_app.h>

// ...and the one file that compiles sokol_glue's implementation, which the
// design requires be compiled "exactly once, in the presenting unit, or
// not at all". sglue_* are functions of their own, untouched by the rename
// above, and they only build sg_* STRUCTS (types are never renamed), so
// they compose with the rename rather than fighting it.
#define SOKOL_GLUE_IMPL
#include <sokol_glue.h>

// Raw GL, for the two things sokol_gfx does not expose: the consumer fence
// this instance inserts after a blit, and the pixel readback the capture
// hook needs. GL_GLEXT_PROTOTYPES is what makes the 3.2+/4.x entry points
// declared rather than merely present in libGL; the spike probe
// (test/fixtures/hnui-spike/glx_share.c) reaches them the same way.
#define GL_GLEXT_PROTOTYPES 1
#include <GL/gl.h>

#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

namespace hermes {
namespace node_compat {

namespace {

/// Sokol's logger for the PRESENTING instance.
///
/// Its own copy rather than the one in ui_platform.cpp, because the two
/// instances are separate builds of the same header and a message needs to
/// say which one produced it: "an error in sg_apply_bindings" means
/// something very different on the producer than on the presenter.
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

/// Drain whatever GL errors raw calls left behind.
///
/// Required, not hygiene: the Linux spike's first probe aborted inside
/// Sokol precisely because it made raw GL calls after `sg_setup()` without
/// draining errors first (`docs/notes/2026-09-19-linux-spike-results.md`,
/// section 4). Sokol asserts on `glGetError() == GL_NO_ERROR` at points
/// that have nothing to do with whoever actually provoked the error, so an
/// undrained one surfaces as a crash in unrelated code.
void drainGlErrors() {
  while (glGetError() != GL_NO_ERROR) {
  }
}

/// The full-screen triangle. Three vertices, no vertex buffer, no uniform
/// block: the positions are computed from `gl_VertexID`, which is why the
/// pipeline below declares no vertex layout and the draw below applies no
/// vertex bindings.
///
/// `p` takes the values (0,0), (2,0), (0,2), so the clip-space positions
/// are (-1,-1), (3,-1), (-1,3) -- one triangle that covers the whole
/// viewport and then some, with `uv` running 0..1 across exactly the
/// visible part. A quad would need two triangles and a seam down the
/// diagonal where interpolation differs; this is the standard trick that
/// avoids both.
///
/// `#version 410` rather than 430: Sokol accepts either for the desktop GL
/// backend (`sokol_gfx.h:924`), and 410 is the lower floor -- nothing here
/// uses a storage buffer, which is the only reason to demand 430.
const char *const kBlitVertexSource = R"(#version 410
out vec2 uv;
void main() {
  vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
  uv = p;
  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

/// Straight texture fetch: the stretching, when the surface and the
/// framebuffer differ in size during a resize, is the sampler's linear
/// filtering doing its job over a full-viewport triangle, not arithmetic
/// here.
const char *const kBlitFragmentSource = R"(#version 410
uniform sampler2D tex;
in vec2 uv;
out vec4 frag_color;
void main() {
  frag_color = texture(tex, uv);
}
)";

} // namespace

//===========================================================================
// State
//===========================================================================

struct GlPresenter::Wrapped {
  sg_image images[kSlotsPerGeneration]{};
  sg_view views[kSlotsPerGeneration]{};
  int width = 0;
  int height = 0;
};

struct GlPresenter::State {
  sg_shader shader{};
  sg_pipeline pipeline{};
  sg_sampler sampler{};
  sg_swapchain swapchain{};
  bool swapchainValid = false;
  /// Keyed by GenerationId::value, like SurfaceTable's own map.
  std::map<uint32_t, Wrapped> wrapped;
};

GlPresenter::~GlPresenter() {
  // Deliberately does NOT call sg_shutdown() or destroy the wrappers. This
  // object is destroyed on the way out of a process whose JS thread may
  // still be live and whose GL context may already be gone; the same
  // reasoning SurfaceTable's destructor records for not running its
  // destroy function. In hermes-node-ui it is not reached at all: the
  // backend is never deleted, sapp_run() never returns, and every exit
  // path ends in _exit() on the JS thread (CLAUDE.md, UI Compositor), which
  // frees the driver's resources wholesale. There is no orderly teardown,
  // by design.
  delete state_;
}

//===========================================================================
// Setup
//===========================================================================

bool GlPresenter::setup(std::string &error) {
  state_ = new State();

  sg_desc desc = {};
  desc.environment = sglue_environment();
  desc.logger.func = presentLog;
  sg_setup(&desc);
  if (!sg_isvalid()) {
    error =
        "the presenting sokol_gfx instance could not be set up against "
        "sokol_app's GL context.";
    return false;
  }

  // Recorded for the producer, which must allocate its surfaces in the
  // swapchain's colour format and cannot ask sokol_app itself.
  colorFormat_ = static_cast<int>(desc.environment.defaults.color_format);
  sampleCount_ = desc.environment.defaults.sample_count;

  sg_shader_desc shaderDesc = {};
  shaderDesc.vertex_func.source = kBlitVertexSource;
  shaderDesc.fragment_func.source = kBlitFragmentSource;
  shaderDesc.views[0].texture.stage = SG_SHADERSTAGE_FRAGMENT;
  shaderDesc.views[0].texture.image_type = SG_IMAGETYPE_2D;
  shaderDesc.views[0].texture.sample_type = SG_IMAGESAMPLETYPE_FLOAT;
  shaderDesc.samplers[0].stage = SG_SHADERSTAGE_FRAGMENT;
  shaderDesc.samplers[0].sampler_type = SG_SAMPLERTYPE_FILTERING;
  shaderDesc.texture_sampler_pairs[0].stage = SG_SHADERSTAGE_FRAGMENT;
  shaderDesc.texture_sampler_pairs[0].view_slot = 0;
  shaderDesc.texture_sampler_pairs[0].sampler_slot = 0;
  // Required on GL 4.1 (`sokol_gfx.h:4041`): the binding is made by name,
  // not by slot number, so this string must match the sampler uniform in
  // the fragment source above.
  shaderDesc.texture_sampler_pairs[0].glsl_name = "tex";
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
  // .colors[0].pixel_format, .depth.pixel_format and .sample_count are all
  // left to Sokol's own defaulting, which takes them from
  // desc.environment above -- i.e. from the swapchain itself
  // (`_sg_pipeline_desc_defaults`, sokol_gfx.h:25900). Restating them here
  // would be three more places for the pipeline and the swapchain to
  // disagree, and a disagreement is rejected at sg_apply_pipeline with a
  // message about formats rather than about the copy that drifted.
  pipelineDesc.label = "hnui-blit-pipeline";
  state_->pipeline = sg_make_pipeline(&pipelineDesc);
  if (sg_query_pipeline_state(state_->pipeline) != SG_RESOURCESTATE_VALID) {
    error = "the presenting blit pipeline could not be created.";
    return false;
  }

  sg_sampler_desc samplerDesc = {};
  // Linear, per spec ("draws a full-screen triangle into the swapchain
  // pass with linear filtering ... When sizes differ -- during a resize --
  // it stretches"). At 1:1, linear filtering at texel centres reduces to
  // an exact copy, which is what lets a capture taken under the agreement
  // rule be compared exactly rather than with a tolerance.
  samplerDesc.min_filter = SG_FILTER_LINEAR;
  samplerDesc.mag_filter = SG_FILTER_LINEAR;
  // Clamp, because the full-screen triangle's uv runs past 1.0 outside the
  // visible region; with REPEAT (Sokol's default) the off-screen part of
  // the triangle would sample wrapped texels, which costs nothing visually
  // but makes the shader's behaviour depend on geometry nobody sees.
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
// Wrappers
//===========================================================================

bool GlPresenter::wrap(
    GenerationId gen,
    const uint32_t *textures,
    int count,
    int width,
    int height,
    std::string &error) {
  if (state_ == nullptr) {
    error = "the presenting instance has not been set up.";
    return false;
  }
  // Idempotent, so the frame callback can call this unconditionally for
  // whatever it is about to present rather than tracking which
  // generations it has already handled.
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
    // Texture injection (spec: Presenting -- "wraps each surface's native
    // texture once per generation, via texture injection
    // (gl_textures[0] / mtl_textures[0])"). The producer created these
    // names on its own context; they are visible here because the two
    // contexts share a share group, measured on llvmpipe and on AMD
    // radeonsi (docs/notes/2026-09-21-linux-hardware-spike-results.md,
    // question E -- content, not just the name).
    //
    // ONE texture, not SG_NUM_INFLIGHT_FRAMES of them: the image is
    // immutable (Sokol's default usage), so it has a single slot. The
    // spike checked exactly this case.
    imageDesc.gl_textures[0] = textures[i];
    imageDesc.gl_texture_target = GL_TEXTURE_2D;
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

void GlPresenter::releaseWrappers(GenerationId gen) {
  if (state_ == nullptr)
    return;
  auto it = state_->wrapped.find(gen.value);
  if (it == state_->wrapped.end())
    return;
  for (int i = 0; i < kSlotsPerGeneration; ++i) {
    sg_destroy_view(it->second.views[i]);
    sg_destroy_image(it->second.images[i]);
  }
  state_->wrapped.erase(it);
  uiTrace("released presenting wrappers for generation %u", gen.value);
}

bool GlPresenter::isWrapped(GenerationId gen) const {
  return state_ != nullptr && state_->wrapped.count(gen.value) != 0;
}

//===========================================================================
// Presenting
//===========================================================================

bool GlPresenter::acquireSwapchain() {
  if (state_ == nullptr)
    return false;
  // Exactly once per frame callback -- sokol_app says so
  // (`sokol_app.h:1935`), and on Metal this is what advances to the next
  // drawable. CompositorBackend::acquireFrameSwapchain() exists to give
  // this call one site.
  state_->swapchain = sglue_swapchain();
  state_->swapchainValid = !state_->swapchain.invalid;
  swapchainWidth_ = state_->swapchain.width;
  swapchainHeight_ = state_->swapchain.height;
  return state_->swapchainValid;
}

void GlPresenter::clear() {
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
  sg_commit();
}

void *GlPresenter::blitAndFence(SurfaceRef ref) {
  if (state_ == nullptr || !state_->swapchainValid)
    return nullptr;
  auto it = state_->wrapped.find(ref.gen.value);
  if (it == state_->wrapped.end())
    return nullptr;
  if (ref.slot >= kSlotsPerGeneration)
    return nullptr;

  sg_pass pass = {};
  // CLEAR rather than DONTCARE even though the triangle covers every
  // pixel: it makes "the blit did not draw" show up as the clear colour
  // rather than as undefined back-buffer contents, which is the difference
  // between a test that fails with a legible picture and one that fails
  // differently on every driver.
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
  sg_commit();

  // The consumer fence (spec, Completion per backend, GL consumer column:
  // "A fence inserted after the blit, polled the same way"). It is created
  // and polled on this one thread, so nothing is transferred -- unlike the
  // producer's, which crosses threads.
  //
  // glFlush for the same reason the producer flushes: a fence that has not
  // been flushed into the command stream need never signal, and
  // glClientWaitSync with timeout 0 and no flush bit will not flush it.
  // sokol_app's buffer swap would flush eventually, but "eventually" is
  // not a guarantee a read reference should wait on.
  GLsync fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
  glFlush();
  drainGlErrors();
  return static_cast<void *>(fence);
}

bool GlPresenter::readPixels(int width, int height, uint8_t *out) {
  if (state_ == nullptr || !state_->swapchainValid || out == nullptr)
    return false;
  if (width <= 0 || height <= 0)
    return false;

  // Bound explicitly rather than trusting what the pass left current, and
  // to the framebuffer the swapchain names (0 for sokol_app on GL, but
  // reading the field is what makes that a fact rather than an
  // assumption).
  glBindFramebuffer(GL_FRAMEBUFFER, state_->swapchain.gl.framebuffer);
  // Rows are 4-byte pixels, so the default 4-byte pack alignment happens
  // to be right -- set to 1 anyway, because "happens to be right" stops
  // being true the moment anyone reads a 3-byte format here.
  glPixelStorei(GL_PACK_ALIGNMENT, 1);

  std::vector<uint8_t> bottomUp(static_cast<size_t>(width) * height * 4);
  glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, bottomUp.data());
  const bool ok = glGetError() == GL_NO_ERROR;

  // GL hands back bottom-up rows; this interface promises top-down (see
  // CompositorBackend::readPresentedPixels), so that a mirrored image is a
  // test failure rather than a convention mismatch nobody can see.
  if (ok) {
    const size_t stride = static_cast<size_t>(width) * 4;
    for (int y = 0; y < height; ++y) {
      std::memcpy(
          out + static_cast<size_t>(y) * stride,
          bottomUp.data() + static_cast<size_t>(height - 1 - y) * stride,
          stride);
    }
  }

  // Raw GL calls happened; hand sokol back a cache that matches reality.
  // See drainGlErrors() above for what skipping this costs.
  drainGlErrors();
  sg_reset_state_cache();
  return ok;
}

} // namespace node_compat
} // namespace hermes
