/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// The platform half of the backend: what ImGui's GLFW or SDL backend does,
// on hnui. So far the display size, the framebuffer scale and the time
// step; events, the cursor and the clipboard join it in later tasks.

#include "imgui_addon.h"

#include <chrono>

namespace hnimgui {

namespace {

/// The clock reading of the previous newFrame(); unset before the first.
bool gHaveLastFrameTime = false;
std::chrono::steady_clock::time_point gLastFrameTime;

} // namespace

void platformInit(hnui_window *) {
  ImGuiIO &io = ImGui::GetIO();
  io.BackendPlatformName = "hermes-node-ui (hnui)";
}

void platformNewFrame(hnui_window *w) {
  ImGuiIO &io = ImGui::GetIO();

  // Everything in logical units, read every frame, so a resize needs no
  // event handling: the display is the window's logical size, and the
  // framebuffer scale is what turns it into the target's pixels.
  hnui_window_state state = {};
  state.size = static_cast<uint32_t>(sizeof(state));
  hnui_get_window_state(w, &state);
  io.DisplaySize =
      ImVec2(static_cast<float>(state.width), static_cast<float>(state.height));
  // 1 for a zero-sized window: ImGui asserts on a zero scale.
  if (state.width > 0 && state.height > 0) {
    io.DisplayFramebufferScale = ImVec2(
        static_cast<float>(state.framebuffer_width) / state.width,
        static_cast<float>(state.framebuffer_height) / state.height);
  } else {
    io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
  }

  // ImGui asserts on a time step that is not positive, and two newFrame()
  // calls in one callback are microseconds apart.
  const auto now = std::chrono::steady_clock::now();
  float delta = 1.0f / 60.0f;
  if (gHaveLastFrameTime)
    delta = std::chrono::duration<float>(now - gLastFrameTime).count();
  io.DeltaTime = delta < 1e-5f ? 1e-5f : delta;
  gLastFrameTime = now;
  gHaveLastFrameTime = true;
}

void platformAfterRender(hnui_window *) {
  // The cursor is set from here once the platform half handles it.
}

} // namespace hnimgui
