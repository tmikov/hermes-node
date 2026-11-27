/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// The `__test` object, exported only under HERMES_NODE_UI_TEST_HOOKS:
// `scene(name)` draws a fixed native scene into the open frame, and
// `state()` reads back what ImGui and the renderer hold.

#include "imgui_addon.h"

#include <cstring>
#include <string>

namespace hnimgui {

namespace {

/// A full-window flat fill through the background draw list. Anti-aliased
/// fill is off for the call, so the rectangle has no feathered fringe and
/// its edges are the window's. (1.93 merged ImDrawListFlags into
/// ImDrawFlags; ImDrawListFlags_AntiAliasedFill is ImDrawFlags_AAFill now,
/// scoped with PushDrawFlag/PopDrawFlag.)
void sceneFill() {
  ImDrawList *list = ImGui::GetBackgroundDrawList();
  list->PushDrawFlag(ImDrawFlags_AAFill, false);
  list->AddRectFilled(
      ImVec2(0.0f, 0.0f), ImGui::GetIO().DisplaySize, IM_COL32(255, 0, 0, 255));
  list->PopDrawFlag();
}

/// `scene(name)`: draws immediately into the frame that is open, so it is
/// called between newFrame() and render(); with no frame open it throws
/// the no-frame error, as showDemoWindow() does.
napi_value scene(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
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
  if (!frameOpen())
    return throwNoFrame(env, "__test.scene");

  if (std::strcmp(name, "none") == 0)
    return nullptr;
  if (std::strcmp(name, "fill") == 0) {
    sceneFill();
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
