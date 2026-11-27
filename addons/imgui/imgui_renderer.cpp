/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// The renderer half of the backend, written against sokol_gfx directly
// through the host's exported sg_* symbols: one shader with hand-written
// sources for the two backends, one pipeline, appended vertex and index
// buffers, ImGui's texture protocol, and the draw loop.
//
// Every sg_* call that makes a resource or draws is made on the JS thread,
// inside a frame callback, after the hnui_frame_target that gave the target
// and before the callback returns: that is where the host's producing
// instance is ours to use. rendererInit() only queries.

#include "imgui_addon.h"

#include <sokol_gfx.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace hnimgui {

namespace {

/// The clear colour, {26, 26, 31} as bytes (`CLEAR` in
/// test/fixtures/imgui/harness.js).
constexpr float kClear[4] = {
    26.0f / 255.0f,
    26.0f / 255.0f,
    31.0f / 255.0f,
    1.0f};

/// Initial buffer capacities, in elements. Growth replaces a buffer by one
/// at least twice the size, so these only set where growth starts.
constexpr size_t kInitialVertices = 16384;
constexpr size_t kInitialIndices = 32768;

/// The largest texture ImGui may ask for (platform_io.Renderer_TextureMax*).
constexpr int kTextureMaxSize = 8192;

/// The vertex shader's one uniform block: the projection, column-major.
struct VsParams {
  float proj[4][4];
};

// GLSL 410, modelled on imgui_impl_opengl3's 410 shader. The attribute
// locations are the pipeline's attribute slots; the names are what Sokol
// looks them up by.
const char kVsGlsl[] =
    "#version 410\n"
    "uniform mat4 proj;\n"
    "layout(location = 0) in vec2 position;\n"
    "layout(location = 1) in vec2 texcoord0;\n"
    "layout(location = 2) in vec4 color0;\n"
    "out vec2 uv;\n"
    "out vec4 color;\n"
    "void main() {\n"
    "  uv = texcoord0;\n"
    "  color = color0;\n"
    "  gl_Position = proj * vec4(position, 0.0, 1.0);\n"
    "}\n";

const char kFsGlsl[] =
    "#version 410\n"
    "uniform sampler2D tex_smp;\n"
    "in vec2 uv;\n"
    "in vec4 color;\n"
    "layout(location = 0) out vec4 frag_color;\n"
    "void main() {\n"
    "  frag_color = color * texture(tex_smp, uv);\n"
    "}\n";

// Metal Shading Language, modelled on imgui_impl_metal.mm's shader and
// compiled by Sokol at run time. The colour arrives as UBYTE4N, so it is
// already a normalized float4 here (imgui_impl_metal.mm declares uchar4 and
// divides by 255 because its vertex descriptor says UChar4). The uniform
// block is [[buffer(0)]] (msl_buffer_n = 0); Sokol places vertex buffers
// after the uniform-block slots, through the vertex descriptor. The two
// stages are compiled separately, so the interpolants are matched by
// [[user(...)]] names.
const char kVsMsl[] =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct vs_params {\n"
    "  float4x4 proj;\n"
    "};\n"
    "struct vs_in {\n"
    "  float2 position [[attribute(0)]];\n"
    "  float2 texcoord0 [[attribute(1)]];\n"
    "  float4 color0 [[attribute(2)]];\n"
    "};\n"
    "struct vs_out {\n"
    "  float4 position [[position]];\n"
    "  float2 uv [[user(locn0)]];\n"
    "  float4 color [[user(locn1)]];\n"
    "};\n"
    "vertex vs_out vs_main(vs_in in [[stage_in]],\n"
    "                      constant vs_params &params [[buffer(0)]]) {\n"
    "  vs_out out;\n"
    "  out.position = params.proj * float4(in.position, 0.0, 1.0);\n"
    "  out.uv = in.texcoord0;\n"
    "  out.color = in.color0;\n"
    "  return out;\n"
    "}\n";

const char kFsMsl[] =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct fs_in {\n"
    "  float2 uv [[user(locn0)]];\n"
    "  float4 color [[user(locn1)]];\n"
    "};\n"
    "fragment float4 fs_main(fs_in in [[stage_in]],\n"
    "                        texture2d<float> tex [[texture(0)]],\n"
    "                        sampler smp [[sampler(0)]]) {\n"
    "  return in.color * tex.sample(smp, in.uv);\n"
    "}\n";

/// What the renderer holds for one ImGuiTexture, hung on
/// ImTextureData::BackendUserData. The ImGui texture id is `view.id`.
struct TextureRecord {
  sg_image image;
  sg_view view;
};

/// The backend cannot be drawn on (decided once in rendererInit()), so
/// the renderer only clears.
bool gUnusable = false;
/// The device objects, made at the first rendererDraw(), inside a frame
/// callback (test/fixtures/hnui-demo/demo_visual.c explains why creation
/// belongs there).
bool gDeviceObjectsTried = false;
bool gDeviceObjectsReady = false;
sg_shader gShader = {};
sg_sampler gSampler = {};
sg_image gWhiteImage = {};
sg_view gWhiteView = {};
/// The pipeline and the target attributes it was made for.
sg_pipeline gPipeline = {};
int gPipelineFormat = -1;
int gPipelineSamples = -1;
/// The last format and sample count a pipeline could not be made for.
int gFailedPipelineFormat = -1;
int gFailedPipelineSamples = -1;
/// The appended buffers and their sizes in bytes.
sg_buffer gVertexBuffer = {};
size_t gVertexBufferSize = 0;
sg_buffer gIndexBuffer = {};
size_t gIndexBufferSize = 0;

int gLiveTextures = 0;
RendererStats gStats = {};

/// The reset-render-state callback ImGui hands back as an identifier
/// (platform_io.DrawCallback_ResetRenderState): never called, compared.
void resetRenderStateCallback(const ImDrawList *, const ImDrawCmd *) {}

sg_buffer makeStreamBuffer(size_t bytes, bool index) {
  sg_buffer_desc desc = {};
  desc.size = bytes;
  desc.usage.vertex_buffer = !index;
  desc.usage.index_buffer = index;
  // Appended, never updated: Sokol allows one sg_update_buffer per Sokol
  // frame and any number of appends, and two render() calls in one tick
  // share a Sokol frame.
  desc.usage.dynamic_update = true;
  desc.label = index ? "imgui-indices" : "imgui-vertices";
  return sg_make_buffer(&desc);
}

/// Make the device objects that do not depend on the target. Tried once;
/// if any is not valid the renderer draws nothing.
bool ensureDeviceObjects() {
  if (gDeviceObjectsTried)
    return gDeviceObjectsReady;
  gDeviceObjectsTried = true;

  sg_shader_desc shd = {};
  if (sg_query_backend() == SG_BACKEND_GLCORE) {
    shd.vertex_func.source = kVsGlsl;
    shd.fragment_func.source = kFsGlsl;
  } else {
    shd.vertex_func.source = kVsMsl;
    shd.vertex_func.entry = "vs_main";
    shd.fragment_func.source = kFsMsl;
    shd.fragment_func.entry = "fs_main";
  }
  const char *const attrNames[3] = {"position", "texcoord0", "color0"};
  for (int i = 0; i < 3; ++i) {
    shd.attrs[i].base_type = SG_SHADERATTRBASETYPE_FLOAT;
    shd.attrs[i].glsl_name = attrNames[i];
  }
  shd.uniform_blocks[0].stage = SG_SHADERSTAGE_VERTEX;
  shd.uniform_blocks[0].size = sizeof(VsParams);
  shd.uniform_blocks[0].msl_buffer_n = 0;
  shd.uniform_blocks[0].glsl_uniforms[0].type = SG_UNIFORMTYPE_MAT4;
  shd.uniform_blocks[0].glsl_uniforms[0].glsl_name = "proj";
  shd.views[0].texture.stage = SG_SHADERSTAGE_FRAGMENT;
  shd.views[0].texture.image_type = SG_IMAGETYPE_2D;
  shd.views[0].texture.sample_type = SG_IMAGESAMPLETYPE_FLOAT;
  shd.views[0].texture.msl_texture_n = 0;
  shd.samplers[0].stage = SG_SHADERSTAGE_FRAGMENT;
  shd.samplers[0].sampler_type = SG_SAMPLERTYPE_FILTERING;
  shd.samplers[0].msl_sampler_n = 0;
  shd.texture_sampler_pairs[0].stage = SG_SHADERSTAGE_FRAGMENT;
  shd.texture_sampler_pairs[0].view_slot = 0;
  shd.texture_sampler_pairs[0].sampler_slot = 0;
  shd.texture_sampler_pairs[0].glsl_name = "tex_smp";
  shd.label = "imgui-shader";
  gShader = sg_make_shader(&shd);

  sg_sampler_desc smp = {};
  smp.min_filter = SG_FILTER_LINEAR;
  smp.mag_filter = SG_FILTER_LINEAR;
  smp.wrap_u = SG_WRAP_CLAMP_TO_EDGE;
  smp.wrap_v = SG_WRAP_CLAMP_TO_EDGE;
  smp.label = "imgui-sampler";
  gSampler = sg_make_sampler(&smp);

  // What a command with a raw invalid texture id samples: 1x1 white, so
  // the vertex colour is the fragment colour.
  static const uint32_t kWhite = 0xFFFFFFFFu;
  sg_image_desc img = {};
  img.width = 1;
  img.height = 1;
  img.pixel_format = SG_PIXELFORMAT_RGBA8;
  img.data.mip_levels[0].ptr = &kWhite;
  img.data.mip_levels[0].size = sizeof(kWhite);
  img.label = "imgui-white";
  gWhiteImage = sg_make_image(&img);
  sg_view_desc view = {};
  view.texture.image = gWhiteImage;
  gWhiteView = sg_make_view(&view);

  gVertexBufferSize = kInitialVertices * sizeof(ImDrawVert);
  gVertexBuffer = makeStreamBuffer(gVertexBufferSize, false);
  gIndexBufferSize = kInitialIndices * sizeof(ImDrawIdx);
  gIndexBuffer = makeStreamBuffer(gIndexBufferSize, true);

  gDeviceObjectsReady =
      sg_query_shader_state(gShader) == SG_RESOURCESTATE_VALID &&
      sg_query_sampler_state(gSampler) == SG_RESOURCESTATE_VALID &&
      sg_query_image_state(gWhiteImage) == SG_RESOURCESTATE_VALID &&
      sg_query_view_state(gWhiteView) == SG_RESOURCESTATE_VALID &&
      sg_query_buffer_state(gVertexBuffer) == SG_RESOURCESTATE_VALID &&
      sg_query_buffer_state(gIndexBuffer) == SG_RESOURCESTATE_VALID;
  if (!gDeviceObjectsReady) {
    // Nothing half-made stays alive: destroying an invalid or failed id is
    // a no-op in Sokol, so each is destroyed whatever its state.
    sg_destroy_buffer(gIndexBuffer);
    sg_destroy_buffer(gVertexBuffer);
    sg_destroy_view(gWhiteView);
    sg_destroy_image(gWhiteImage);
    sg_destroy_sampler(gSampler);
    sg_destroy_shader(gShader);
    gIndexBuffer = {};
    gVertexBuffer = {};
    gWhiteView = {};
    gWhiteImage = {};
    gSampler = {};
    gShader = {};
    trace("device objects failed; nothing will be drawn");
  }
  return gDeviceObjectsReady;
}

/// The pipeline for this target's colour format and sample count, rebuilt
/// when either differs from the last target's. Returns false if it could
/// not be made.
bool ensurePipeline(const hnui_target &target) {
  if (gPipeline.id != SG_INVALID_ID && gPipelineFormat == target.color_format &&
      gPipelineSamples == target.sample_count)
    return true;
  // A pair that failed once fails again: not retried, and traced once.
  if (gFailedPipelineFormat == target.color_format &&
      gFailedPipelineSamples == target.sample_count)
    return false;
  if (gPipeline.id != SG_INVALID_ID)
    sg_destroy_pipeline(gPipeline);

  sg_pipeline_desc pip = {};
  pip.shader = gShader;
  pip.layout.buffers[0].stride = sizeof(ImDrawVert);
  pip.layout.attrs[0].offset = offsetof(ImDrawVert, pos);
  pip.layout.attrs[0].format = SG_VERTEXFORMAT_FLOAT2;
  pip.layout.attrs[1].offset = offsetof(ImDrawVert, uv);
  pip.layout.attrs[1].format = SG_VERTEXFORMAT_FLOAT2;
  pip.layout.attrs[2].offset = offsetof(ImDrawVert, col);
  pip.layout.attrs[2].format = SG_VERTEXFORMAT_UBYTE4N;
  static_assert(sizeof(ImDrawIdx) == 2, "the pipeline says UINT16");
  pip.index_type = SG_INDEXTYPE_UINT16;
  pip.cull_mode = SG_CULLMODE_NONE;
  pip.depth.pixel_format = SG_PIXELFORMAT_NONE;
  pip.depth.write_enabled = false;
  pip.colors[0].pixel_format =
      static_cast<sg_pixel_format>(target.color_format);
  pip.colors[0].blend.enabled = true;
  pip.colors[0].blend.src_factor_rgb = SG_BLENDFACTOR_SRC_ALPHA;
  pip.colors[0].blend.dst_factor_rgb = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
  pip.colors[0].blend.src_factor_alpha = SG_BLENDFACTOR_ONE;
  pip.colors[0].blend.dst_factor_alpha = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
  pip.sample_count = target.sample_count;
  pip.label = "imgui-pipeline";
  gPipeline = sg_make_pipeline(&pip);
  if (sg_query_pipeline_state(gPipeline) != SG_RESOURCESTATE_VALID) {
    sg_destroy_pipeline(gPipeline);
    gPipeline = {};
    gPipelineFormat = -1;
    gPipelineSamples = -1;
    gFailedPipelineFormat = target.color_format;
    gFailedPipelineSamples = target.sample_count;
    trace(
        "pipeline failed (format %d, samples %d)",
        target.color_format,
        target.sample_count);
    return false;
  }
  gPipelineFormat = target.color_format;
  gPipelineSamples = target.sample_count;
  trace(
      "pipeline created (format %d, samples %d)",
      target.color_format,
      target.sample_count);
  return true;
}

/// ImGui's texture protocol, for one texture whose status is not OK. A
/// texture to create or update is uploaded whole as a new immutable image,
/// which is legal any number of times per Sokol frame (sg_update_image is
/// once per frame); the previous image is destroyed only once the new one
/// is valid, and ImGui's status changes only then.
void updateTexture(ImTextureData *tex) {
  if (tex->Status == ImTextureStatus_WantCreate ||
      tex->Status == ImTextureStatus_WantUpdates) {
    // ImGui's default, and the only format this renderer uploads.
    if (tex->Format != ImTextureFormat_RGBA32) {
      trace("texture %d failed (format %d)", tex->UniqueID, tex->Format);
      return;
    }
    sg_image_desc img = {};
    img.width = tex->Width;
    img.height = tex->Height;
    img.pixel_format = SG_PIXELFORMAT_RGBA8;
    img.data.mip_levels[0].ptr = tex->GetPixels();
    img.data.mip_levels[0].size = static_cast<size_t>(tex->GetSizeInBytes());
    img.label = "imgui-texture";
    const sg_image image = sg_make_image(&img);
    sg_view_desc vd = {};
    vd.texture.image = image;
    const sg_view view = sg_make_view(&vd);
    if (sg_query_image_state(image) != SG_RESOURCESTATE_VALID ||
        sg_query_view_state(view) != SG_RESOURCESTATE_VALID) {
      sg_destroy_view(view);
      sg_destroy_image(image);
      trace("texture %d failed", tex->UniqueID);
      return;
    }
    auto *record = static_cast<TextureRecord *>(tex->BackendUserData);
    const bool replaced = record != nullptr;
    if (replaced) {
      sg_destroy_view(record->view);
      sg_destroy_image(record->image);
    } else {
      record = new TextureRecord();
      tex->BackendUserData = record;
      ++gLiveTextures;
    }
    record->image = image;
    record->view = view;
    tex->SetTexID(static_cast<ImTextureID>(view.id));
    tex->SetStatus(ImTextureStatus_OK);
    trace(
        "texture %d %s %dx%d",
        tex->UniqueID,
        replaced ? "replaced" : "created",
        tex->Width,
        tex->Height);
  } else if (
      tex->Status == ImTextureStatus_WantDestroy && tex->UnusedFrames > 0) {
    auto *record = static_cast<TextureRecord *>(tex->BackendUserData);
    if (record != nullptr) {
      sg_destroy_view(record->view);
      sg_destroy_image(record->image);
      delete record;
      --gLiveTextures;
    }
    tex->SetTexID(ImTextureID_Invalid);
    tex->BackendUserData = nullptr;
    tex->SetStatus(ImTextureStatus_Destroyed);
    trace("texture %d destroyed", tex->UniqueID);
  }
}

/// Make sure \p buffer (of \p size bytes) can take an append of \p bytes
/// in this Sokol frame: if it cannot, replace it by a buffer of at least
/// twice the size and at least \p bytes, which has had no append in this
/// frame. Draws already issued from the old one are unaffected by its
/// destruction. Returns false if the new buffer could not be made.
bool reserveAppend(sg_buffer &buffer, size_t &size, size_t bytes, bool index) {
  if (!sg_query_buffer_will_overflow(buffer, bytes))
    return true;
  // The append rounds its size up to 4 bytes, and so must the new size.
  const size_t needed = (bytes + 3) & ~static_cast<size_t>(3);
  size_t newSize = size * 2;
  if (newSize < needed)
    newSize = needed;
  const sg_buffer grown = makeStreamBuffer(newSize, index);
  if (sg_query_buffer_state(grown) != SG_RESOURCESTATE_VALID) {
    sg_destroy_buffer(grown);
    trace(
        "%s buffer could not grow to %zu bytes",
        index ? "index" : "vertex",
        newSize);
    return false;
  }
  sg_destroy_buffer(buffer);
  buffer = grown;
  size = newSize;
  trace("%s buffer grown to %zu bytes", index ? "index" : "vertex", newSize);
  return true;
}

/// The render state every draw starts from: pipeline, viewport and
/// projection. Run after sg_begin_pass and again for the reset callback.
/// (Bindings and the scissor are applied per command anyway.)
void setupRenderState(const ImDrawData *dd, const hnui_target &target) {
  sg_apply_pipeline(gPipeline);
  sg_apply_viewport(0, 0, target.width, target.height, true);

  // imgui_impl_opengl3's orthographic projection over DisplayPos ..
  // DisplayPos + DisplaySize, ImGui's y = 0 at clip-space +1. That is the
  // top on both backends as Sokol presents them: on GL, where the target's
  // row 0 is its bottom, Sokol's top-left viewport and scissor rectangles
  // count from the same top. test/ui-imgui-draw.js proves both halves:
  // CLIP the scissor's orientation, MANY the projection's.
  const float L = dd->DisplayPos.x;
  const float R = dd->DisplayPos.x + dd->DisplaySize.x;
  const float T = dd->DisplayPos.y;
  const float B = dd->DisplayPos.y + dd->DisplaySize.y;
  const VsParams params = {{
      {2.0f / (R - L), 0.0f, 0.0f, 0.0f},
      {0.0f, 2.0f / (T - B), 0.0f, 0.0f},
      {0.0f, 0.0f, -1.0f, 0.0f},
      {(R + L) / (L - R), (T + B) / (B - T), 0.0f, 1.0f},
  }};
  const sg_range range = {&params, sizeof(params)};
  sg_apply_uniforms(0, &range);
}

/// Draw one list's commands from the bytes appended for it.
void drawList(
    const ImDrawData *dd,
    const ImDrawList *list,
    const hnui_target &target,
    int vertexOffset,
    int indexOffset) {
  const ImVec2 clipOff = dd->DisplayPos;
  const ImVec2 clipScale = dd->FramebufferScale;
  const float fbWidth = static_cast<float>(target.width);
  const float fbHeight = static_cast<float>(target.height);
  ImDrawCallback reset = ImGui::GetPlatformIO().DrawCallback_ResetRenderState;

  for (const ImDrawCmd &cmd : list->CmdBuffer) {
    if (cmd.UserCallback != nullptr) {
      if (cmd.UserCallback == reset)
        setupRenderState(dd, target);
      else
        cmd.UserCallback(list, &cmd);
      continue;
    }

    // imgui_impl_metal.mm's clip rectangle: into framebuffer space,
    // clamped to the framebuffer, empty skipped.
    ImVec2 clipMin(
        (cmd.ClipRect.x - clipOff.x) * clipScale.x,
        (cmd.ClipRect.y - clipOff.y) * clipScale.y);
    ImVec2 clipMax(
        (cmd.ClipRect.z - clipOff.x) * clipScale.x,
        (cmd.ClipRect.w - clipOff.y) * clipScale.y);
    if (clipMin.x < 0.0f)
      clipMin.x = 0.0f;
    if (clipMin.y < 0.0f)
      clipMin.y = 0.0f;
    if (clipMax.x > fbWidth)
      clipMax.x = fbWidth;
    if (clipMax.y > fbHeight)
      clipMax.y = fbHeight;
    if (clipMax.x <= clipMin.x || clipMax.y <= clipMin.y)
      continue;
    if (cmd.ElemCount == 0)
      continue;

    // A texture whose upload failed is still not uploaded: skip the command
    // rather than ask GetTexID(), which asserts on it.
    if (cmd.TexRef._TexData != nullptr &&
        cmd.TexRef._TexData->TexID == ImTextureID_Invalid)
      continue;
    const ImTextureID texId = cmd.GetTexID();

    sg_apply_scissor_rectf(
        clipMin.x,
        clipMin.y,
        clipMax.x - clipMin.x,
        clipMax.y - clipMin.y,
        true);
    sg_bindings bind = {};
    bind.vertex_buffers[0] = gVertexBuffer;
    bind.vertex_buffer_offsets[0] = vertexOffset;
    bind.index_buffer = gIndexBuffer;
    bind.index_buffer_offset = indexOffset;
    bind.views[0] = texId == ImTextureID_Invalid
        ? gWhiteView
        : sg_view{static_cast<uint32_t>(texId)};
    bind.samplers[0] = gSampler;
    sg_apply_bindings(&bind);
    sg_draw_ex(
        static_cast<int>(cmd.IdxOffset),
        static_cast<int>(cmd.ElemCount),
        1,
        static_cast<int>(cmd.VtxOffset),
        0);
  }
}

} // namespace

void rendererInit() {
  ImGuiIO &io = ImGui::GetIO();
  io.BackendRendererName = "hermes-node-imgui (sokol_gfx)";
  // Set from the start: ImGui 1.93 asserts in NewFrame() on a backend with
  // neither this flag nor a font atlas built for it.
  io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
  // A command's VtxOffset becomes the draw's base vertex, so 16-bit
  // indices reach lists of any size.
  io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;

  ImGuiPlatformIO &pio = ImGui::GetPlatformIO();
  pio.Renderer_TextureMaxWidth = kTextureMaxSize;
  pio.Renderer_TextureMaxHeight = kTextureMaxSize;
  pio.DrawCallback_ResetRenderState = resetRenderStateCallback;

  const sg_backend backend = sg_query_backend();
  if (backend != SG_BACKEND_GLCORE && backend != SG_BACKEND_METAL_MACOS) {
    gUnusable = true;
    trace("unsupported sokol backend %d", static_cast<int>(backend));
  } else if (!sg_query_features().draw_base_vertex) {
    // ImGui does not split a list by itself, so without a base vertex a
    // list past 65,536 vertices cannot be drawn with 16-bit indices. Said
    // unconditionally: a renderer that silently draws nothing is the
    // failure this line exists to prevent.
    gUnusable = true;
    std::fprintf(stderr, "[ui] imgui: sokol backend lacks draw_base_vertex\n");
    std::fflush(stderr);
  }
}

void rendererDraw(ImDrawData *dd, const hnui_target &target) {
  RendererStats stats = {};
  for (const ImDrawList *list : dd->CmdLists)
    stats.commands += list->CmdBuffer.Size;
  stats.vertices = dd->TotalVtxCount;
  stats.indices = dd->TotalIdxCount;

  const bool usable = !gUnusable && ensureDeviceObjects();
  // Textures first, before the pass: a command refers to its texture by
  // the id this hands ImGui.
  if (usable && dd->Textures != nullptr) {
    for (ImTextureData *tex : *dd->Textures)
      if (tex->Status != ImTextureStatus_OK)
        updateTexture(tex);
  }
  stats.liveTextures = gLiveTextures;
  gStats = stats;

  // One pass on the target, begun as test/fixtures/hnui-demo/demo_visual.c
  // begins its own.
  sg_pass pass = {};
  // By value: hnui_gpu_view and sg_view share a layout.
  pass.attachments.colors[0].id = target.color_view.id;
  pass.action.colors[0].load_action = SG_LOADACTION_CLEAR;
  pass.action.colors[0].store_action = SG_STOREACTION_STORE;
  pass.action.colors[0].clear_value.r = kClear[0];
  pass.action.colors[0].clear_value.g = kClear[1];
  pass.action.colors[0].clear_value.b = kClear[2];
  pass.action.colors[0].clear_value.a = kClear[3];
  sg_begin_pass(&pass);

  if (usable && ensurePipeline(target)) {
    setupRenderState(dd, target);
    // Each list is appended and then drawn before the next is appended,
    // because an append that would not fit replaces the buffer, and the
    // offsets a list is drawn with belong to the buffer it was appended to.
    for (const ImDrawList *list : dd->CmdLists) {
      int vertexOffset = 0;
      int indexOffset = 0;
      const size_t vtxBytes =
          static_cast<size_t>(list->VtxBuffer.Size) * sizeof(ImDrawVert);
      const size_t idxBytes =
          static_cast<size_t>(list->IdxBuffer.Size) * sizeof(ImDrawIdx);
      if (vtxBytes > 0 && idxBytes > 0) {
        if (!reserveAppend(gVertexBuffer, gVertexBufferSize, vtxBytes, false) ||
            !reserveAppend(gIndexBuffer, gIndexBufferSize, idxBytes, true))
          break;
        const sg_range vtx = {list->VtxBuffer.Data, vtxBytes};
        const sg_range idx = {list->IdxBuffer.Data, idxBytes};
        vertexOffset = sg_append_buffer(gVertexBuffer, &vtx);
        indexOffset = sg_append_buffer(gIndexBuffer, &idx);
      }
      drawList(dd, list, target, vertexOffset, indexOffset);
    }
  }

  sg_end_pass();
  // No sg_commit: the host commits once the tick's last callback returns.
}

RendererStats rendererStats() {
  return gStats;
}

} // namespace hnimgui
