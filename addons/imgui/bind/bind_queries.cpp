/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// The Item and input queries group (surface table, "Item and input
// queries"). Task 1 binds the part the core's tests need; Task 3 completes
// it.

#include "bind_util.h"

#include "imgui_addon.h"

namespace hnimgui {
namespace bind {

namespace {

/// `getItemRectMin(out?)`, imgui.h:1071.
napi_value getItemRectMin(napi_env env, napi_callback_info info) {
  Call c(env, info, "getItemRectMin");
  if (!c.ok() || !c.outArray(0, "out") || !c.needFrame())
    return nullptr;
  c.enterImGui();
  const ImVec2 v = ImGui::GetItemRectMin();
  if (!c.leaveImGui())
    return nullptr;
  return c.resultVec2(0, v);
}

/// `getItemRectMax(out?)`, imgui.h:1072.
napi_value getItemRectMax(napi_env env, napi_callback_info info) {
  Call c(env, info, "getItemRectMax");
  if (!c.ok() || !c.outArray(0, "out") || !c.needFrame())
    return nullptr;
  c.enterImGui();
  const ImVec2 v = ImGui::GetItemRectMax();
  if (!c.leaveImGui())
    return nullptr;
  return c.resultVec2(0, v);
}

} // namespace

bool registerQueries(napi_env env, napi_value exports) {
  return registerFunction(env, exports, "getItemRectMin", getItemRectMin) &&
      registerFunction(env, exports, "getItemRectMax", getItemRectMax);
}

} // namespace bind
} // namespace hnimgui
