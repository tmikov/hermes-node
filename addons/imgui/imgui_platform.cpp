/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// The platform half of the backend: what ImGui's GLFW or SDL backend does,
// on hnui. The display size, the framebuffer scale and the time step, the
// input events, the mouse cursor and the clipboard.

#include "imgui_addon.h"

#include <cfloat>
#include <chrono>
#include <cstring>
#include <string>

namespace hnimgui {

namespace {

/// The window platformInit() was given, for the clipboard functions, which
/// ImGui calls with only its context.
hnui_window *gWindow = nullptr;

/// The clock reading of the previous newFrame(); unset before the first.
bool gHaveLastFrameTime = false;
std::chrono::steady_clock::time_point gLastFrameTime;

/// The ImGui key for an hnui `key_code`. hnui's numbering is Sokol's,
/// which is GLFW's, so this is ImGui_ImplGlfw_KeyToImGuiKey
/// (backends/imgui_impl_glfw.cpp, ImGui v1.93.0-docking) case for case,
/// with each GLFW_KEY_* name replaced by its value; the addon includes no
/// GLFW header. ImGuiKey_None for a key ImGui has no name for.
ImGuiKey translateKey(int keyCode) {
  switch (keyCode) {
    case 258: // GLFW_KEY_TAB
      return ImGuiKey_Tab;
    case 263: // GLFW_KEY_LEFT
      return ImGuiKey_LeftArrow;
    case 262: // GLFW_KEY_RIGHT
      return ImGuiKey_RightArrow;
    case 265: // GLFW_KEY_UP
      return ImGuiKey_UpArrow;
    case 264: // GLFW_KEY_DOWN
      return ImGuiKey_DownArrow;
    case 266: // GLFW_KEY_PAGE_UP
      return ImGuiKey_PageUp;
    case 267: // GLFW_KEY_PAGE_DOWN
      return ImGuiKey_PageDown;
    case 268: // GLFW_KEY_HOME
      return ImGuiKey_Home;
    case 269: // GLFW_KEY_END
      return ImGuiKey_End;
    case 260: // GLFW_KEY_INSERT
      return ImGuiKey_Insert;
    case 261: // GLFW_KEY_DELETE
      return ImGuiKey_Delete;
    case 259: // GLFW_KEY_BACKSPACE
      return ImGuiKey_Backspace;
    case 32: // GLFW_KEY_SPACE
      return ImGuiKey_Space;
    case 257: // GLFW_KEY_ENTER
      return ImGuiKey_Enter;
    case 256: // GLFW_KEY_ESCAPE
      return ImGuiKey_Escape;
    case 39: // GLFW_KEY_APOSTROPHE
      return ImGuiKey_Apostrophe;
    case 44: // GLFW_KEY_COMMA
      return ImGuiKey_Comma;
    case 45: // GLFW_KEY_MINUS
      return ImGuiKey_Minus;
    case 46: // GLFW_KEY_PERIOD
      return ImGuiKey_Period;
    case 47: // GLFW_KEY_SLASH
      return ImGuiKey_Slash;
    case 59: // GLFW_KEY_SEMICOLON
      return ImGuiKey_Semicolon;
    case 61: // GLFW_KEY_EQUAL
      return ImGuiKey_Equal;
    case 91: // GLFW_KEY_LEFT_BRACKET
      return ImGuiKey_LeftBracket;
    case 92: // GLFW_KEY_BACKSLASH
      return ImGuiKey_Backslash;
    case 161: // GLFW_KEY_WORLD_1
      return ImGuiKey_Oem102;
    case 162: // GLFW_KEY_WORLD_2
      return ImGuiKey_Oem102;
    case 93: // GLFW_KEY_RIGHT_BRACKET
      return ImGuiKey_RightBracket;
    case 96: // GLFW_KEY_GRAVE_ACCENT
      return ImGuiKey_GraveAccent;
    case 280: // GLFW_KEY_CAPS_LOCK
      return ImGuiKey_CapsLock;
    case 281: // GLFW_KEY_SCROLL_LOCK
      return ImGuiKey_ScrollLock;
    case 282: // GLFW_KEY_NUM_LOCK
      return ImGuiKey_NumLock;
    case 283: // GLFW_KEY_PRINT_SCREEN
      return ImGuiKey_PrintScreen;
    case 284: // GLFW_KEY_PAUSE
      return ImGuiKey_Pause;
    case 320: // GLFW_KEY_KP_0
      return ImGuiKey_Keypad0;
    case 321: // GLFW_KEY_KP_1
      return ImGuiKey_Keypad1;
    case 322: // GLFW_KEY_KP_2
      return ImGuiKey_Keypad2;
    case 323: // GLFW_KEY_KP_3
      return ImGuiKey_Keypad3;
    case 324: // GLFW_KEY_KP_4
      return ImGuiKey_Keypad4;
    case 325: // GLFW_KEY_KP_5
      return ImGuiKey_Keypad5;
    case 326: // GLFW_KEY_KP_6
      return ImGuiKey_Keypad6;
    case 327: // GLFW_KEY_KP_7
      return ImGuiKey_Keypad7;
    case 328: // GLFW_KEY_KP_8
      return ImGuiKey_Keypad8;
    case 329: // GLFW_KEY_KP_9
      return ImGuiKey_Keypad9;
    case 330: // GLFW_KEY_KP_DECIMAL
      return ImGuiKey_KeypadDecimal;
    case 331: // GLFW_KEY_KP_DIVIDE
      return ImGuiKey_KeypadDivide;
    case 332: // GLFW_KEY_KP_MULTIPLY
      return ImGuiKey_KeypadMultiply;
    case 333: // GLFW_KEY_KP_SUBTRACT
      return ImGuiKey_KeypadSubtract;
    case 334: // GLFW_KEY_KP_ADD
      return ImGuiKey_KeypadAdd;
    case 335: // GLFW_KEY_KP_ENTER
      return ImGuiKey_KeypadEnter;
    case 336: // GLFW_KEY_KP_EQUAL
      return ImGuiKey_KeypadEqual;
    case 340: // GLFW_KEY_LEFT_SHIFT
      return ImGuiKey_LeftShift;
    case 341: // GLFW_KEY_LEFT_CONTROL
      return ImGuiKey_LeftCtrl;
    case 342: // GLFW_KEY_LEFT_ALT
      return ImGuiKey_LeftAlt;
    case 343: // GLFW_KEY_LEFT_SUPER
      return ImGuiKey_LeftSuper;
    case 344: // GLFW_KEY_RIGHT_SHIFT
      return ImGuiKey_RightShift;
    case 345: // GLFW_KEY_RIGHT_CONTROL
      return ImGuiKey_RightCtrl;
    case 346: // GLFW_KEY_RIGHT_ALT
      return ImGuiKey_RightAlt;
    case 347: // GLFW_KEY_RIGHT_SUPER
      return ImGuiKey_RightSuper;
    case 348: // GLFW_KEY_MENU
      return ImGuiKey_Menu;
    case 48: // GLFW_KEY_0
      return ImGuiKey_0;
    case 49: // GLFW_KEY_1
      return ImGuiKey_1;
    case 50: // GLFW_KEY_2
      return ImGuiKey_2;
    case 51: // GLFW_KEY_3
      return ImGuiKey_3;
    case 52: // GLFW_KEY_4
      return ImGuiKey_4;
    case 53: // GLFW_KEY_5
      return ImGuiKey_5;
    case 54: // GLFW_KEY_6
      return ImGuiKey_6;
    case 55: // GLFW_KEY_7
      return ImGuiKey_7;
    case 56: // GLFW_KEY_8
      return ImGuiKey_8;
    case 57: // GLFW_KEY_9
      return ImGuiKey_9;
    case 65: // GLFW_KEY_A
      return ImGuiKey_A;
    case 66: // GLFW_KEY_B
      return ImGuiKey_B;
    case 67: // GLFW_KEY_C
      return ImGuiKey_C;
    case 68: // GLFW_KEY_D
      return ImGuiKey_D;
    case 69: // GLFW_KEY_E
      return ImGuiKey_E;
    case 70: // GLFW_KEY_F
      return ImGuiKey_F;
    case 71: // GLFW_KEY_G
      return ImGuiKey_G;
    case 72: // GLFW_KEY_H
      return ImGuiKey_H;
    case 73: // GLFW_KEY_I
      return ImGuiKey_I;
    case 74: // GLFW_KEY_J
      return ImGuiKey_J;
    case 75: // GLFW_KEY_K
      return ImGuiKey_K;
    case 76: // GLFW_KEY_L
      return ImGuiKey_L;
    case 77: // GLFW_KEY_M
      return ImGuiKey_M;
    case 78: // GLFW_KEY_N
      return ImGuiKey_N;
    case 79: // GLFW_KEY_O
      return ImGuiKey_O;
    case 80: // GLFW_KEY_P
      return ImGuiKey_P;
    case 81: // GLFW_KEY_Q
      return ImGuiKey_Q;
    case 82: // GLFW_KEY_R
      return ImGuiKey_R;
    case 83: // GLFW_KEY_S
      return ImGuiKey_S;
    case 84: // GLFW_KEY_T
      return ImGuiKey_T;
    case 85: // GLFW_KEY_U
      return ImGuiKey_U;
    case 86: // GLFW_KEY_V
      return ImGuiKey_V;
    case 87: // GLFW_KEY_W
      return ImGuiKey_W;
    case 88: // GLFW_KEY_X
      return ImGuiKey_X;
    case 89: // GLFW_KEY_Y
      return ImGuiKey_Y;
    case 90: // GLFW_KEY_Z
      return ImGuiKey_Z;
    case 290: // GLFW_KEY_F1
      return ImGuiKey_F1;
    case 291: // GLFW_KEY_F2
      return ImGuiKey_F2;
    case 292: // GLFW_KEY_F3
      return ImGuiKey_F3;
    case 293: // GLFW_KEY_F4
      return ImGuiKey_F4;
    case 294: // GLFW_KEY_F5
      return ImGuiKey_F5;
    case 295: // GLFW_KEY_F6
      return ImGuiKey_F6;
    case 296: // GLFW_KEY_F7
      return ImGuiKey_F7;
    case 297: // GLFW_KEY_F8
      return ImGuiKey_F8;
    case 298: // GLFW_KEY_F9
      return ImGuiKey_F9;
    case 299: // GLFW_KEY_F10
      return ImGuiKey_F10;
    case 300: // GLFW_KEY_F11
      return ImGuiKey_F11;
    case 301: // GLFW_KEY_F12
      return ImGuiKey_F12;
    case 302: // GLFW_KEY_F13
      return ImGuiKey_F13;
    case 303: // GLFW_KEY_F14
      return ImGuiKey_F14;
    case 304: // GLFW_KEY_F15
      return ImGuiKey_F15;
    case 305: // GLFW_KEY_F16
      return ImGuiKey_F16;
    case 306: // GLFW_KEY_F17
      return ImGuiKey_F17;
    case 307: // GLFW_KEY_F18
      return ImGuiKey_F18;
    case 308: // GLFW_KEY_F19
      return ImGuiKey_F19;
    case 309: // GLFW_KEY_F20
      return ImGuiKey_F20;
    case 310: // GLFW_KEY_F21
      return ImGuiKey_F21;
    case 311: // GLFW_KEY_F22
      return ImGuiKey_F22;
    case 312: // GLFW_KEY_F23
      return ImGuiKey_F23;
    case 313: // GLFW_KEY_F24
      return ImGuiKey_F24;
    default:
      return ImGuiKey_None;
  }
}

/// ImGui's mouse button for an hnui one: hnui numbers left, middle, right
/// as the DOM does (0, 1, 2), ImGui left, right, middle. -1 for any other.
int translateButton(int button) {
  switch (button) {
    case HNUI_BUTTON_LEFT:
      return ImGuiMouseButton_Left;
    case HNUI_BUTTON_MIDDLE:
      return ImGuiMouseButton_Middle;
    case HNUI_BUTTON_RIGHT:
      return ImGuiMouseButton_Right;
    default:
      return -1;
  }
}

/// The state of the four modifiers, from an event's `modifiers` bits.
void addModifiers(ImGuiIO &io, int modifiers) {
  io.AddKeyEvent(ImGuiMod_Ctrl, (modifiers & HNUI_MOD_CTRL) != 0);
  io.AddKeyEvent(ImGuiMod_Shift, (modifiers & HNUI_MOD_SHIFT) != 0);
  io.AddKeyEvent(ImGuiMod_Alt, (modifiers & HNUI_MOD_ALT) != 0);
  io.AddKeyEvent(ImGuiMod_Super, (modifiers & HNUI_MOD_META) != 0);
}

/// \p v clamped to +-kMaxMouseCoordinate, traced when it was outside;
/// \p what names it in the trace.
float clampedInput(double v, const char *what) {
  const double limit = kMaxMouseCoordinate;
  if (v >= -limit && v <= limit)
    return static_cast<float>(v);
  const float clamped = v > limit ? kMaxMouseCoordinate
      : v < -limit                ? -kMaxMouseCoordinate
                                  : 0.0f; // NaN
  trace("input: %s %g clamped to %g", what, v, static_cast<double>(clamped));
  return clamped;
}

/// The one event listener: every hnui event, translated into ImGui's input
/// queue. ImGui applies the queue at the next NewFrame(), trickling it over
/// several frames where one frame would lose a press-and-release.
void onEvent(hnui_window *, void *, const hnui_event *event) {
  if (!hnui_size_ok(event->size, HNUI_EVENT_MIN_SIZE)) {
    // Every event from a host this old is too small, so once is enough.
    static bool traced = false;
    if (!traced) {
      traced = true;
      trace("event rejected: size %u", event->size);
    }
    return;
  }
  ImGuiIO &io = ImGui::GetIO();
  switch (static_cast<hnui_event_type>(event->type)) {
    case HNUI_EVENT_MOUSE_MOVE: {
      // Two statements, so that the traces come in this order.
      const float x = clampedInput(event->x, "mouse x");
      const float y = clampedInput(event->y, "mouse y");
      io.AddMousePosEvent(x, y);
      break;
    }
    case HNUI_EVENT_MOUSE_DOWN:
    case HNUI_EVENT_MOUSE_UP: {
      addModifiers(io, event->modifiers);
      const float x = clampedInput(event->x, "mouse x");
      const float y = clampedInput(event->y, "mouse y");
      io.AddMousePosEvent(x, y);
      const int button = translateButton(event->button);
      if (button >= 0)
        io.AddMouseButtonEvent(button, event->type == HNUI_EVENT_MOUSE_DOWN);
      break;
    }
    case HNUI_EVENT_MOUSE_WHEEL: {
      // A wheel delta times the scroll step becomes the scroll target,
      // which CalcNextScrollFromScrollTargetAndClamp converts to an ImS64
      // (ImRoundPositive64, imgui.cpp:12815-12817).
      const float dx = clampedInput(event->dx, "wheel x");
      const float dy = clampedInput(event->dy, "wheel y");
      io.AddMouseWheelEvent(dx, dy);
      break;
    }
    case HNUI_EVENT_MOUSE_LEAVE:
      io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
      break;
    case HNUI_EVENT_KEY_DOWN:
    case HNUI_EVENT_KEY_UP: {
      addModifiers(io, event->modifiers);
      // ImGui does its own key repeat, from the time a key has been down.
      if (event->type == HNUI_EVENT_KEY_DOWN && event->repeat != 0)
        break;
      const ImGuiKey key = translateKey(event->key_code);
      if (key != ImGuiKey_None)
        io.AddKeyEvent(key, event->type == HNUI_EVENT_KEY_DOWN);
      break;
    }
    case HNUI_EVENT_TEXT_INPUT:
      // A repeated character is a real one: holding a key types it again.
      if (event->text != nullptr)
        io.AddInputCharactersUTF8(event->text);
      break;
    case HNUI_EVENT_FOCUS:
      io.AddFocusEvent(true);
      break;
    case HNUI_EVENT_BLUR:
      io.AddFocusEvent(false);
      break;
    // The size is read every frame (platformNewFrame), and ImGui pastes
    // through its clipboard functions rather than from an event; the rest
    // have no ImGui input counterpart.
    case HNUI_EVENT_INVALID:
    case HNUI_EVENT_RESIZE:
    case HNUI_EVENT_CLOSE_REQUESTED:
    case HNUI_EVENT_FILE_DROP:
    case HNUI_EVENT_MOUSE_ENTER:
    case HNUI_EVENT_MINIMIZE:
    case HNUI_EVENT_RESTORE:
    case HNUI_EVENT_PASTE:
    case HNUI_EVENT_TYPE_FORCE_I32:
      break;
  }
}

/// ImGui's Platform_SetClipboardTextFn: copy and cut.
void setClipboardText(ImGuiContext *, const char *text) {
  // Traced first: the host applies the write on the platform thread and
  // traces its own read-back, which must not be able to come first.
  trace("clipboard write requested, %zu bytes", std::strlen(text));
  hnui_write_clipboard(gWindow, text);
}

/// ImGui's Platform_GetClipboardTextFn: paste. ImGui reads the result
/// before it calls this again, so one buffer is enough; NULL is ImGui's
/// "no text", which leaves the field unchanged.
const char *getClipboardText(ImGuiContext *) {
  static std::string text;
  char *out = nullptr;
  const int status = hnui_read_clipboard_sync(gWindow, 500, &out);
  if (status != HNUI_OK) {
    trace("clipboard get: status %d", status);
    return nullptr;
  }
  text = out;
  hnui_free(out);
  trace("clipboard get: ok, %zu bytes", text.size());
  return text.c_str();
}

/// The hnui cursor for a shape ImGui asks for. Wait and Progress have no
/// hnui counterpart and get the arrow.
hnui_cursor translateCursor(ImGuiMouseCursor c) {
  switch (c) {
    case ImGuiMouseCursor_Arrow:
    case ImGuiMouseCursor_Wait:
    case ImGuiMouseCursor_Progress:
      return HNUI_CURSOR_ARROW;
    case ImGuiMouseCursor_TextInput:
      return HNUI_CURSOR_IBEAM;
    case ImGuiMouseCursor_ResizeAll:
      return HNUI_CURSOR_RESIZE_ALL;
    case ImGuiMouseCursor_ResizeNS:
      return HNUI_CURSOR_RESIZE_NS;
    case ImGuiMouseCursor_ResizeEW:
      return HNUI_CURSOR_RESIZE_EW;
    case ImGuiMouseCursor_ResizeNESW:
      return HNUI_CURSOR_RESIZE_NESW;
    case ImGuiMouseCursor_ResizeNWSE:
      return HNUI_CURSOR_RESIZE_NWSE;
    case ImGuiMouseCursor_Hand:
      return HNUI_CURSOR_POINTING_HAND;
    case ImGuiMouseCursor_NotAllowed:
      return HNUI_CURSOR_NOT_ALLOWED;
    default: // None and COUNT, which are not shapes
      return HNUI_CURSOR_ARROW;
  }
}

/// What platformAfterRender() last did, kept as two values because ImGui
/// can hide the cursor without changing its shape (io.MouseDrawCursor):
/// whether it is hidden, and the shape last set, or ImGuiMouseCursor_None
/// before the first, which no frame matches, so the first frame sets one.
bool gCursorHidden = false;
ImGuiMouseCursor gCursorShape = ImGuiMouseCursor_None;

} // namespace

void platformInit(hnui_window *w) {
  ImGuiIO &io = ImGui::GetIO();
  io.BackendPlatformName = "hermes-node hnui";
  io.BackendFlags |= ImGuiBackendFlags_HasMouseCursors;
  gWindow = w;
  ImGuiPlatformIO &pio = ImGui::GetPlatformIO();
  pio.Platform_SetClipboardTextFn = setClipboardText;
  pio.Platform_GetClipboardTextFn = getClipboardText;
  const int id = hnui_add_event_listener(w, onEvent, nullptr);
  trace("event listener %s (id %d)", id != 0 ? "registered" : "FAILED", id);
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

void platformAfterRender(hnui_window *w) {
  const ImGuiIO &io = ImGui::GetIO();
  if (io.ConfigFlags & ImGuiConfigFlags_NoMouseCursorChange)
    return;
  const ImGuiMouseCursor c = ImGui::GetMouseCursor();
  // ImGui draws the cursor itself under MouseDrawCursor, so the system's
  // is hidden then, whatever the shape.
  const bool hidden = c == ImGuiMouseCursor_None || io.MouseDrawCursor;
  // Each trace precedes its call, so it is ordered before the host's
  // trace of applying the command on the platform thread.
  if (hidden != gCursorHidden) {
    trace("cursor %s", hidden ? "hidden" : "shown");
    hnui_show_cursor(w, !hidden);
    gCursorHidden = hidden;
  }
  if (!hidden && c != gCursorShape) {
    const hnui_cursor hc = translateCursor(c);
    trace("cursor %d -> hnui %d", c, static_cast<int>(hc));
    hnui_set_cursor(w, hc);
    gCursorShape = c;
  }
}

int lastCursorRequested() {
  return gCursorShape;
}

} // namespace hnimgui
