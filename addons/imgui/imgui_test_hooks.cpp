/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// The `__test` object, exported only under HERMES_NODE_UI_TEST_HOOKS:
// `scene(name[, arg])` draws a fixed native scene into the open frame, and
// `state()` reads back what ImGui and the renderer hold.

#include "imgui_addon.h"

#include <sokol_gfx.h>

#include <cstring>
#include <string>

namespace hnimgui {

namespace {

/// Every scene draws on the background draw list with anti-aliased fill
/// off, so a rectangle has no feathered fringe and its edges are exactly
/// its coordinates. (1.93 merged ImDrawListFlags into ImDrawFlags;
/// ImDrawListFlags_AntiAliasedFill is ImDrawFlags_AAFill now, scoped with
/// PushDrawFlag/PopDrawFlag.)
ImDrawList *beginScene() {
  ImDrawList *list = ImGui::GetBackgroundDrawList();
  list->PushDrawFlag(ImDrawFlags_AAFill, false);
  return list;
}

void endScene(ImDrawList *list) {
  list->PopDrawFlag();
}

/// Fill the whole display with \p color.
void fillDisplay(ImDrawList *list, ImU32 color) {
  list->AddRectFilled(ImVec2(0.0f, 0.0f), ImGui::GetIO().DisplaySize, color);
}

/// `fill`: a full-window red fill.
void sceneFill() {
  ImDrawList *list = beginScene();
  fillDisplay(list, IM_COL32(255, 0, 0, 255));
  endScene(list);
}

/// `clip`: a green fill of the whole display under a clip rectangle
/// (100,100)-(300,200), so only that rectangle is drawn.
void sceneClip() {
  ImDrawList *list = beginScene();
  list->PushClipRect(ImVec2(100.0f, 100.0f), ImVec2(300.0f, 200.0f), true);
  fillDisplay(list, IM_COL32(0, 255, 0, 255));
  list->PopClipRect();
  endScene(list);
}

/// `many`: 4x4 quads tiling a 640x480 display, 160 columns by 120 rows,
/// 76,800 vertices in one list: past 16-bit indices, so ImGui splits the
/// list into commands with non-zero vertex offsets, and past the
/// renderer's initial buffer sizes. Quad (c, r) is red, green or blue for
/// (c + r) % 3 = 0, 1, 2.
void sceneMany() {
  static const ImU32 kColors[3] = {
      IM_COL32(255, 0, 0, 255),
      IM_COL32(0, 255, 0, 255),
      IM_COL32(0, 0, 255, 255)};
  ImDrawList *list = beginScene();
  for (int r = 0; r < 120; ++r) {
    for (int c = 0; c < 160; ++c) {
      const ImVec2 min(4.0f * c, 4.0f * r);
      list->AddRectFilled(
          min, ImVec2(min.x + 4.0f, min.y + 4.0f), kColors[(c + r) % 3]);
    }
  }
  endScene(list);
}

/// `two`: a full-window fill, red for arg 0 and blue for any other.
void sceneTwo(int arg) {
  ImDrawList *list = beginScene();
  fillDisplay(
      list, arg == 0 ? IM_COL32(255, 0, 0, 255) : IM_COL32(0, 0, 255, 255));
  endScene(list);
}

/// A user callback that shrinks the viewport to the top-left quarter of
/// the framebuffer, which only a reset of the render state undoes.
void quarterViewport(const ImDrawList *, const ImDrawCmd *) {
  const ImDrawData *dd = ImGui::GetDrawData();
  const int fbWidth =
      static_cast<int>(dd->DisplaySize.x * dd->FramebufferScale.x);
  const int fbHeight =
      static_cast<int>(dd->DisplaySize.y * dd->FramebufferScale.y);
  sg_apply_viewport(0, 0, fbWidth / 4, fbHeight / 4, true);
}

/// `callback`: the quarter-viewport callback, then ImGui's reset callback,
/// then a full-window green fill, which covers the whole window only if
/// the reset re-applied the viewport. With \p arg non-zero the reset is
/// left out, so the fill lands in the top-left quarter only if the user
/// callback was called.
void sceneCallback(int arg) {
  ImDrawList *list = beginScene();
  list->AddCallback(quarterViewport, nullptr);
  if (arg == 0)
    list->AddCallback(
        ImGui::GetPlatformIO().DrawCallback_ResetRenderState, nullptr);
  fillDisplay(list, IM_COL32(0, 255, 0, 255));
  endScene(list);
}

/// `scene(name[, arg])`: draws immediately into the frame that is open, so
/// it is called between newFrame() and render(); with no frame open it
/// throws the no-frame error, as showDemoWindow() does. `arg` is an
/// optional integer some scenes take; it defaults to 0.
napi_value scene(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2] = {nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  char name[32] = {};
  size_t length = 0;
  if (argc < 1 ||
      napi_get_value_string_utf8(env, argv[0], name, sizeof(name), &length) !=
          napi_ok) {
    napi_throw_type_error(
        env, nullptr, "imgui.__test.scene(): a scene name is required");
    return nullptr;
  }
  int32_t arg = 0;
  napi_valuetype argType = napi_undefined;
  if (argc >= 2)
    napi_typeof(env, argv[1], &argType);
  if (argType != napi_undefined &&
      napi_get_value_int32(env, argv[1], &arg) != napi_ok) {
    napi_throw_type_error(
        env, nullptr, "imgui.__test.scene(): the argument must be a number");
    return nullptr;
  }
  if (!frameOpen())
    return throwNoFrame(env, "__test.scene");

  if (std::strcmp(name, "none") == 0)
    return nullptr;
  if (std::strcmp(name, "fill") == 0) {
    sceneFill();
    return nullptr;
  }
  if (std::strcmp(name, "clip") == 0) {
    sceneClip();
    return nullptr;
  }
  if (std::strcmp(name, "many") == 0) {
    sceneMany();
    return nullptr;
  }
  if (std::strcmp(name, "two") == 0) {
    sceneTwo(arg);
    return nullptr;
  }
  if (std::strcmp(name, "callback") == 0) {
    sceneCallback(arg);
    return nullptr;
  }
  const std::string message =
      std::string("imgui.__test.scene(): unknown scene '") + name + "'";
  napi_throw_error(env, "ERR_INVALID_ARG_VALUE", message.c_str());
  return nullptr;
}

bool setNumber(napi_env env, napi_value object, const char *name, double v) {
  napi_value value = nullptr;
  return napi_create_double(env, v, &value) == napi_ok &&
      napi_set_named_property(env, object, name, value) == napi_ok;
}

bool setBool(napi_env env, napi_value object, const char *name, bool v) {
  napi_value value = nullptr;
  return napi_get_boolean(env, v, &value) == napi_ok &&
      napi_set_named_property(env, object, name, value) == napi_ok;
}

/// `state()`: as of the last completed newFrame()/render() pair, except
/// `frameOpen`, which is now. Before the context exists the ImGui fields
/// read as zero.
napi_value state(napi_env env, napi_callback_info) {
  napi_value result = nullptr;
  if (napi_create_object(env, &result) != napi_ok)
    return nullptr;

  ImVec2 display(0.0f, 0.0f);
  ImVec2 scale(0.0f, 0.0f);
  if (ImGui::GetCurrentContext() != nullptr) {
    const ImGuiIO &io = ImGui::GetIO();
    display = io.DisplaySize;
    scale = io.DisplayFramebufferScale;
  }
  const hnui_target &target = lastTarget();
  const RendererStats stats = rendererStats();

  const bool ok = setBool(env, result, "frameOpen", frameOpen()) &&
      setNumber(env, result, "displayWidth", display.x) &&
      setNumber(env, result, "displayHeight", display.y) &&
      setNumber(env, result, "scaleX", scale.x) &&
      setNumber(env, result, "scaleY", scale.y) &&
      setNumber(env, result, "targetGeneration", target.generation) &&
      setNumber(env, result, "targetWidth", target.width) &&
      setNumber(env, result, "targetHeight", target.height) &&
      setNumber(env, result, "commands", stats.commands) &&
      setNumber(env, result, "vertices", stats.vertices) &&
      setNumber(env, result, "indices", stats.indices) &&
      setNumber(env, result, "liveTextures", stats.liveTextures);
  return ok ? result : nullptr;
}

} // namespace

napi_value createTestHooks(napi_env env) {
  napi_value hooks = nullptr;
  if (napi_create_object(env, &hooks) != napi_ok)
    return nullptr;
  const struct {
    const char *name;
    napi_callback cb;
  } functions[] = {{"scene", scene}, {"state", state}};
  for (const auto &f : functions) {
    napi_value fn = nullptr;
    if (napi_create_function(
            env, f.name, NAPI_AUTO_LENGTH, f.cb, nullptr, &fn) != napi_ok ||
        napi_set_named_property(env, hooks, f.name, fn) != napi_ok)
      return nullptr;
  }
  return hooks;
}

} // namespace hnimgui
