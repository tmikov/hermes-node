/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <hermes/node-compat/compositor/channels.h>
#include <hermes/node-compat/compositor/hnui_event_convert.h>
#include <hermes/node-compat/ui/hnui_abi.h>

#include "gtest/gtest.h"

#include <cstddef>
#include <set>
#include <type_traits>
#include <utility>

using namespace hermes::node_compat;

// HnuiAbiCProbe.c -- compiled as C99, not C++ -- reports what a plain-C
// compilation of hnui_abi.h sees. CAndCxxCompilationsAgreeOnLayout below
// compares each of these against what this (C++) translation unit sees.
extern "C" {
size_t hnuiAbiCProbeSizeofEvent(void);
size_t hnuiAbiCProbeSizeofWindowState(void);
size_t hnuiAbiCProbeSizeofTarget(void);
size_t hnuiAbiCProbeSizeofEventTypeField(void);
size_t hnuiAbiCProbeOffsetofEventText(void);
size_t hnuiAbiCProbeOffsetofEventPaths(void);
}

// hnui_event::type is declared int32_t, deliberately not hnui_event_type
// itself -- see the field's own comment in hnui_abi.h. This is the
// strongest form the pin can take: a compile-time type-identity check
// that fails the whole file's build (not one TEST body) the moment the
// field's declared type changes, which is exactly the failure mode wanted
// for something a single compiler's test run cannot otherwise prove: an
// enum's underlying type is implementation-defined, so no test that only
// runs would ever see a *different* compiler disagree about its width.
static_assert(
    std::is_same<decltype(hnui_event{}.type), int32_t>::value,
    "hnui_event::type must be declared int32_t, not hnui_event_type -- a "
    "struct read at fixed byte offsets cannot depend on an enum's "
    "implementation-defined underlying-type width");

// No test in this file can block: every one of them is a plain
// construction-and-assertion over POD data, with no lock, no thread and no
// I/O, so this file carries no TestWatchdog. (Compare
// unittests/CompositorTest.cpp and UiHostTest.cpp, whose watchdogs guard
// tests that spin threads against each other or drive a real event loop.)

//===========================================================================
// Layout: the size-prefix rule.
//===========================================================================

TEST(HnuiAbiLayout, EverySizePrefixIsTheFirstMember) {
  // The whole size-prefix scheme (comment at the top of hnui_abi.h) depends
  // on `size` being readable without knowing anything else about the
  // struct's layout -- which only holds if it is always at offset 0.
  EXPECT_EQ(offsetof(hnui_event, size), 0u);
  EXPECT_EQ(offsetof(hnui_window_state, size), 0u);
  EXPECT_EQ(offsetof(hnui_target, size), 0u);
}
// Mutation: reorder hnui_event so `type` precedes `size`. Fails: the first
// EXPECT_EQ reports offsetof(hnui_event, size) == 4, not 0.

TEST(HnuiAbiLayout, MinSizeConstantsNeverExceedTheCurrentStruct) {
  // HNUI_*_MIN_SIZE names how much of the struct the *current* header
  // guarantees was written; it can never claim more bytes than the struct
  // this version of the header actually has.
  EXPECT_LE(HNUI_EVENT_MIN_SIZE, sizeof(hnui_event));
  EXPECT_LE(HNUI_WINDOW_STATE_MIN_SIZE, sizeof(hnui_window_state));
  EXPECT_LE(HNUI_TARGET_MIN_SIZE, sizeof(hnui_target));
}
// Mutation: define HNUI_EVENT_MIN_SIZE as sizeof(hnui_event) + 4 (a
// hand-typed constant that has drifted from the struct it describes).
// Fails.

TEST(HnuiAbiLayout, MinSizeConstantsAreComputedNotHandWritten) {
  // Each MIN_SIZE is defined via offsetof(lastField) + sizeof(lastField),
  // specifically so trailing struct padding can never sneak into it (a
  // caller that filled exactly through the last field, with the compiler
  // adding tail padding it never touched, must still satisfy the minimum).
  // Recomputing it here as offsetof(nextField) pins that: if MIN_SIZE were
  // ever hand-written as sizeof(struct) instead, this test would still pass
  // as long as there is no tail padding, so what makes it meaningful is
  // sizeof(hnui_window_state) > HNUI_WINDOW_STATE_MIN_SIZE -- that struct
  // holds a `double`, so it is 8-byte aligned as a whole, and its last
  // field (`bool minimized`) leaves real tail padding on a typical 64-bit
  // ABI -- asserted separately below so a future layout change that
  // removes the padding does not turn this file silently untested.
  //
  // This test only ever compares MIN_SIZE against a re-derivation of the
  // SAME macro expression (offsetof of the field the macro itself names),
  // so it catches a hand-typed constant that has drifted from that
  // expression, but not a reorder that moves some OTHER field past the
  // macro's anchor field -- EveryFieldLiesWithinItsStructsMinimumSize below
  // is what catches that.
  EXPECT_EQ(
      HNUI_EVENT_MIN_SIZE, offsetof(hnui_event, path_count) + sizeof(uint32_t));
  EXPECT_EQ(
      HNUI_WINDOW_STATE_MIN_SIZE,
      offsetof(hnui_window_state, minimized) + sizeof(bool));
  EXPECT_EQ(
      HNUI_TARGET_MIN_SIZE,
      offsetof(hnui_target, generation) + sizeof(uint32_t));
  EXPECT_LT(HNUI_WINDOW_STATE_MIN_SIZE, sizeof(hnui_window_state))
      << "hnui_window_state should have trailing padding after `minimized` "
         "on a typical 64-bit ABI; if this now fails because the layout "
         "changed, the point above still needs a struct with real tail "
         "padding to be meaningful -- pick another one";
}
// Mutation: hardcode HNUI_WINDOW_STATE_MIN_SIZE as
// sizeof(hnui_window_state) instead of offsetof(minimized) + sizeof(bool).
// Fails both the EXPECT_EQ for hnui_window_state above (sizeof() now
// includes the struct's trailing padding, which offsetof(minimized) +
// sizeof(bool) computed here does not) and the EXPECT_LT below (the two
// become equal). The identical mutation applied to HNUI_TARGET_MIN_SIZE
// instead does NOT turn this test red: hnui_target has no trailing padding
// on this ABI (all of its fields are 4 bytes wide), so
// offsetof(generation) + sizeof(uint32_t) and sizeof(hnui_target) already
// agree, and "hardcoded" and "computed" are the same expression by
// coincidence rather than by a bug being masked. Measured, not assumed --
// this was tried first and passed, which is why the comment reads this
// way instead of claiming it as a second catch.

TEST(HnuiAbiLayout, EveryFieldLiesWithinItsStructsMinimumSize) {
  // The comprehensive version of the rule above: every field this version
  // of the header declares, in all three structs, must lie entirely within
  // that struct's own minimum -- not merely the one field each MIN_SIZE
  // macro happens to be anchored on. A reorder that moves any field past
  // the macro's anchor field is invisible to the test above (the macro
  // recomputes around wherever its named field now sits) but is caught
  // here, because each assertion below recomputes its field's own
  // offsetof+sizeof independently of what any macro says.
  EXPECT_LE(offsetof(hnui_event, size) + sizeof(uint32_t), HNUI_EVENT_MIN_SIZE);
  EXPECT_LE(offsetof(hnui_event, type) + sizeof(int32_t), HNUI_EVENT_MIN_SIZE);
  EXPECT_LE(offsetof(hnui_event, x) + sizeof(double), HNUI_EVENT_MIN_SIZE);
  EXPECT_LE(offsetof(hnui_event, y) + sizeof(double), HNUI_EVENT_MIN_SIZE);
  EXPECT_LE(offsetof(hnui_event, dx) + sizeof(double), HNUI_EVENT_MIN_SIZE);
  EXPECT_LE(offsetof(hnui_event, dy) + sizeof(double), HNUI_EVENT_MIN_SIZE);
  EXPECT_LE(
      offsetof(hnui_event, button) + sizeof(int32_t), HNUI_EVENT_MIN_SIZE);
  EXPECT_LE(
      offsetof(hnui_event, modifiers) + sizeof(int32_t), HNUI_EVENT_MIN_SIZE);
  EXPECT_LE(
      offsetof(hnui_event, key_code) + sizeof(int32_t), HNUI_EVENT_MIN_SIZE);
  EXPECT_LE(
      offsetof(hnui_event, text) + sizeof(const char *), HNUI_EVENT_MIN_SIZE);
  EXPECT_LE(
      offsetof(hnui_event, paths) + sizeof(const char *const *),
      HNUI_EVENT_MIN_SIZE);
  EXPECT_LE(
      offsetof(hnui_event, path_count) + sizeof(uint32_t), HNUI_EVENT_MIN_SIZE);

  EXPECT_LE(
      offsetof(hnui_window_state, size) + sizeof(uint32_t),
      HNUI_WINDOW_STATE_MIN_SIZE);
  EXPECT_LE(
      offsetof(hnui_window_state, width) + sizeof(int32_t),
      HNUI_WINDOW_STATE_MIN_SIZE);
  EXPECT_LE(
      offsetof(hnui_window_state, height) + sizeof(int32_t),
      HNUI_WINDOW_STATE_MIN_SIZE);
  EXPECT_LE(
      offsetof(hnui_window_state, framebuffer_width) + sizeof(int32_t),
      HNUI_WINDOW_STATE_MIN_SIZE);
  EXPECT_LE(
      offsetof(hnui_window_state, framebuffer_height) + sizeof(int32_t),
      HNUI_WINDOW_STATE_MIN_SIZE);
  EXPECT_LE(
      offsetof(hnui_window_state, dpi_scale) + sizeof(double),
      HNUI_WINDOW_STATE_MIN_SIZE);
  EXPECT_LE(
      offsetof(hnui_window_state, focused) + sizeof(bool),
      HNUI_WINDOW_STATE_MIN_SIZE);
  EXPECT_LE(
      offsetof(hnui_window_state, fullscreen) + sizeof(bool),
      HNUI_WINDOW_STATE_MIN_SIZE);
  EXPECT_LE(
      offsetof(hnui_window_state, minimized) + sizeof(bool),
      HNUI_WINDOW_STATE_MIN_SIZE);

  EXPECT_LE(
      offsetof(hnui_target, size) + sizeof(uint32_t), HNUI_TARGET_MIN_SIZE);
  EXPECT_LE(
      offsetof(hnui_target, color_view) + sizeof(hnui_gpu_view),
      HNUI_TARGET_MIN_SIZE);
  EXPECT_LE(
      offsetof(hnui_target, resolve_view) + sizeof(hnui_gpu_view),
      HNUI_TARGET_MIN_SIZE);
  EXPECT_LE(
      offsetof(hnui_target, width) + sizeof(int32_t), HNUI_TARGET_MIN_SIZE);
  EXPECT_LE(
      offsetof(hnui_target, height) + sizeof(int32_t), HNUI_TARGET_MIN_SIZE);
  EXPECT_LE(
      offsetof(hnui_target, color_format) + sizeof(int32_t),
      HNUI_TARGET_MIN_SIZE);
  EXPECT_LE(
      offsetof(hnui_target, sample_count) + sizeof(int32_t),
      HNUI_TARGET_MIN_SIZE);
  EXPECT_LE(
      offsetof(hnui_target, generation) + sizeof(uint32_t),
      HNUI_TARGET_MIN_SIZE);
}
// Mutation: in hnui_abi.h, swap the declaration order of `sample_count`
// and `generation` inside hnui_target, so `sample_count` becomes the LAST
// field instead of `generation`. HNUI_TARGET_MIN_SIZE's macro
// (offsetof(hnui_target, generation) + sizeof(uint32_t)) silently shrinks
// to wherever `generation` now sits --
// MinSizeConstantsAreComputedNotHandWritten above does not notice, since it
// only re-derives that same macro expression against itself and the two still
// agree. This test does: `sample_count`'s own offsetof+sizeof now exceeds the
// (shrunk) HNUI_TARGET_MIN_SIZE, since sample_count sits after generation in
// the mutated layout.

TEST(HnuiAbiLayout, InvalidEventTypeIsZero) {
  // Fact carried over from UiEventType::kInvalid (channels.h): zero must be
  // "not a real event" so that a default-constructed or half-filled
  // hnui_event cannot masquerade as a valid one -- in particular, a
  // zero-initialized hnui_event{} must read as HNUI_EVENT_INVALID, not as
  // whatever event type happens to be first in registration order.
  EXPECT_EQ(static_cast<int>(HNUI_EVENT_INVALID), 0);
  EXPECT_EQ(
      static_cast<int>(HNUI_EVENT_INVALID),
      static_cast<int>(UiEventType::kInvalid));

  hnui_event zeroed{};
  EXPECT_EQ(static_cast<hnui_event_type>(zeroed.type), HNUI_EVENT_INVALID);
}
// Mutation: move HNUI_EVENT_MOUSE_MOVE to the front of the enum (so it is
// zero) and HNUI_EVENT_INVALID elsewhere. Fails: a value-initialized
// hnui_event now reads as a mouse move.

TEST(HnuiAbiLayout, EventTypeFieldWidthAndStructSizeArePinned) {
  // The static_assert near the top of this file is the real pin for the
  // field's declared TYPE -- reverting `int32_t type;` to `hnui_event_type
  // type;` in hnui_abi.h fails to COMPILE this file, which is a stronger,
  // more direct signal than any runtime assertion here could give: the
  // risk finding 1 names is a compiler that picks a narrower, or
  // differently-signed, underlying type for the enum than this compiler
  // does, which no single compiler's test *run* can demonstrate by
  // executing -- only a compile-time type-identity check rules it out
  // unconditionally, for every compiler. This runtime check restates the
  // field's width for a reader who only reads test output, and separately
  // pins the struct's current total size.
  EXPECT_EQ(sizeof(decltype(hnui_event{}.type)), sizeof(int32_t));
  EXPECT_EQ(sizeof(hnui_event), 80u);
}
// Mutation: revert `int32_t type;` to `hnui_event_type type;` in
// hnui_abi.h. Measured: on this compiler (Clang, both C and C++) NEITHER
// EXPECT_EQ above goes red -- Clang's chosen underlying type for
// hnui_event_type (widened by HNUI_EVENT_TYPE_FORCE_I32) already happens
// to be a 4-byte type here, the same "coincidence, not a caught bug"
// situation as the HNUI_TARGET_MIN_SIZE hardcode mutation above. What DOES
// turn this file red is the static_assert preceding
// EventTypeFieldWidthAndStructSizeArePinned:
// std::is_same<hnui_event_type, int32_t>::value is false regardless of
// the two types' sizes agreeing, so the whole HnuiAbiTest binary fails to
// compile -- RED via a build failure rather than a failing TEST body,
// which is the stronger of the two failure modes and the one this
// specific risk needs.

TEST(HnuiAbiLayout, CAndCxxCompilationsAgreeOnLayout) {
  // HnuiAbiCProbe.c includes this exact header under C (-std=c99
  // -pedantic, see unittests/CMakeLists.txt); its functions report what
  // that compilation sees. Comparing each against what THIS (C++)
  // translation unit sees is the actual cross-language ABI-agreement
  // question -- stronger than "hnui_abi.h merely compiles as C", which a
  // construct silently accepted by the compiler under both languages, but
  // producing a different layout in each, would not fail.
  EXPECT_EQ(hnuiAbiCProbeSizeofEvent(), sizeof(hnui_event));
  EXPECT_EQ(hnuiAbiCProbeSizeofWindowState(), sizeof(hnui_window_state));
  EXPECT_EQ(hnuiAbiCProbeSizeofTarget(), sizeof(hnui_target));
  EXPECT_EQ(hnuiAbiCProbeSizeofEventTypeField(), sizeof(int32_t));
  EXPECT_EQ(hnuiAbiCProbeOffsetofEventText(), offsetof(hnui_event, text));
  EXPECT_EQ(hnuiAbiCProbeOffsetofEventPaths(), offsetof(hnui_event, paths));
}
// Mutation: in hnui_abi.h, wrap one hnui_event field (e.g. `int32_t
// key_code;`) in `#ifdef __cplusplus ... #endif`, so it exists under C++
// but not under C -- a plausible mistake: someone "helps" plain-C
// compilation by guarding a field they wrongly believe is C++-only. Both
// languages still compile the header successfully, and nothing else in
// this file notices; this test's first EXPECT_EQ does, because
// sizeof(hnui_event) now differs between the two TUs.

//===========================================================================
// hnui_size_ok: the rejection rule.
//===========================================================================

TEST(HnuiSizeOk, RejectsBelowTheMinimumAcceptsAtOrAboveIt) {
  EXPECT_FALSE(hnui_size_ok(0, HNUI_EVENT_MIN_SIZE));
  EXPECT_FALSE(hnui_size_ok(HNUI_EVENT_MIN_SIZE - 1, HNUI_EVENT_MIN_SIZE));
  EXPECT_TRUE(hnui_size_ok(HNUI_EVENT_MIN_SIZE, HNUI_EVENT_MIN_SIZE));
  // The case that matters for forward compatibility: an addon built
  // against a LATER, larger header passes a bigger `size` to a host still
  // running this version. The host's minimum must still accept it.
  EXPECT_TRUE(hnui_size_ok(HNUI_EVENT_MIN_SIZE + 100, HNUI_EVENT_MIN_SIZE));
}
// Mutation: implement hnui_size_ok as `providedSize > minSize` (strict).
// Fails: the exact-equality case (an addon whose struct is precisely this
// version's minimum) is wrongly rejected.

//===========================================================================
// UiEvent -> hnui_event conversion.
//===========================================================================

TEST(HnuiEventConvert, EveryEventTypeMapsToItsOwnDistinctCounterpart) {
  const std::pair<UiEventType, hnui_event_type> table[] = {
      {UiEventType::kInvalid, HNUI_EVENT_INVALID},
      {UiEventType::kMouseMove, HNUI_EVENT_MOUSE_MOVE},
      {UiEventType::kMouseDown, HNUI_EVENT_MOUSE_DOWN},
      {UiEventType::kMouseUp, HNUI_EVENT_MOUSE_UP},
      {UiEventType::kMouseWheel, HNUI_EVENT_MOUSE_WHEEL},
      {UiEventType::kKeyDown, HNUI_EVENT_KEY_DOWN},
      {UiEventType::kKeyUp, HNUI_EVENT_KEY_UP},
      {UiEventType::kTextInput, HNUI_EVENT_TEXT_INPUT},
      {UiEventType::kResize, HNUI_EVENT_RESIZE},
      {UiEventType::kFocus, HNUI_EVENT_FOCUS},
      {UiEventType::kBlur, HNUI_EVENT_BLUR},
      {UiEventType::kCloseRequested, HNUI_EVENT_CLOSE_REQUESTED},
      {UiEventType::kFileDrop, HNUI_EVENT_FILE_DROP},
  };
  std::set<int> seen;
  for (const auto &entry : table) {
    UiEvent source;
    source.type = entry.first;
    HnuiEventConverter converter(source);
    EXPECT_EQ(
        static_cast<hnui_event_type>(converter.event().type), entry.second)
        << "UiEventType " << static_cast<int>(entry.first);
    EXPECT_TRUE(seen.insert(static_cast<int>(entry.second)).second)
        << "two different UiEventType values mapped to the same "
           "hnui_event_type -- one of them is a copy-paste swap in "
           "toHnuiEventType()";
  }
}
// Mutation: swap the kMouseDown/kMouseUp (or kFocus/kBlur) cases in
// toHnuiEventType(). Fails: the row for the swapped-away type reports the
// wrong hnui_event_type.

TEST(HnuiEventConvert, ScalarFieldsAndTheCurrentSizeAreCopied) {
  UiEvent source;
  source.type = UiEventType::kMouseWheel;
  source.x = 12.5;
  source.y = -3.25;
  source.dx = 1.0;
  source.dy = -2.0;
  source.button = 1;
  source.modifiers = 6;
  source.keyCode = 0;

  HnuiEventConverter converter(source);
  const hnui_event &event = converter.event();

  EXPECT_EQ(event.size, sizeof(hnui_event))
      << "the converter must announce that every field it currently knows "
         "about was written";
  EXPECT_EQ(static_cast<hnui_event_type>(event.type), HNUI_EVENT_MOUSE_WHEEL);
  EXPECT_DOUBLE_EQ(event.x, 12.5);
  EXPECT_DOUBLE_EQ(event.y, -3.25);
  EXPECT_DOUBLE_EQ(event.dx, 1.0);
  EXPECT_DOUBLE_EQ(event.dy, -2.0);
  EXPECT_EQ(event.button, 1);
  EXPECT_EQ(event.modifiers, 6);
  EXPECT_EQ(event.text, nullptr) << "text is kTextInput-only";
  EXPECT_EQ(event.paths, nullptr) << "paths is kFileDrop-only";
  EXPECT_EQ(event.path_count, 0u);
}
// Mutation: drop the `event_.dy = source.dy;` line in the converter. Fails:
// EXPECT_DOUBLE_EQ(event.dy, -2.0) sees 0.0.

TEST(HnuiEventConvert, TextInputBorrowsTheSourceStringRatherThanCopyingIt) {
  UiEvent source;
  source.type = UiEventType::kTextInput;
  source.text = "hello, world";

  HnuiEventConverter converter(source);
  const hnui_event &event = converter.event();

  ASSERT_NE(event.text, nullptr);
  EXPECT_STREQ(event.text, "hello, world");
  // The load-bearing check: hnui_abi.h documents this pointer as borrowed
  // from the source UiEvent's own storage, not copied. A correct
  // implementation therefore aliases source.text's buffer exactly.
  EXPECT_EQ(event.text, source.text.c_str())
      << "hnui_event::text must borrow the source UiEvent's std::string "
         "storage, not copy it into storage of its own";
}
// Mutation: have the converter copy the text into an owned std::string
// member instead of aliasing source.text.c_str(). The STREQ check still
// passes, but the pointer-identity check fails -- exactly the distinction
// between "reads the same text" and "borrows the text", which is the
// contract this task's brief states explicitly.

TEST(HnuiEventConvert, FileDropBorrowsEveryPathAndCountsThemCorrectly) {
  UiEvent source;
  source.type = UiEventType::kFileDrop;
  source.paths = {"/first/path", "/second/path/longer"};

  HnuiEventConverter converter(source);
  const hnui_event &event = converter.event();

  ASSERT_NE(event.paths, nullptr);
  ASSERT_EQ(event.path_count, 2u);
  EXPECT_STREQ(event.paths[0], "/first/path");
  EXPECT_STREQ(event.paths[1], "/second/path/longer");
  EXPECT_EQ(event.paths[0], source.paths[0].c_str())
      << "each path must borrow the source UiEvent's own std::string "
         "storage, not a copy of it";
  EXPECT_EQ(event.paths[1], source.paths[1].c_str());
}
// Mutation: swap the push order (push source.paths[1] before [0]). Fails:
// paths[0] no longer reads "/first/path".
// A second mutation this catches: have the converter copy each path into
// strings of its own. The STREQ checks still pass; the pointer-identity
// checks do not.

TEST(HnuiEventConvert, NonFileDropEventLeavesPathsNullEvenIfSet) {
  UiEvent source;
  source.type = UiEventType::kMouseMove;
  // Not a state the real platform callback would ever produce, but the
  // guard must not depend on `paths` happening to be empty -- it must
  // depend on the event actually being a kFileDrop.
  source.paths = {"/should/be/ignored"};

  HnuiEventConverter converter(source);
  EXPECT_EQ(converter.event().paths, nullptr);
  EXPECT_EQ(converter.event().path_count, 0u);
}
// Mutation: drop the `source.type == UiEventType::kFileDrop` half of the
// guard in the converter, leaving only `!source.paths.empty()`. This
// event's paths is non-empty, so the mutated condition takes the
// populate-paths branch and path_count becomes 1 instead of 0 -- caught
// here.
//
// The mirror-image mutation -- dropping the `!source.paths.empty()` half
// and keeping only the type check -- is not pinned by a test of its own:
// for an empty paths vector, taking the populate branch is
// observationally identical to taking the empty-else branch (reserve(0)
// on an already-empty vector performs no allocation, so path_count comes
// out 0 either way, and the standard leaves an empty vector's data()
// pointer unspecified rather than guaranteeing null). That half of the
// guard is therefore an optimization -- skip building an unnecessary
// zero-length pointer array -- and not an externally observable
// invariant, so no test claims to pin it.
