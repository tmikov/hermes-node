/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// Declarations shared by the addon's units. Every unit sees exactly these;
// everything else stays file-local.

#ifndef HERMES_NODE_IMGUI_ADDON_H
#define HERMES_NODE_IMGUI_ADDON_H

#include <hnui.h>
#include <imgui.h>
#include <node_api.h>

namespace hnimgui {

// "[ui] imgui: ..." when UI tracing is on
void trace(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
bool testHooksEnabled(); // HERMES_NODE_UI_TEST_HOOKS, same rule as the host
// frame state, for the test hooks
bool frameOpen(); // between a newFrame() and its render()
const hnui_target &lastTarget(); // of the last drawing render(); zero before
// throws ERR_IMGUI_NO_FRAME naming `imgui.<name>()`; returns nullptr
napi_value throwNoFrame(napi_env env, const char *name);
// The one context, created for this runtime's window if it does not exist
// yet, with error recovery configured (guard::configureContext()) before it
// returns. False, having done nothing, when there is no window.
bool ensureContext(napi_env env);
// platform
// The largest magnitude of a mouse coordinate and of a wheel delta the
// platform half gives ImGui, in logical units: 2^16, beyond any display
// (X11's own coordinates are 16-bit). ImGui converts a position to int as
// it is (ImFloor in AddMousePosEvent, imgui.cpp:2051), and a drag
// multiplies the difference of two by its speed before converting it
// (dragInt in bind/bind_widgets.cpp has the arithmetic).
constexpr float kMaxMouseCoordinate = 65536.0f;
void platformInit(hnui_window *w); // listener, clipboard hooks, backend flags
void platformNewFrame(
    hnui_window *w); // DisplaySize, DisplayFramebufferScale, DeltaTime
void platformAfterRender(hnui_window *w); // cursor
int lastCursorRequested(); // ImGuiMouseCursor last acted on, for state()
// renderer
void rendererInit(); // backend flags and checks; GPU objects wait for a draw
void rendererDraw(
    ImDrawData *dd,
    const hnui_target &target); // textures, pass, draws
struct RendererStats {
  int commands, vertices, indices, liveTextures;
};
RendererStats rendererStats(); // totals of the last rendererDraw
// test hooks
napi_value createTestHooks(napi_env env); // the `__test` object

} // namespace hnimgui

#endif // HERMES_NODE_IMGUI_ADDON_H
