/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "sapp_event_convert.h"

#include <hermes/node-compat/ui/hnui_abi.h>

#include <utility>

namespace hermes {
namespace node_compat {

// sapp_keycode mirrors GLFW's numbering, and the converter passes key codes
// through unchanged. Three anchors, one from each of the ranges the enum is
// built from, so a Sokol that stopped mirroring GLFW fails to build here
// rather than delivering the wrong key.
static_assert(SAPP_KEYCODE_SPACE == 32, "sapp_keycode no longer mirrors GLFW");
static_assert(SAPP_KEYCODE_A == 65, "sapp_keycode no longer mirrors GLFW");
static_assert(
    SAPP_KEYCODE_ESCAPE == 256,
    "sapp_keycode no longer mirrors GLFW");

namespace {

/// Mapped bit by bit, never cast: the values of HNUI_MOD_* are this ABI's
/// own, and a renumbering of SAPP_MODIFIER_* must break a test here rather
/// than every addon.
int convertModifiers(uint32_t sappModifiers) {
  int result = 0;
  if (sappModifiers & SAPP_MODIFIER_SHIFT)
    result |= HNUI_MOD_SHIFT;
  if (sappModifiers & SAPP_MODIFIER_CTRL)
    result |= HNUI_MOD_CTRL;
  if (sappModifiers & SAPP_MODIFIER_ALT)
    result |= HNUI_MOD_ALT;
  if (sappModifiers & SAPP_MODIFIER_SUPER)
    result |= HNUI_MOD_META;
  if (sappModifiers & SAPP_MODIFIER_LMB)
    result |= HNUI_MOD_LMB;
  if (sappModifiers & SAPP_MODIFIER_RMB)
    result |= HNUI_MOD_RMB;
  if (sappModifiers & SAPP_MODIFIER_MMB)
    result |= HNUI_MOD_MMB;
  return result;
}

/// Sokol numbers its buttons left, right, middle; the DOM numbers them left,
/// middle, right.
int convertButton(sapp_mousebutton button) {
  switch (button) {
    case SAPP_MOUSEBUTTON_LEFT:
      return HNUI_BUTTON_LEFT;
    case SAPP_MOUSEBUTTON_MIDDLE:
      return HNUI_BUTTON_MIDDLE;
    case SAPP_MOUSEBUTTON_RIGHT:
      return HNUI_BUTTON_RIGHT;
    default:
      return HNUI_BUTTON_LEFT;
  }
}

void appendUtf8(std::string &out, uint32_t cp) {
  if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
    cp = 0xFFFD; // not a scalar value: the replacement character
  if (cp < 0x80) {
    out.push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

} // namespace

bool convertSappEvent(
    const sapp_event &in,
    const PlatformSource &src,
    UiEvent *out) {
  UiEvent ev;
  ev.window = src.window;
  // Positions arrive in framebuffer pixels, ours are logical.
  const double scale = src.window.dpiScale > 0 ? src.window.dpiScale : 1.0;

  auto pointer = [&] {
    ev.x = in.mouse_x / scale;
    ev.y = in.mouse_y / scale;
    ev.dx = in.mouse_dx / scale;
    ev.dy = in.mouse_dy / scale;
    ev.modifiers = convertModifiers(in.modifiers);
  };

  switch (in.type) {
    case SAPP_EVENTTYPE_KEY_DOWN:
    case SAPP_EVENTTYPE_KEY_UP:
      ev.type = in.type == SAPP_EVENTTYPE_KEY_DOWN ? UiEventType::kKeyDown
                                                   : UiEventType::kKeyUp;
      ev.keyCode = static_cast<int>(in.key_code);
      ev.repeat = in.key_repeat;
      ev.modifiers = convertModifiers(in.modifiers);
      break;
    case SAPP_EVENTTYPE_CHAR:
      ev.type = UiEventType::kTextInput;
      appendUtf8(ev.text, in.char_code);
      ev.repeat = in.key_repeat;
      ev.modifiers = convertModifiers(in.modifiers);
      break;
    case SAPP_EVENTTYPE_MOUSE_DOWN:
    case SAPP_EVENTTYPE_MOUSE_UP:
      ev.type = in.type == SAPP_EVENTTYPE_MOUSE_DOWN ? UiEventType::kMouseDown
                                                     : UiEventType::kMouseUp;
      pointer();
      ev.button = convertButton(in.mouse_button);
      break;
    case SAPP_EVENTTYPE_MOUSE_SCROLL:
      ev.type = UiEventType::kMouseWheel;
      pointer();
      // The scroll distance rides in dx/dy, as a quantity rather than a
      // position: it is not a framebuffer length, so no division.
      ev.dx = in.scroll_x;
      ev.dy = in.scroll_y;
      break;
    case SAPP_EVENTTYPE_MOUSE_MOVE:
      ev.type = UiEventType::kMouseMove;
      pointer();
      break;
    case SAPP_EVENTTYPE_MOUSE_ENTER:
      ev.type = UiEventType::kMouseEnter;
      pointer();
      break;
    case SAPP_EVENTTYPE_MOUSE_LEAVE:
      ev.type = UiEventType::kMouseLeave;
      pointer();
      break;
    case SAPP_EVENTTYPE_RESIZED:
      ev.type = UiEventType::kResize;
      break;
    case SAPP_EVENTTYPE_ICONIFIED:
      ev.type = UiEventType::kMinimize;
      ev.window.minimized = true;
      break;
    case SAPP_EVENTTYPE_RESTORED:
      ev.type = UiEventType::kRestore;
      ev.window.minimized = false;
      break;
    case SAPP_EVENTTYPE_FOCUSED:
      ev.type = UiEventType::kFocus;
      ev.window.focused = true;
      break;
    case SAPP_EVENTTYPE_UNFOCUSED:
      ev.type = UiEventType::kBlur;
      ev.window.focused = false;
      break;
    case SAPP_EVENTTYPE_QUIT_REQUESTED:
      ev.type = UiEventType::kCloseRequested;
      break;
    case SAPP_EVENTTYPE_CLIPBOARD_PASTED:
      // A failed read is refused rather than forwarded as an empty paste:
      // "the clipboard is empty" and "the read did not work" are different
      // facts, and a program acting on the first would paste nothing.
      if (!src.readClipboard || !src.readClipboard(ev.text))
        return false;
      ev.type = UiEventType::kPaste;
      break;
    case SAPP_EVENTTYPE_FILES_DROPPED:
      ev.type = UiEventType::kFileDrop;
      pointer();
      if (src.droppedPaths)
        ev.paths = src.droppedPaths();
      break;
    // Touch events come only from iOS, Android and Emscripten; neither v1
    // platform can produce one. SUSPENDED and RESUMED are mobile-only
    // lifecycle events likewise. Listed rather than defaulted, so that a
    // Sokol that adds a type is a -Wswitch warning here.
    case SAPP_EVENTTYPE_INVALID:
    case SAPP_EVENTTYPE_TOUCHES_BEGAN:
    case SAPP_EVENTTYPE_TOUCHES_MOVED:
    case SAPP_EVENTTYPE_TOUCHES_ENDED:
    case SAPP_EVENTTYPE_TOUCHES_CANCELLED:
    case SAPP_EVENTTYPE_SUSPENDED:
    case SAPP_EVENTTYPE_RESUMED:
    case _SAPP_EVENTTYPE_NUM:
    case _SAPP_EVENTTYPE_FORCE_U32:
      return false;
  }

  *out = std::move(ev);
  return true;
}

namespace {

bool isHighSurrogate(uint32_t cp) {
  return cp >= 0xD800 && cp <= 0xDBFF;
}

bool isLowSurrogate(uint32_t cp) {
  return cp >= 0xDC00 && cp <= 0xDFFF;
}

} // namespace

void SappEventConverter::convert(
    const sapp_event &in,
    const PlatformSource &src,
    std::vector<UiEvent> &out) {
  const bool isChar = in.type == SAPP_EVENTTYPE_CHAR;
  UiEvent ev;

  if (havePendingHigh_) {
    havePendingHigh_ = false;
    if (isChar && isLowSurrogate(in.char_code)) {
      sapp_event combined = in;
      combined.char_code = 0x10000 + ((pendingHigh_.char_code - 0xD800) << 10) +
          (in.char_code - 0xDC00);
      if (convertSappEvent(combined, src, &ev))
        out.push_back(std::move(ev));
      return;
    }
    // An orphan. Converted as a CHAR of U+FFFD against the state it
    // arrived with; no callback of that source is reached for a CHAR.
    sapp_event orphan = pendingHigh_;
    orphan.char_code = 0xFFFD;
    PlatformSource orphanSrc;
    orphanSrc.window = pendingWindow_;
    if (convertSappEvent(orphan, orphanSrc, &ev))
      out.push_back(std::move(ev));
  }

  if (isChar && isHighSurrogate(in.char_code)) {
    havePendingHigh_ = true;
    pendingHigh_ = in;
    pendingWindow_ = src.window;
    return;
  }

  if (convertSappEvent(in, src, &ev))
    out.push_back(std::move(ev));
}

const char *domCodeForKey(int keyCode) {
  switch (keyCode) {
    case SAPP_KEYCODE_SPACE:
      return "Space";
    case SAPP_KEYCODE_APOSTROPHE:
      return "Quote";
    case SAPP_KEYCODE_COMMA:
      return "Comma";
    case SAPP_KEYCODE_MINUS:
      return "Minus";
    case SAPP_KEYCODE_PERIOD:
      return "Period";
    case SAPP_KEYCODE_SLASH:
      return "Slash";
    case SAPP_KEYCODE_SEMICOLON:
      return "Semicolon";
    case SAPP_KEYCODE_EQUAL:
      return "Equal";
    case SAPP_KEYCODE_LEFT_BRACKET:
      return "BracketLeft";
    case SAPP_KEYCODE_BACKSLASH:
      return "Backslash";
    case SAPP_KEYCODE_RIGHT_BRACKET:
      return "BracketRight";
    case SAPP_KEYCODE_GRAVE_ACCENT:
      return "Backquote";
    case SAPP_KEYCODE_WORLD_1:
      return "IntlBackslash";
    case SAPP_KEYCODE_WORLD_2:
      return "Unidentified";
    case SAPP_KEYCODE_ESCAPE:
      return "Escape";
    case SAPP_KEYCODE_ENTER:
      return "Enter";
    case SAPP_KEYCODE_TAB:
      return "Tab";
    case SAPP_KEYCODE_BACKSPACE:
      return "Backspace";
    case SAPP_KEYCODE_INSERT:
      return "Insert";
    case SAPP_KEYCODE_DELETE:
      return "Delete";
    case SAPP_KEYCODE_RIGHT:
      return "ArrowRight";
    case SAPP_KEYCODE_LEFT:
      return "ArrowLeft";
    case SAPP_KEYCODE_DOWN:
      return "ArrowDown";
    case SAPP_KEYCODE_UP:
      return "ArrowUp";
    case SAPP_KEYCODE_PAGE_UP:
      return "PageUp";
    case SAPP_KEYCODE_PAGE_DOWN:
      return "PageDown";
    case SAPP_KEYCODE_HOME:
      return "Home";
    case SAPP_KEYCODE_END:
      return "End";
    case SAPP_KEYCODE_CAPS_LOCK:
      return "CapsLock";
    case SAPP_KEYCODE_SCROLL_LOCK:
      return "ScrollLock";
    case SAPP_KEYCODE_NUM_LOCK:
      return "NumLock";
    case SAPP_KEYCODE_PRINT_SCREEN:
      return "PrintScreen";
    case SAPP_KEYCODE_PAUSE:
      return "Pause";
    case SAPP_KEYCODE_KP_DECIMAL:
      return "NumpadDecimal";
    case SAPP_KEYCODE_KP_DIVIDE:
      return "NumpadDivide";
    case SAPP_KEYCODE_KP_MULTIPLY:
      return "NumpadMultiply";
    case SAPP_KEYCODE_KP_SUBTRACT:
      return "NumpadSubtract";
    case SAPP_KEYCODE_KP_ADD:
      return "NumpadAdd";
    case SAPP_KEYCODE_KP_ENTER:
      return "NumpadEnter";
    case SAPP_KEYCODE_KP_EQUAL:
      return "NumpadEqual";
    case SAPP_KEYCODE_LEFT_SHIFT:
      return "ShiftLeft";
    case SAPP_KEYCODE_LEFT_CONTROL:
      return "ControlLeft";
    case SAPP_KEYCODE_LEFT_ALT:
      return "AltLeft";
    case SAPP_KEYCODE_LEFT_SUPER:
      return "MetaLeft";
    case SAPP_KEYCODE_RIGHT_SHIFT:
      return "ShiftRight";
    case SAPP_KEYCODE_RIGHT_CONTROL:
      return "ControlRight";
    case SAPP_KEYCODE_RIGHT_ALT:
      return "AltRight";
    case SAPP_KEYCODE_RIGHT_SUPER:
      return "MetaRight";
    case SAPP_KEYCODE_MENU:
      return "ContextMenu";
    case SAPP_KEYCODE_0:
      return "Digit0";
    case SAPP_KEYCODE_KP_0:
      return "Numpad0";
    case SAPP_KEYCODE_1:
      return "Digit1";
    case SAPP_KEYCODE_KP_1:
      return "Numpad1";
    case SAPP_KEYCODE_2:
      return "Digit2";
    case SAPP_KEYCODE_KP_2:
      return "Numpad2";
    case SAPP_KEYCODE_3:
      return "Digit3";
    case SAPP_KEYCODE_KP_3:
      return "Numpad3";
    case SAPP_KEYCODE_4:
      return "Digit4";
    case SAPP_KEYCODE_KP_4:
      return "Numpad4";
    case SAPP_KEYCODE_5:
      return "Digit5";
    case SAPP_KEYCODE_KP_5:
      return "Numpad5";
    case SAPP_KEYCODE_6:
      return "Digit6";
    case SAPP_KEYCODE_KP_6:
      return "Numpad6";
    case SAPP_KEYCODE_7:
      return "Digit7";
    case SAPP_KEYCODE_KP_7:
      return "Numpad7";
    case SAPP_KEYCODE_8:
      return "Digit8";
    case SAPP_KEYCODE_KP_8:
      return "Numpad8";
    case SAPP_KEYCODE_9:
      return "Digit9";
    case SAPP_KEYCODE_KP_9:
      return "Numpad9";
    case SAPP_KEYCODE_A:
      return "KeyA";
    case SAPP_KEYCODE_B:
      return "KeyB";
    case SAPP_KEYCODE_C:
      return "KeyC";
    case SAPP_KEYCODE_D:
      return "KeyD";
    case SAPP_KEYCODE_E:
      return "KeyE";
    case SAPP_KEYCODE_F:
      return "KeyF";
    case SAPP_KEYCODE_G:
      return "KeyG";
    case SAPP_KEYCODE_H:
      return "KeyH";
    case SAPP_KEYCODE_I:
      return "KeyI";
    case SAPP_KEYCODE_J:
      return "KeyJ";
    case SAPP_KEYCODE_K:
      return "KeyK";
    case SAPP_KEYCODE_L:
      return "KeyL";
    case SAPP_KEYCODE_M:
      return "KeyM";
    case SAPP_KEYCODE_N:
      return "KeyN";
    case SAPP_KEYCODE_O:
      return "KeyO";
    case SAPP_KEYCODE_P:
      return "KeyP";
    case SAPP_KEYCODE_Q:
      return "KeyQ";
    case SAPP_KEYCODE_R:
      return "KeyR";
    case SAPP_KEYCODE_S:
      return "KeyS";
    case SAPP_KEYCODE_T:
      return "KeyT";
    case SAPP_KEYCODE_U:
      return "KeyU";
    case SAPP_KEYCODE_V:
      return "KeyV";
    case SAPP_KEYCODE_W:
      return "KeyW";
    case SAPP_KEYCODE_X:
      return "KeyX";
    case SAPP_KEYCODE_Y:
      return "KeyY";
    case SAPP_KEYCODE_Z:
      return "KeyZ";
    case SAPP_KEYCODE_F1:
      return "F1";
    case SAPP_KEYCODE_F2:
      return "F2";
    case SAPP_KEYCODE_F3:
      return "F3";
    case SAPP_KEYCODE_F4:
      return "F4";
    case SAPP_KEYCODE_F5:
      return "F5";
    case SAPP_KEYCODE_F6:
      return "F6";
    case SAPP_KEYCODE_F7:
      return "F7";
    case SAPP_KEYCODE_F8:
      return "F8";
    case SAPP_KEYCODE_F9:
      return "F9";
    case SAPP_KEYCODE_F10:
      return "F10";
    case SAPP_KEYCODE_F11:
      return "F11";
    case SAPP_KEYCODE_F12:
      return "F12";
    case SAPP_KEYCODE_F13:
      return "F13";
    case SAPP_KEYCODE_F14:
      return "F14";
    case SAPP_KEYCODE_F15:
      return "F15";
    case SAPP_KEYCODE_F16:
      return "F16";
    case SAPP_KEYCODE_F17:
      return "F17";
    case SAPP_KEYCODE_F18:
      return "F18";
    case SAPP_KEYCODE_F19:
      return "F19";
    case SAPP_KEYCODE_F20:
      return "F20";
    case SAPP_KEYCODE_F21:
      return "F21";
    case SAPP_KEYCODE_F22:
      return "F22";
    case SAPP_KEYCODE_F23:
      return "F23";
    case SAPP_KEYCODE_F24:
      return "F24";
    case SAPP_KEYCODE_F25:
      return "F25";
    default:
      return nullptr;
  }
}

} // namespace node_compat
} // namespace hermes
