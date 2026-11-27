/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// The renderer half of the backend, written against sokol_gfx directly
// through the host's exported sg_* symbols. So far it owns one pass on the
// target and clears it; the pipeline, buffers, textures and the draw loop
// join it with the texture protocol.

#include "imgui_addon.h"

#include <sokol_gfx.h>

namespace hnimgui {

namespace {

/// The clear colour, {26, 26, 31} as bytes (`CLEAR` in
/// test/fixtures/imgui/harness.js).
constexpr float kClear[4] = {
    26.0f / 255.0f,
    26.0f / 255.0f,
    31.0f / 255.0f,
    1.0f};

RendererStats gStats = {};

} // namespace

void rendererInit() {
  ImGuiIO &io = ImGui::GetIO();
  io.BackendRendererName = "hermes-node-imgui (sokol_gfx)";
  // Set from the start: ImGui 1.93 asserts in NewFrame() on a backend with
  // neither this flag nor a font atlas built for it.
  io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
}

void rendererDraw(ImDrawData *dd, const hnui_target &target) {
  // Totals of what ImGui produced. Nothing is drawn from them yet: drawing
  // needs the textures ImGui asks for, and ImDrawCmd::GetTexID() asserts
  // on one that was never uploaded.
  RendererStats stats = {};
  for (const ImDrawList *list : dd->CmdLists)
    stats.commands += list->CmdBuffer.Size;
  stats.vertices = dd->TotalVtxCount;
  stats.indices = dd->TotalIdxCount;
  stats.liveTextures = 0;
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
  sg_end_pass();
  // No sg_commit: the host commits once the tick's last callback returns.
}

RendererStats rendererStats() {
  return gStats;
}

} // namespace hnimgui
