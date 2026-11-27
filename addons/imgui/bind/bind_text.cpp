/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// The Text group (surface table, "Text"). Task 1 binds the part the core's
// tests need; Task 3 completes it. Every text function passes a finished
// string: ImGui never interprets a `%` in it.

#include "bind_util.h"

#include "imgui_addon.h"

namespace hnimgui {
namespace bind {

namespace {

/// `text(text)`: TextUnformatted, imgui.h:634.
napi_value text(napi_env env, napi_callback_info info) {
  Call c(env, info, "text");
  std::string s;
  if (!c.ok() || !c.string(0, "text", &s) || !c.needFrame())
    return nullptr;
  c.enterImGui();
  ImGui::TextUnformatted(s.c_str(), s.c_str() + s.size());
  if (!c.leaveImGui())
    return nullptr;
  return c.undefined();
}

} // namespace

bool registerText(napi_env env, napi_value exports) {
  return registerFunction(env, exports, "text", text);
}

} // namespace bind
} // namespace hnimgui
