/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// The `__test` object, exported only under HERMES_NODE_UI_TEST_HOOKS:
// `scene(name[, arg])` draws a fixed native scene into the open frame,
// `state()` reads back what ImGui and the renderer hold, `bindCalls()` how
// many times each binding function has called ImGui, and `guards()` how
// many times each guard condition has refused.

#include "imgui_addon.h"

#include "bind/bind_guards.h"
#include "bind/bind_util.h"

#include <sokol_gfx.h>

#include <cstdint>
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

/// The `quad` scene's view, made on its first use. It and its image live
/// as long as the process, like every other Sokol resource here.
sg_view gQuadView = {};

/// `quad`: a 2x2 image of the addon's own (red, green / blue, white, rows
/// top first) drawn with AddImage over (100,100)-(300,300), its view id as
/// the ImTextureID. The image is made on the scene's first use, which must
/// be inside a frame callback, where sg_* resource creation belongs (see
/// imgui_renderer.cpp); outside one this throws, and so does a failed
/// creation.
void sceneQuad(napi_env env) {
  if (gQuadView.id == SG_INVALID_ID) {
    hnui_target target = {};
    target.size = static_cast<uint32_t>(sizeof(target));
    if (!hnui_frame_target(hnui_get_window(env), &target)) {
      napi_throw_error(
          env,
          nullptr,
          "imgui.__test.scene('quad'): first use must be inside a frame "
          "callback");
      return;
    }
    // RGBA8, rows top first.
    static const uint8_t kPixels[2][2][4] = {
        {{255, 0, 0, 255}, {0, 255, 0, 255}},
        {{0, 0, 255, 255}, {255, 255, 255, 255}}};
    sg_image_desc img = {};
    img.width = 2;
    img.height = 2;
    img.pixel_format = SG_PIXELFORMAT_RGBA8;
    img.data.mip_levels[0].ptr = kPixels;
    img.data.mip_levels[0].size = sizeof(kPixels);
    img.label = "imgui-test-quad";
    // Image first, as in imgui_renderer.cpp: a view of an invalid or
    // failed image is a validation failure, which ends the process.
    const sg_image image = sg_make_image(&img);
    sg_view view = {};
    if (sg_query_image_state(image) == SG_RESOURCESTATE_VALID) {
      sg_view_desc vd = {};
      vd.texture.image = image;
      view = sg_make_view(&vd);
    }
    if (sg_query_view_state(view) != SG_RESOURCESTATE_VALID) {
      sg_destroy_view(view);
      sg_destroy_image(image);
      napi_throw_error(
          env, nullptr, "imgui.__test.scene('quad'): image creation failed");
      return;
    }
    gQuadView = view;
    trace("test quad image created (view %u)", view.id);
  }
  ImDrawList *list = beginScene();
  list->AddImage(
      static_cast<ImTextureID>(gQuadView.id),
      ImVec2(100.0f, 100.0f),
      ImVec2(300.0f, 300.0f));
  endScene(list);
}

/// `glyphs`: `abc` in white at (20,20) at the default size; with \p arg
/// non-zero also `XYZ` at (20,200) at 40 units, whose glyphs the font atlas
/// does not hold until they are first drawn, so ImGui updates or replaces
/// the atlas texture.
void sceneGlyphs(int arg) {
  ImDrawList *list = beginScene();
  list->AddText(ImVec2(20.0f, 20.0f), IM_COL32_WHITE, "abc");
  if (arg != 0) {
    ImGui::PushFont(nullptr, 40.0f);
    list->AddText(ImVec2(20.0f, 200.0f), IM_COL32_WHITE, "XYZ");
    ImGui::PopFont();
  }
  endScene(list);
}

/// What the `widgets` scene has seen, for state(). Accumulated across
/// frames and never reset: a test reads deltas.
struct WidgetsState {
  int clicks = 0;
  int keyAPresses = 0;
  bool keyADown = false;
  double wheel = 0;
  char text[256] = {};
  ImVec4 buttonRect{0, 0, 0, 0};
  ImVec4 fieldRect{0, 0, 0, 0};
};
WidgetsState gWidgets;

/// The last item's screen rectangle, as (x0, y0, x1, y1).
ImVec4 lastItemRect() {
  const ImVec2 min = ImGui::GetItemRectMin();
  const ImVec2 max = ImGui::GetItemRectMax();
  return ImVec4(min.x, min.y, max.x, max.y);
}

/// `widgets`: a window "T" at (50,50), 300x200, holding a 100x30 button "B"
/// that counts its clicks and a text field. Each frame also counts presses
/// of A (not repeats), reads whether A is down and adds up the wheel.
/// \p drawCursor, when given, is first stored in io.MouseDrawCursor (ImGui
/// draws the cursor itself when it is set); it stays as set for later
/// frames.
void sceneWidgets(const bool *drawCursor) {
  if (drawCursor != nullptr)
    ImGui::GetIO().MouseDrawCursor = *drawCursor;
  ImGui::SetNextWindowPos(ImVec2(50.0f, 50.0f), ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(300.0f, 200.0f), ImGuiCond_Always);
  ImGui::Begin(
      "T",
      nullptr,
      ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse);
  if (ImGui::Button("B", ImVec2(100.0f, 30.0f)))
    ++gWidgets.clicks;
  gWidgets.buttonRect = lastItemRect();
  ImGui::InputText("##t", gWidgets.text, sizeof(gWidgets.text));
  gWidgets.fieldRect = lastItemRect();
  ImGui::End();
  if (ImGui::IsKeyPressed(ImGuiKey_A, false))
    ++gWidgets.keyAPresses;
  gWidgets.keyADown = ImGui::IsKeyDown(ImGuiKey_A);
  gWidgets.wheel += ImGui::GetIO().MouseWheel;
}

/// ImGui's io.MouseWheel as the `wheel` scene last saw it non-zero; 0
/// until then. Never reset: a test compares it with the value it sent.
double gLastWheel = 0;

/// `wheel`: draws nothing; records this frame's io.MouseWheel in
/// gLastWheel when it is non-zero. ImGui clears the field when the frame
/// ends, so it is read here, inside the frame, which is what lets a test
/// see the wheel reach ImGui under any other drawing (the demo window's,
/// say) without the `widgets` scene.
void sceneWheel() {
  const float wheel = ImGui::GetIO().MouseWheel;
  if (wheel != 0.0f)
    gLastWheel = wheel;
}

/// `scene(name[, arg])`: draws immediately into the frame that is open, so
/// it is called between newFrame() and render(); with no frame open it
/// throws the no-frame error, as showDemoWindow() does. `arg` is optional:
/// an integer for the scenes that take one (default 0), or, for `widgets`
/// only, `softcursor` or `hardcursor`, which set or clear
/// io.MouseDrawCursor. Any other string is refused.
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
  char textArg[32] = {};
  napi_valuetype argType = napi_undefined;
  if (argc >= 2)
    napi_typeof(env, argv[1], &argType);
  if (argType == napi_string) {
    if (napi_get_value_string_utf8(
            env, argv[1], textArg, sizeof(textArg), &length) != napi_ok) {
      napi_throw_type_error(
          env, nullptr, "imgui.__test.scene(): unreadable string argument");
      return nullptr;
    }
  } else if (
      argType != napi_undefined &&
      napi_get_value_int32(env, argv[1], &arg) != napi_ok) {
    napi_throw_type_error(
        env,
        nullptr,
        "imgui.__test.scene(): the argument must be a number or a string");
    return nullptr;
  }
  if (argType == napi_string && std::strcmp(name, "widgets") != 0) {
    const std::string message = std::string("imgui.__test.scene('") + name +
        "'): the argument must be a number";
    napi_throw_type_error(env, nullptr, message.c_str());
    return nullptr;
  }
  // The one place the strings are read; '' is refused like any other.
  bool drawCursor = false;
  if (argType == napi_string) {
    if (std::strcmp(textArg, "softcursor") == 0) {
      drawCursor = true;
    } else if (std::strcmp(textArg, "hardcursor") != 0) {
      const std::string message =
          std::string("imgui.__test.scene('widgets'): unknown argument '") +
          textArg + "'";
      napi_throw_error(env, "ERR_INVALID_ARG_VALUE", message.c_str());
      return nullptr;
    }
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
  if (std::strcmp(name, "quad") == 0) {
    sceneQuad(env);
    return nullptr;
  }
  if (std::strcmp(name, "glyphs") == 0) {
    sceneGlyphs(arg);
    return nullptr;
  }
  if (std::strcmp(name, "wheel") == 0) {
    sceneWheel();
    return nullptr;
  }
  if (std::strcmp(name, "widgets") == 0) {
    sceneWidgets(argType == napi_string ? &drawCursor : nullptr);
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

bool setString(
    napi_env env,
    napi_value object,
    const char *name,
    const char *v) {
  napi_value value = nullptr;
  return napi_create_string_utf8(env, v, NAPI_AUTO_LENGTH, &value) == napi_ok &&
      napi_set_named_property(env, object, name, value) == napi_ok;
}

/// An array of \p count elements, each made by \p make(i, &value).
template <typename Make>
bool setArray(
    napi_env env,
    napi_value object,
    const char *name,
    uint32_t count,
    Make make) {
  napi_value array = nullptr;
  if (napi_create_array_with_length(env, count, &array) != napi_ok)
    return false;
  for (uint32_t i = 0; i < count; ++i) {
    napi_value value = nullptr;
    if (!make(i, &value) || napi_set_element(env, array, i, value) != napi_ok)
      return false;
  }
  return napi_set_named_property(env, object, name, array) == napi_ok;
}

/// `[x0, y0, x1, y1]`.
bool setRect(napi_env env, napi_value object, const char *name, ImVec4 r) {
  const float v[4] = {r.x, r.y, r.z, r.w};
  return setArray(env, object, name, 4, [&](uint32_t i, napi_value *out) {
    return napi_create_double(env, v[i], out) == napi_ok;
  });
}

bool setBool(napi_env env, napi_value object, const char *name, bool v) {
  napi_value value = nullptr;
  return napi_get_boolean(env, v, &value) == napi_ok &&
      napi_set_named_property(env, object, name, value) == napi_ok;
}

/// `windows`: `[{name, active, docked}]`, every window ImGui knows, as of
/// the last ended frame.
bool setWindows(napi_env env, napi_value object) {
  const std::vector<guard::WindowInfo> windows = guard::windowsOfLastFrame();
  return setArray(
      env,
      object,
      "windows",
      static_cast<uint32_t>(windows.size()),
      [&](uint32_t i, napi_value *out) {
        return napi_create_object(env, out) == napi_ok &&
            setString(env, *out, "name", windows[i].name.c_str()) &&
            setBool(env, *out, "active", windows[i].active) &&
            setBool(env, *out, "docked", windows[i].docked);
      });
}

/// `state()`: as of the last completed newFrame()/render() pair, except
/// `frameOpen`, which is now. Before the context exists the ImGui fields
/// read as zero and `windows` is empty.
napi_value state(napi_env env, napi_callback_info) {
  napi_value result = nullptr;
  if (napi_create_object(env, &result) != napi_ok)
    return nullptr;

  ImVec2 display(0.0f, 0.0f);
  ImVec2 scale(0.0f, 0.0f);
  ImVec2 mouse(0.0f, 0.0f);
  bool mouseDown[3] = {false, false, false};
  bool wantCaptureMouse = false;
  bool wantCaptureKeyboard = false;
  bool wantTextInput = false;
  double atlasTexId = 0;
  if (ImGui::GetCurrentContext() != nullptr) {
    const ImGuiIO &io = ImGui::GetIO();
    // The id the renderer handed ImGui for the font atlas's texture (its
    // Sokol view id), 0 before the atlas is uploaded. A texture re-created
    // gets a new view, and so a new id.
    if (io.Fonts != nullptr && io.Fonts->TexData != nullptr)
      atlasTexId = static_cast<double>(io.Fonts->TexData->GetTexID());
    display = io.DisplaySize;
    scale = io.DisplayFramebufferScale;
    mouse = io.MousePos;
    for (int i = 0; i < 3; ++i)
      mouseDown[i] = io.MouseDown[i];
    wantCaptureMouse = io.WantCaptureMouse;
    wantCaptureKeyboard = io.WantCaptureKeyboard;
    wantTextInput = io.WantTextInput;
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
      setNumber(env, result, "liveTextures", stats.liveTextures) &&
      setNumber(env, result, "atlasTexId", atlasTexId) &&
      setNumber(env, result, "mouseX", mouse.x) &&
      setNumber(env, result, "mouseY", mouse.y) &&
      setArray(env,
               result,
               "mouseDown",
               3,
               [&](uint32_t i, napi_value *out) {
                 return napi_get_boolean(env, mouseDown[i], out) == napi_ok;
               }) &&
      setBool(env, result, "wantCaptureMouse", wantCaptureMouse) &&
      setBool(env, result, "wantCaptureKeyboard", wantCaptureKeyboard) &&
      setBool(env, result, "wantTextInput", wantTextInput) &&
      setNumber(env, result, "clicks", gWidgets.clicks) &&
      setNumber(env, result, "keyAPresses", gWidgets.keyAPresses) &&
      setBool(env, result, "keyADown", gWidgets.keyADown) &&
      setString(env, result, "text", gWidgets.text) &&
      setNumber(env, result, "wheel", gWidgets.wheel) &&
      setNumber(env, result, "lastWheel", gLastWheel) &&
      setRect(env, result, "buttonRect", gWidgets.buttonRect) &&
      setRect(env, result, "fieldRect", gWidgets.fieldRect) &&
      setNumber(env, result, "cursor", lastCursorRequested()) &&
      setWindows(env, result);
  return ok ? result : nullptr;
}

/// An object of name -> count.
napi_value countsObject(
    napi_env env,
    const std::vector<std::pair<std::string, int>> &counts) {
  napi_value result = nullptr;
  if (napi_create_object(env, &result) != napi_ok)
    return nullptr;
  for (const auto &entry : counts) {
    if (!setNumber(env, result, entry.first.c_str(), entry.second))
      return nullptr;
  }
  return result;
}

/// `bindCalls()`: binding function name -> how many times it has called
/// ImGui. A function never called is absent.
napi_value bindCalls(napi_env env, napi_callback_info) {
  return countsObject(env, bind::bindCalls());
}

/// `guards()`: guard condition name -> how many times it has refused.
/// Every registered condition is present, refused or not.
napi_value guards(napi_env env, napi_callback_info) {
  return countsObject(env, guard::guardRefusals());
}

/// `setDoubleClickTime(seconds)`: sets io.MouseDoubleClickTime and returns
/// the previous value, so that a test of a double click does not depend on
/// how long its frames take (ImGui measures the interval in its own time,
/// which advances by io.DeltaTime from the real clock). It also sets
/// io.MouseSingleClickDelay to `seconds + 0.2`, ImGui's default gap
/// (0.30 / 0.50, imgui.cpp:1818-1820), since NewFrame asserts plainly that
/// the delay is the larger (imgui.cpp:11854); restoring 0.3 restores both
/// defaults. Creates the context as the binding functions do;
/// ERR_IMGUI_NO_WINDOW without a window.
napi_value setDoubleClickTime(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  napi_valuetype type = napi_undefined;
  double seconds = 0;
  if (argc < 1 || napi_typeof(env, argv[0], &type) != napi_ok ||
      type != napi_number ||
      napi_get_value_double(env, argv[0], &seconds) != napi_ok ||
      !(seconds > 0 && seconds <= 1e6)) {
    napi_throw_range_error(
        env,
        "ERR_OUT_OF_RANGE",
        "imgui.__test.setDoubleClickTime(): seconds must be in (0, 1e6]");
    return nullptr;
  }
  if (!ensureContext(env)) {
    napi_throw_error(
        env,
        "ERR_IMGUI_NO_WINDOW",
        "imgui.__test.setDoubleClickTime(): no window; call "
        "ui.createWindow() first");
    return nullptr;
  }
  ImGuiIO &io = ImGui::GetIO();
  const double previous = io.MouseDoubleClickTime;
  io.MouseDoubleClickTime = static_cast<float>(seconds);
  io.MouseSingleClickDelay = static_cast<float>(seconds + 0.2);
  napi_value result = nullptr;
  if (napi_create_double(env, previous, &result) != napi_ok)
    return nullptr;
  return result;
}

/// `setTooltipDelay(seconds)`: sets style.HoverStationaryDelay and
/// style.HoverDelayShort, the two delays of the hover check that
/// setItemTooltip() makes with style.HoverFlagsForTooltipMouse
/// (`Stationary | DelayShort`, imgui.cpp:1660, 5199-5204, 5991-5992), and
/// returns the previous HoverDelayShort. Both are measured in ImGui's time,
/// which advances by io.DeltaTime from the real clock, so with 0 a tooltip
/// test does not depend on how long its frames take. Both default to 0.15
/// (imgui.cpp:1657-1658), so restoring 0.15 restores both. Creates the
/// context as the binding functions do; ERR_IMGUI_NO_WINDOW without a
/// window.
napi_value setTooltipDelay(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  napi_valuetype type = napi_undefined;
  double seconds = 0;
  if (argc < 1 || napi_typeof(env, argv[0], &type) != napi_ok ||
      type != napi_number ||
      napi_get_value_double(env, argv[0], &seconds) != napi_ok ||
      !(seconds >= 0 && seconds <= 1e6)) {
    napi_throw_range_error(
        env,
        "ERR_OUT_OF_RANGE",
        "imgui.__test.setTooltipDelay(): seconds must be in [0, 1e6]");
    return nullptr;
  }
  if (!ensureContext(env)) {
    napi_throw_error(
        env,
        "ERR_IMGUI_NO_WINDOW",
        "imgui.__test.setTooltipDelay(): no window; call "
        "ui.createWindow() first");
    return nullptr;
  }
  ImGuiStyle &style = ImGui::GetStyle();
  const double previous = style.HoverDelayShort;
  style.HoverStationaryDelay = static_cast<float>(seconds);
  style.HoverDelayShort = static_cast<float>(seconds);
  napi_value result = nullptr;
  if (napi_create_double(env, previous, &result) != napi_ok)
    return nullptr;
  return result;
}

} // namespace

napi_value createTestHooks(napi_env env) {
  napi_value hooks = nullptr;
  if (napi_create_object(env, &hooks) != napi_ok)
    return nullptr;
  const struct {
    const char *name;
    napi_callback cb;
  } functions[] = {
      {"scene", scene},
      {"state", state},
      {"bindCalls", bindCalls},
      {"guards", guards},
      {"setDoubleClickTime", setDoubleClickTime},
      {"setTooltipDelay", setTooltipDelay}};
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
