/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <hermes/node-compat/compositor/hnui_event_convert.h>

namespace hermes {
namespace node_compat {

namespace {

/// A switch, not a `static_cast` between the two enums: `UiEventType` and
/// `hnui_event_type` are declared in separate headers with separate owners,
/// and a numeric-identity assumption between them would silently drift the
/// moment either gained a member out of step with the other. Each arm is
/// named explicitly so a reordering on either side is a missing-case
/// compiler warning here, not a swapped event delivered to an addon.
hnui_event_type toHnuiEventType(UiEventType type) {
  switch (type) {
    case UiEventType::kInvalid:
      return HNUI_EVENT_INVALID;
    case UiEventType::kMouseMove:
      return HNUI_EVENT_MOUSE_MOVE;
    case UiEventType::kMouseDown:
      return HNUI_EVENT_MOUSE_DOWN;
    case UiEventType::kMouseUp:
      return HNUI_EVENT_MOUSE_UP;
    case UiEventType::kMouseWheel:
      return HNUI_EVENT_MOUSE_WHEEL;
    case UiEventType::kKeyDown:
      return HNUI_EVENT_KEY_DOWN;
    case UiEventType::kKeyUp:
      return HNUI_EVENT_KEY_UP;
    case UiEventType::kTextInput:
      return HNUI_EVENT_TEXT_INPUT;
    case UiEventType::kResize:
      return HNUI_EVENT_RESIZE;
    case UiEventType::kFocus:
      return HNUI_EVENT_FOCUS;
    case UiEventType::kBlur:
      return HNUI_EVENT_BLUR;
    case UiEventType::kCloseRequested:
      return HNUI_EVENT_CLOSE_REQUESTED;
    case UiEventType::kFileDrop:
      return HNUI_EVENT_FILE_DROP;
  }
  // Unreachable for any value the compiler let through the switch above
  // (which covers every UiEventType enumerator); a value that bypassed the
  // enum class's type safety entirely is exactly the "nobody wrote this"
  // case HNUI_EVENT_INVALID exists to name.
  return HNUI_EVENT_INVALID;
}

} // namespace

HnuiEventConverter::HnuiEventConverter(const UiEvent &source) {
  event_.size = sizeof(hnui_event);
  // event_.type is int32_t, not hnui_event_type (see the field's own
  // comment in hnui_abi.h for why); this is the one cast that boundary
  // requires.
  event_.type = static_cast<int32_t>(toHnuiEventType(source.type));
  event_.x = source.x;
  event_.y = source.y;
  event_.dx = source.dx;
  event_.dy = source.dy;
  event_.button = source.button;
  event_.modifiers = source.modifiers;
  event_.key_code = source.keyCode;

  // Borrowed directly from the source UiEvent's own std::string storage,
  // which std::string guarantees is NUL-terminated -- no copy. NULL for
  // every event kind but kTextInput, matching the field's documented
  // "NULL otherwise".
  event_.text =
      source.type == UiEventType::kTextInput ? source.text.c_str() : nullptr;

  if (source.type == UiEventType::kFileDrop && !source.paths.empty()) {
    pathPointers_.reserve(source.paths.size());
    for (const std::string &path : source.paths)
      pathPointers_.push_back(path.c_str());
    event_.paths = pathPointers_.data();
    event_.path_count = static_cast<uint32_t>(pathPointers_.size());
  } else {
    event_.paths = nullptr;
    event_.path_count = 0;
  }
}

} // namespace node_compat
} // namespace hermes
