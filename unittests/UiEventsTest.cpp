/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <hermes/node-compat/compositor/channels.h>
#include <hermes/node-compat/ui/hnui_abi.h>

#include <event_injector.h>
#include <sapp_event_convert.h>

#include "gtest/gtest.h"

#include <cstring>
#include <map>
#include <string>
#include <vector>

using namespace hermes::node_compat;

namespace {

sapp_event makeEvent(sapp_event_type type) {
  sapp_event e;
  std::memset(&e, 0, sizeof(e));
  e.type = type;
  return e;
}

/// A source whose clipboard and drop callbacks fail the test if they are
/// reached for an event that has no business calling them.
PlatformSource plainSource() {
  PlatformSource src;
  src.readClipboard = [](std::string &) {
    ADD_FAILURE() << "readClipboard called";
    return false;
  };
  src.droppedPaths = [] {
    ADD_FAILURE() << "droppedPaths called";
    return std::vector<std::string>();
  };
  return src;
}

} // namespace

TEST(SappEventConvertTest, EveryEventTypeIsEitherForwardedOrRefused) {
  const std::map<int, UiEventType> forwarded = {
      {SAPP_EVENTTYPE_KEY_DOWN, UiEventType::kKeyDown},
      {SAPP_EVENTTYPE_KEY_UP, UiEventType::kKeyUp},
      {SAPP_EVENTTYPE_CHAR, UiEventType::kTextInput},
      {SAPP_EVENTTYPE_MOUSE_DOWN, UiEventType::kMouseDown},
      {SAPP_EVENTTYPE_MOUSE_UP, UiEventType::kMouseUp},
      {SAPP_EVENTTYPE_MOUSE_SCROLL, UiEventType::kMouseWheel},
      {SAPP_EVENTTYPE_MOUSE_MOVE, UiEventType::kMouseMove},
      {SAPP_EVENTTYPE_MOUSE_ENTER, UiEventType::kMouseEnter},
      {SAPP_EVENTTYPE_MOUSE_LEAVE, UiEventType::kMouseLeave},
      {SAPP_EVENTTYPE_RESIZED, UiEventType::kResize},
      {SAPP_EVENTTYPE_ICONIFIED, UiEventType::kMinimize},
      {SAPP_EVENTTYPE_RESTORED, UiEventType::kRestore},
      {SAPP_EVENTTYPE_FOCUSED, UiEventType::kFocus},
      {SAPP_EVENTTYPE_UNFOCUSED, UiEventType::kBlur},
      {SAPP_EVENTTYPE_QUIT_REQUESTED, UiEventType::kCloseRequested},
      {SAPP_EVENTTYPE_CLIPBOARD_PASTED, UiEventType::kPaste},
      {SAPP_EVENTTYPE_FILES_DROPPED, UiEventType::kFileDrop},
  };
  const std::vector<int> refused = {
      SAPP_EVENTTYPE_INVALID,
      SAPP_EVENTTYPE_TOUCHES_BEGAN,
      SAPP_EVENTTYPE_TOUCHES_MOVED,
      SAPP_EVENTTYPE_TOUCHES_ENDED,
      SAPP_EVENTTYPE_TOUCHES_CANCELLED,
      SAPP_EVENTTYPE_SUSPENDED,
      SAPP_EVENTTYPE_RESUMED,
  };

  for (int t = 0; t < _SAPP_EVENTTYPE_NUM; ++t) {
    const bool isForwarded = forwarded.count(t) != 0;
    int refusedCount = 0;
    for (int r : refused)
      refusedCount += r == t;
    EXPECT_EQ(int(isForwarded) + refusedCount, 1)
        << "sapp_event_type " << t
        << " is in neither table or in both: decide which it belongs in";

    PlatformSource src;
    src.readClipboard = [](std::string &s) {
      s = "x";
      return true;
    };
    src.droppedPaths = [] { return std::vector<std::string>{"a"}; };
    UiEvent out;
    const bool ok =
        convertSappEvent(makeEvent(static_cast<sapp_event_type>(t)), src, &out);
    EXPECT_EQ(ok, isForwarded) << "sapp_event_type " << t;
    if (isForwarded && ok)
      EXPECT_EQ(out.type, forwarded.at(t)) << "sapp_event_type " << t;
  }
}
// Mutation: return true for SAPP_EVENTTYPE_TOUCHES_BEGAN. Fails.

TEST(SappEventConvertTest, ButtonsUseDomNumbering) {
  struct {
    sapp_mousebutton in;
    int expected;
  } cases[] = {
      {SAPP_MOUSEBUTTON_LEFT, 0},
      {SAPP_MOUSEBUTTON_RIGHT, 2},
      {SAPP_MOUSEBUTTON_MIDDLE, 1},
  };
  for (const auto &c : cases) {
    sapp_event e = makeEvent(SAPP_EVENTTYPE_MOUSE_DOWN);
    e.mouse_button = c.in;
    UiEvent out;
    ASSERT_TRUE(convertSappEvent(e, plainSource(), &out));
    EXPECT_EQ(out.button, c.expected) << "sapp button " << int(c.in);
    e.type = SAPP_EVENTTYPE_MOUSE_UP;
    ASSERT_TRUE(convertSappEvent(e, plainSource(), &out));
    EXPECT_EQ(out.button, c.expected);
  }
}
// Mutation: pass mouse_button through. Fails for RIGHT and MIDDLE.

TEST(SappEventConvertTest, ModifiersMapBitByBit) {
  struct {
    uint32_t sapp;
    int hnui;
  } bits[] = {
      {SAPP_MODIFIER_SHIFT, HNUI_MOD_SHIFT},
      {SAPP_MODIFIER_CTRL, HNUI_MOD_CTRL},
      {SAPP_MODIFIER_ALT, HNUI_MOD_ALT},
      {SAPP_MODIFIER_SUPER, HNUI_MOD_META},
      {SAPP_MODIFIER_LMB, HNUI_MOD_LMB},
      {SAPP_MODIFIER_RMB, HNUI_MOD_RMB},
      {SAPP_MODIFIER_MMB, HNUI_MOD_MMB},
  };
  uint32_t allSapp = 0;
  int allHnui = 0;
  for (const auto &b : bits) {
    allSapp |= b.sapp;
    allHnui |= b.hnui;
    sapp_event e = makeEvent(SAPP_EVENTTYPE_KEY_DOWN);
    e.modifiers = b.sapp;
    UiEvent out;
    ASSERT_TRUE(convertSappEvent(e, plainSource(), &out));
    EXPECT_EQ(out.modifiers, b.hnui) << "sapp modifier " << b.sapp;
  }
  sapp_event e = makeEvent(SAPP_EVENTTYPE_MOUSE_MOVE);
  e.modifiers = allSapp;
  UiEvent out;
  ASSERT_TRUE(convertSappEvent(e, plainSource(), &out));
  EXPECT_EQ(out.modifiers, allHnui);
}

TEST(SappEventConvertTest, PositionsAreLogicalPixels) {
  PlatformSource src = plainSource();
  src.window.dpiScale = 2.0;
  sapp_event e = makeEvent(SAPP_EVENTTYPE_MOUSE_MOVE);
  e.mouse_x = 200;
  e.mouse_y = 80;
  e.mouse_dx = 10;
  e.mouse_dy = -6;
  UiEvent out;
  ASSERT_TRUE(convertSappEvent(e, src, &out));
  EXPECT_EQ(out.x, 100);
  EXPECT_EQ(out.y, 40);
  EXPECT_EQ(out.dx, 5);
  EXPECT_EQ(out.dy, -3);
}
// Mutation: drop the division. Fails.

TEST(SappEventConvertTest, CharCodeBecomesUtf8) {
  struct {
    uint32_t cp;
    const char *utf8;
  } cases[] = {
      {'a', "a"},
      {0xE9, "\xC3\xA9"},
      {0x20AC, "\xE2\x82\xAC"},
      {0x1F600, "\xF0\x9F\x98\x80"},
  };
  for (const auto &c : cases) {
    sapp_event e = makeEvent(SAPP_EVENTTYPE_CHAR);
    e.char_code = c.cp;
    UiEvent out;
    ASSERT_TRUE(convertSappEvent(e, plainSource(), &out));
    EXPECT_EQ(out.text, c.utf8) << "U+" << std::hex << c.cp;
  }
}

TEST(SappEventConvertTest, WheelCarriesScrollInDxDy) {
  PlatformSource src = plainSource();
  src.window.dpiScale = 2.0;
  sapp_event e = makeEvent(SAPP_EVENTTYPE_MOUSE_SCROLL);
  e.scroll_x = 1.5f;
  e.scroll_y = -3.0f;
  e.mouse_x = 40;
  e.mouse_dx = 99; // must not leak into the scroll
  UiEvent out;
  ASSERT_TRUE(convertSappEvent(e, src, &out));
  EXPECT_EQ(out.dx, 1.5) << "a scroll distance is not a framebuffer length";
  EXPECT_EQ(out.dy, -3.0);
  EXPECT_EQ(out.x, 20);
}

TEST(SappEventConvertTest, KeyRepeatIsCarried) {
  sapp_event e = makeEvent(SAPP_EVENTTYPE_KEY_DOWN);
  e.key_code = SAPP_KEYCODE_A;
  e.key_repeat = true;
  UiEvent out;
  ASSERT_TRUE(convertSappEvent(e, plainSource(), &out));
  EXPECT_TRUE(out.repeat);
  EXPECT_EQ(out.keyCode, 65);
  e.key_repeat = false;
  ASSERT_TRUE(convertSappEvent(e, plainSource(), &out));
  EXPECT_FALSE(out.repeat);
}

TEST(SappEventConvertTest, StateEventsUpdateTheSnapshot) {
  PlatformSource src = plainSource();
  src.window.width = 640;
  src.window.height = 480;
  src.window.dpiScale = 1.5;
  src.window.fullscreen = true;
  UiEvent out;

  ASSERT_TRUE(convertSappEvent(makeEvent(SAPP_EVENTTYPE_ICONIFIED), src, &out));
  EXPECT_TRUE(out.window.minimized);
  EXPECT_FALSE(out.window.focused);

  PlatformSource minimized = src;
  minimized.window.minimized = true;
  ASSERT_TRUE(
      convertSappEvent(makeEvent(SAPP_EVENTTYPE_RESTORED), minimized, &out));
  EXPECT_FALSE(out.window.minimized);

  ASSERT_TRUE(convertSappEvent(makeEvent(SAPP_EVENTTYPE_FOCUSED), src, &out));
  EXPECT_TRUE(out.window.focused);

  PlatformSource focused = src;
  focused.window.focused = true;
  ASSERT_TRUE(
      convertSappEvent(makeEvent(SAPP_EVENTTYPE_UNFOCUSED), focused, &out));
  EXPECT_FALSE(out.window.focused);

  // Everything else carries the caller's snapshot unchanged.
  PlatformSource busy = src;
  busy.window.focused = true;
  busy.window.minimized = true;
  for (sapp_event_type t :
       {SAPP_EVENTTYPE_KEY_DOWN,
        SAPP_EVENTTYPE_MOUSE_MOVE,
        SAPP_EVENTTYPE_MOUSE_ENTER,
        SAPP_EVENTTYPE_RESIZED,
        SAPP_EVENTTYPE_QUIT_REQUESTED}) {
    ASSERT_TRUE(convertSappEvent(makeEvent(t), busy, &out)) << int(t);
    EXPECT_EQ(out.window.width, 640) << int(t);
    EXPECT_EQ(out.window.height, 480);
    EXPECT_EQ(out.window.dpiScale, 1.5);
    EXPECT_TRUE(out.window.fullscreen);
    EXPECT_TRUE(out.window.focused);
    EXPECT_TRUE(out.window.minimized);
  }
}

TEST(SappEventConvertTest, PasteCarriesTheClipboardAndAFailedReadIsRefused) {
  PlatformSource src = plainSource();
  src.readClipboard = [](std::string &s) {
    s = "clip";
    return true;
  };
  UiEvent out;
  ASSERT_TRUE(
      convertSappEvent(makeEvent(SAPP_EVENTTYPE_CLIPBOARD_PASTED), src, &out));
  EXPECT_EQ(out.type, UiEventType::kPaste);
  EXPECT_EQ(out.text, "clip");

  src.readClipboard = [](std::string &s) {
    s = "stale";
    return false;
  };
  EXPECT_FALSE(
      convertSappEvent(makeEvent(SAPP_EVENTTYPE_CLIPBOARD_PASTED), src, &out));
}

TEST(SappEventConvertTest, DropCarriesPathsAndPosition) {
  PlatformSource src = plainSource();
  src.window.dpiScale = 2.0;
  src.droppedPaths = [] { return std::vector<std::string>{"/a/b", "/c"}; };
  sapp_event e = makeEvent(SAPP_EVENTTYPE_FILES_DROPPED);
  e.mouse_x = 30;
  e.mouse_y = 50;
  UiEvent out;
  ASSERT_TRUE(convertSappEvent(e, src, &out));
  EXPECT_EQ(out.type, UiEventType::kFileDrop);
  ASSERT_EQ(out.paths.size(), 2u);
  EXPECT_EQ(out.paths[0], "/a/b");
  EXPECT_EQ(out.paths[1], "/c");
  EXPECT_EQ(out.x, 15);
  EXPECT_EQ(out.y, 25);
}

TEST(DomCodeTest, RepresentativeNames) {
  EXPECT_STREQ(domCodeForKey(SAPP_KEYCODE_A), "KeyA");
  EXPECT_STREQ(domCodeForKey(SAPP_KEYCODE_1), "Digit1");
  EXPECT_STREQ(domCodeForKey(SAPP_KEYCODE_ESCAPE), "Escape");
  EXPECT_STREQ(domCodeForKey(SAPP_KEYCODE_LEFT), "ArrowLeft");
  EXPECT_STREQ(domCodeForKey(SAPP_KEYCODE_LEFT_SHIFT), "ShiftLeft");
  EXPECT_STREQ(domCodeForKey(SAPP_KEYCODE_KP_ENTER), "NumpadEnter");
  EXPECT_STREQ(domCodeForKey(SAPP_KEYCODE_F12), "F12");
  EXPECT_EQ(domCodeForKey(-1), nullptr);
}

TEST(DomCodeTest, EveryKeyHasAName) {
  // 120 real enumerators in sapp_keycode at c0db757. Counted by taking the
  // lines of the enum (sokol_app.h, `typedef enum sapp_keycode` to
  // `} sapp_keycode;`) that name a SAPP_KEYCODE_ constant with a numeric
  // value, and leaving out SAPP_KEYCODE_INVALID (not a key) and
  // _SAPP_KEYCODE_FORCE_U32 (a width forcer, not a key): 122 lines carrying
  // SAPP_KEYCODE_, less those two. A Sokol bump that adds a key fails here
  // until it is named in domCodeForKey().
  const int kRealKeys = 120;
  int named = 0;
  for (int k = 0; k <= 400; ++k)
    named += domCodeForKey(k) != nullptr;
  EXPECT_EQ(named, kRealKeys);
  EXPECT_EQ(domCodeForKey(SAPP_KEYCODE_INVALID), nullptr);
}

TEST(PlatformEventInjectorTest, PreservesInjectionOrder) {
  // Mutation: take from the back of the queue. The paste would then come
  // out first, reordering a test's input relative to what came before it.
  PlatformEventInjector injector;

  InjectedPlatformEvent key;
  key.event = makeEvent(SAPP_EVENTTYPE_KEY_DOWN);
  key.event.key_code = SAPP_KEYCODE_A;
  InjectedPlatformEvent drop;
  drop.event = makeEvent(SAPP_EVENTTYPE_FILES_DROPPED);
  drop.paths = {"/a", "/b"};
  InjectedPlatformEvent paste;
  paste.event = makeEvent(SAPP_EVENTTYPE_CLIPBOARD_PASTED);
  paste.hasClipboard = true;
  paste.clipboard = "clip";

  injector.inject(key);
  injector.inject(drop);
  injector.inject(paste);

  InjectedPlatformEvent out;
  ASSERT_TRUE(injector.takeNext(&out));
  EXPECT_EQ(out.event.type, SAPP_EVENTTYPE_KEY_DOWN);
  EXPECT_EQ(out.event.key_code, SAPP_KEYCODE_A);
  ASSERT_TRUE(injector.takeNext(&out));
  EXPECT_EQ(out.event.type, SAPP_EVENTTYPE_FILES_DROPPED);
  EXPECT_EQ(out.paths, (std::vector<std::string>{"/a", "/b"}));
  ASSERT_TRUE(injector.takeNext(&out));
  EXPECT_EQ(out.event.type, SAPP_EVENTTYPE_CLIPBOARD_PASTED);
  EXPECT_TRUE(out.hasClipboard);
  EXPECT_EQ(out.clipboard, "clip");

  EXPECT_FALSE(injector.takeNext(&out));
}
