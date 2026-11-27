/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// The Clipper group (surface table, "Clipper"): ImGuiListClipper bound as
// one function owning its loop, `listClipper(count, itemHeight, callback)`
// (design, "Exceptions: List clipper"). The clipper lives on this
// function's stack, so every way out of it, a normal end, an exception
// from the callback, or an abandoned scope, ends it here, and ImGui's stack
// of clipper data (g.ClipperTempDataStacked, imgui.cpp:3505-3507, 3527-3531)
// is as it was on entry.

#include "bind_guards.h"
#include "bind_util.h"

#include "imgui_addon.h"

#include <climits>

namespace hnimgui {
namespace bind {

namespace {

/// The smallest itemHeight. Step() converts the distance from the cursor
/// to each edge of the clip rectangle, divided by the height, to int
/// (`(int)(... / ItemsHeight)`, imgui.cpp:3692-3693); the distance is at
/// most the list's height plus the window's, so with a height of at least
/// 1 the quotient is at most the item count plus the window's height in
/// units, inside `int` under the next bound for any window below 2^30
/// units tall.
constexpr float kMinItemHeight = 1.0f;
/// The largest count * itemHeight, the list's height. End() seeks the
/// cursor to StartPosY + count * itemHeight (imgui.cpp:3549-3556, in
/// double, stored as float), and the window's content size is then
/// ImTrunc64 of that distance (imgui.cpp:7250), undefined at 2^63 or for
/// an infinity; 2^30 keeps both that and the quotient above (2^30 plus a
/// window below 2^30 units) defined, and is far above any list a window
/// shows (a million items of 17 units is 2^24).
constexpr double kMaxListHeight = 1073741824.0;

/// `listClipper(count, itemHeight, callback)`: Begin(count, itemHeight),
/// then the callback once per range Step() yields, `callback(start, end)`,
/// its return value ignored.
///
/// After each callback, before anything else touches the clipper: if the
/// window scope (guard::sameScope) is not what it was at entry, the
/// clipper is abandoned, released with ItemsCount set to -1 so that End()
/// does not seek the cursor of whatever window is now current
/// (imgui.cpp:3521-3522), and the callback's exception propagates or,
/// with none, ERR_IMGUI_USAGE is thrown. Otherwise, if the callback threw,
/// the clipper is ended normally and the exception propagates. Step() is
/// never called again after either.
napi_value listClipper(napi_env env, napi_callback_info info) {
  Call c(env, info, "listClipper");
  int count = 0;
  float itemHeight = 0.0f;
  // INT_MAX is ImGui's "unknown count" (imgui.h:3082), which needs
  // SeekCursorForItem(), not bound.
  if (!c.ok() || !c.intIn(0, "count", 0, INT_MAX - 1, &count) ||
      !c.f32(1, "itemHeight", &itemHeight))
    return nullptr;
  // A height of 0 or less asks ImGui to measure the first item
  // (imgui.h:3083), which is not bound; f32 has already refused NaN.
  if (!(itemHeight >= kMinItemHeight))
    return c.throwRange(
        "itemHeight must be at least 1; got %g",
        static_cast<double>(itemHeight));
  if (static_cast<double>(count) * itemHeight > kMaxListHeight)
    return c.throwRange(
        "count * itemHeight must be at most 1073741824; got %g",
        static_cast<double>(count) * itemHeight);
  if (c.typeOf(2) != napi_function)
    return c.throwType("ERR_INVALID_ARG_TYPE", "callback must be a function");
  if (!c.needFrame())
    return nullptr;
  if (const char *msg = guard::canListClipper())
    return c.throwUsage("%s", msg);

  napi_value callback = c.arg(2);
  const guard::ScopeSnapshot scope = guard::snapshotScope();
  c.enterImGui();
  ImGuiListClipper clipper;
  clipper.Begin(count, itemHeight);
  // A usage error Begin() or Step() reported would otherwise be dropped by
  // the next binding call's enterImGui(). None is known to be reachable
  // with a positive itemHeight (the measuring branch, which has the one
  // recoverable check, imgui.cpp:3620, is not entered); this keeps it so.
  if (!c.leaveImGui()) {
    clipper.End();
    return nullptr;
  }
  while (clipper.Step()) {
    if (!c.leaveImGui()) {
      clipper.End();
      return nullptr;
    }
    napi_value args[2] = {nullptr, nullptr};
    if (!c.napiOk(
            napi_create_int32(env, clipper.DisplayStart, &args[0]),
            "passing",
            "start") ||
        !c.napiOk(
            napi_create_int32(env, clipper.DisplayEnd, &args[1]),
            "passing",
            "end")) {
      clipper.End();
      return nullptr;
    }
    {
      CallbackScope inCallback;
      napi_value ignored = nullptr;
      // Its status says only what the pending-exception check below says.
      (void)napi_call_function(env, c.undefined(), callback, 2, args, &ignored);
    }
    bool pending = false;
    napi_is_exception_pending(env, &pending);
    if (!guard::sameScope(scope)) {
      clipper.ItemsCount = -1;
      clipper.End();
      trace("listClipper: abandoned, the callback changed the window scope");
      if (pending)
        return nullptr;
      return c.throwUsage(
          "the callback changed the window scope (it closed the clipper's "
          "window or table, or left one open); the clipper was abandoned");
    }
    if (pending) {
      clipper.End();
      return nullptr;
    }
  }
  if (!c.leaveImGui())
    return nullptr;
  return c.undefined();
}

} // namespace

bool registerClipper(napi_env env, napi_value exports) {
  return registerFunction(env, exports, "listClipper", listClipper);
}

} // namespace bind
} // namespace hnimgui
