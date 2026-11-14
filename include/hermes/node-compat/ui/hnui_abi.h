/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_NODE_COMPAT_UI_HNUI_ABI_H
#define HERMES_NODE_COMPAT_UI_HNUI_ABI_H

/// The public C ABI for hermes-node's UI compositor: POD types and enums
/// only. See docs/superpowers/specs/2026-09-18-ui-compositor-design.md,
/// "The native C API", for the surrounding entry points -- those (and the
/// `hnui_window` opaque handle they operate on) are declared in `hnui.h`,
/// not here, because they need a native runtime behind them and this header
/// does not. `hnui_abi.h` is included by `hnui.h` and by addons directly,
/// so it must stay compilable as plain C: no C++ headers, no default member
/// initializers, nothing that requires a constructor to run.
///
/// **The size-prefix rule.** Every struct below starts with a `uint32_t
/// size`, and this header names each struct's minimum accepted size as a
/// `HNUI_*_MIN_SIZE` constant. `size` is set by whichever side constructs
/// the struct -- an addon filling an out-parameter before a call such as
/// `hnui_get_window_state`, or the host filling an `hnui_event` it is about
/// to hand to a callback -- and read by the other side before it trusts any
/// field beyond the ones the *reading* side's own header version requires.
/// A struct may grow by appending fields after a release: an addon
/// compiled against an older, smaller header still passes its old (smaller)
/// `size`, which stays >= the *current* minimum as long as the minimum
/// constant for the version it was compiled against is never redefined out
/// from under it -- see the per-struct comments for the rule that keeps
/// that true. The host must write at most `size` bytes and at least the
/// minimum; a `size` below the minimum must be rejected rather than read,
/// via `hnui_size_ok()` below.
///
/// **Borrowed pointers.** A few fields are `const char *` (or an array of
/// them) borrowed from storage the callee does not own. Each such field
/// says, at the field, how long it stays valid -- never only in this
/// file-level comment.
///
/// **C99 is the floor.** `-std=c89` rejects `inline` (used by
/// `hnui_size_ok()` below) outright, and separately warns on `//` comments
/// and on the trailing comma every enum here has before its `_FORCE_I32`
/// terminator. Verified by compiling this exact header under both
/// `-std=c99` and `-std=c11`, C and C++, with identical struct sizes in
/// both languages -- `unittests/HnuiAbiCProbe.c` (built at `-std=c99
/// -pedantic`) is the C half of that check, and
/// `HnuiAbiTest.cpp`'s `CAndCxxCompilationsAgreeOnLayout` is the
/// comparison. Nothing in CMake enforces plain-C compilability on its own;
/// that C translation unit existing at all is the enforcement -- a
/// `namespace`, a default member initializer, or an unguarded
/// `_Static_assert` breaks its build long before any test assertion would
/// notice.

#include <stddef.h>
#include <stdint.h>
#ifndef __cplusplus
#include <stdbool.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/// Bumped whenever a change here is one an already-compiled addon cannot
/// tolerate (a field removed or repurposed, a callback signature changed --
/// growth by appending a field, guarded by the size-prefix rule above, is
/// not one of those). `hnui_api_version()` (declared in `hnui.h`) returns
/// the value the *running host* was built with; an addon compares it
/// against the `HNUI_ABI_VERSION` it was compiled against and refuses to
/// proceed on a mismatch, the same way a `dlopen`'d addon into plain
/// hermes-node (no `hnui_*` symbols at all) already fails to load.
#define HNUI_ABI_VERSION 1

/// Opaque; a window belongs to one owner thread and every `hnui_*` entry
/// point that takes one is documented (in `hnui.h`) as to which thread may
/// call it. Forward-declared here, rather than in `hnui.h`, only because
/// the callback typedefs below need the name.
typedef struct hnui_window hnui_window;

/// True when `providedSize` (a caller-filled `size` field read back from
/// one of the structs below) is large enough that every field through
/// `minSize` was actually written by whoever filled it in. Data at or past
/// `providedSize` was never written and must not be read -- this is the
/// check that stands between "an addon built against an older, smaller
/// version of this header" (a real, supported case) and "a struct that is
/// simply garbage" (not one, and not distinguishable from the first case by
/// any other means).
static inline bool hnui_size_ok(uint32_t providedSize, uint32_t minSize) {
  return providedSize >= minSize;
}

//===========================================================================
// Status codes
//===========================================================================

/// Returned as a plain `int` (never as `enum hnui_status` by value) by the
/// `hnui_*` entry points declared in `hnui.h` that can fail, for the same
/// C-ABI-stability reason `hnui_target`'s fields below mirror Sokol's
/// handles as a bare `uint32_t` rather than a Sokol type: an `enum`'s
/// underlying type is not part of the C ABI two different compilers are
/// guaranteed to agree on, where `int` is. `HNUI_OK` is zero, matching the
/// convention `napi_status` and libuv's own return codes already use
/// throughout this codebase -- unlike `UiEventType::kInvalid` inside the
/// compositor, whose zero deliberately means "not a real value" for a type
/// that is queued and drained rather than returned; a status code has no
/// such queue to be silently misread from, so ordinary "zero is success"
/// applies here.
typedef enum hnui_status {
  HNUI_OK = 0,
  HNUI_ERROR = 1,
  /// A synchronous call (`hnui_read_clipboard_sync`) hit its deadline
  /// before an answer arrived.
  HNUI_TIMEOUT = 2,
  /// Includes a `size` field that failed `hnui_size_ok()`.
  HNUI_INVALID_ARGUMENT = 3,
  /// `hnui_read_clipboard_sync`: nothing owns the clipboard, or its owner
  /// holds no text. Not an error, and deliberately not `HNUI_OK` with an
  /// empty string either, so a caller can tell "nothing there" from "an
  /// empty string there" when it cares to.
  HNUI_UNAVAILABLE = 4,
  /// `hnui_read_clipboard_sync`: the clipboard's owner offered more than
  /// `HNUI_CLIPBOARD_MAX_BYTES - 1` bytes, or offered it incrementally.
  HNUI_TOO_LARGE = 5,
  /// Never a real value. Standard C leaves an enum's underlying type
  /// implementation-defined (commonly `int`, but not guaranteed); this
  /// terminator forces every conforming compiler to pick one wide enough
  /// to hold it, the same convention Sokol uses on every public enum it
  /// declares (`_SG_PIXELFORMAT_FORCE_U32` and siblings). Since this enum
  /// is returned by value as a plain `int` rather than stored in a
  /// size-prefixed struct (see the comment above), the practical risk is
  /// smaller here than for `hnui_event_type` below -- this exists for
  /// consistency across every public `hnui_*` enum rather than because a
  /// status code sits behind a width-sensitive offset.
  HNUI_STATUS_FORCE_I32 = 0x7FFFFFFF,
} hnui_status;

//===========================================================================
// hnui_event
//===========================================================================

/// Mirrors `hermes::node_compat::UiEventType`
/// (`include/hermes/node-compat/compositor/channels.h`) value for value,
/// deliberately: the two are kept in the same order so the conversion in
/// `hnui_event_convert.h` is a table lookup, not a hand-maintained switch
/// that can drift. `HNUI_EVENT_INVALID` is zero for the identical reason
/// `UiEventType::kInvalid` is zero -- carried into this ABI enum rather
/// than starting it at a meaningful value, so a zero-initialized or
/// half-filled `hnui_event` (for instance one whose `size` failed
/// `hnui_size_ok()` and was therefore never read further) cannot be
/// mistaken for a real mouse move.
typedef enum hnui_event_type {
  HNUI_EVENT_INVALID = 0,
  HNUI_EVENT_MOUSE_MOVE,
  HNUI_EVENT_MOUSE_DOWN,
  HNUI_EVENT_MOUSE_UP,
  HNUI_EVENT_MOUSE_WHEEL,
  HNUI_EVENT_KEY_DOWN,
  HNUI_EVENT_KEY_UP,
  HNUI_EVENT_TEXT_INPUT,
  HNUI_EVENT_RESIZE,
  HNUI_EVENT_FOCUS,
  HNUI_EVENT_BLUR,
  HNUI_EVENT_CLOSE_REQUESTED,
  HNUI_EVENT_FILE_DROP,
  HNUI_EVENT_MOUSE_ENTER,
  HNUI_EVENT_MOUSE_LEAVE,
  HNUI_EVENT_MINIMIZE,
  HNUI_EVENT_RESTORE,
  HNUI_EVENT_PASTE,
  /// Never a real value. See `HNUI_STATUS_FORCE_I32` above for why this
  /// terminator exists on every public `hnui_*` enum; it matters more here
  /// than there, because `hnui_event::type` below is declared as a plain
  /// `int32_t` specifically so the field never depends on this enum's
  /// underlying-type width agreeing between compilers in the first place --
  /// this terminator is the belt to that field's suspenders.
  HNUI_EVENT_TYPE_FORCE_I32 = 0x7FFFFFFF,
} hnui_event_type;

/// Bits of `hnui_event::modifiers`. These are this ABI's own values, not
/// Sokol's: the host maps them bit by bit, so a renumbering inside Sokol
/// cannot change what an addon compiled against this header sees.
#define HNUI_MOD_SHIFT 0x1
#define HNUI_MOD_CTRL 0x2
#define HNUI_MOD_ALT 0x4
#define HNUI_MOD_META 0x8
#define HNUI_MOD_LMB 0x100
#define HNUI_MOD_RMB 0x200
#define HNUI_MOD_MMB 0x400

/// Values of `hnui_event::button`, in DOM `MouseEvent.button` numbering.
#define HNUI_BUTTON_LEFT 0
#define HNUI_BUTTON_MIDDLE 1
#define HNUI_BUTTON_RIGHT 2

/// Limits on a `HNUI_EVENT_FILE_DROP`. At most `HNUI_DROP_MAX_FILES` paths
/// are delivered; a drop of more delivers the first that many and silently
/// leaves out the rest. Each path holds at most `HNUI_DROP_MAX_PATH_BYTES -
/// 1` bytes after percent-decoding, plus its terminator, and **one
/// oversized path discards the entire drop**: no `HNUI_EVENT_FILE_DROP` is
/// delivered at all, for any of its files. That is what Sokol does on X11
/// (`_sapp_x11_parse_dropped_files_list` clears the whole drop buffer and
/// reports failure, logging `DROPPED_FILE_PATH_TOO_LONG` to stderr), and a
/// drop with a URI that is not `file://` is discarded the same way.
#define HNUI_DROP_MAX_FILES 32
#define HNUI_DROP_MAX_PATH_BYTES 4096

/// The size of the clipboard buffer, terminator included: a UTF-8 string of
/// at least this many bytes cannot be written to the clipboard. The limit
/// is enforced where a write is accepted (`ui.writeClipboard` throws a
/// `RangeError`), because handing Sokol a string that does not fit makes it
/// take ownership of the selection with an empty buffer. Also the buffer
/// size `sapp_desc::clipboard_size` is given, so the two cannot disagree.
#define HNUI_CLIPBOARD_MAX_BYTES (1u << 20)

/// Which fields of an `hnui_event` mean something, by `type`. Every field
/// not listed for a type is zero (NULL for the pointers).
///
///   MOUSE_MOVE, MOUSE_ENTER, MOUSE_LEAVE
///                 x, y, dx, dy (pointer movement), modifiers
///   MOUSE_DOWN, MOUSE_UP
///                 x, y, dx, dy, modifiers, button
///   MOUSE_WHEEL   x, y, modifiers; dx, dy carry the SCROLL distance, as
///                 Sokol reports it and unscaled -- a quantity, not a
///                 position, so not in logical pixels -- rather than
///                 pointer movement
///   KEY_DOWN, KEY_UP
///                 key_code, modifiers, repeat
///   TEXT_INPUT    text (one character), modifiers, repeat
///   PASTE         text
///   FILE_DROP     x, y, modifiers, paths, path_count
///   RESIZE, FOCUS, BLUR, MINIMIZE, RESTORE, CLOSE_REQUESTED
///                 none: the state they change is read with
///                 `hnui_get_window_state`, which already reports it when
///                 the listener runs
///
/// The public, flat counterpart of the internal `UiEvent`
/// (`channels.h`). `UiEvent` owns its payloads (`std::string`,
/// `std::vector`) because it is queued on the platform thread and drained
/// on another; a type crossing the C boundary cannot own C++ containers, so
/// this one borrows instead. Built by `hnui_event_convert.h`'s
/// `HnuiEventConverter`, which states exactly how long the borrow lasts.
typedef struct hnui_event {
  /// Set by the host to `sizeof(hnui_event)` as of the header it was built
  /// against when it constructs one of these; read by the addon's callback
  /// before trusting any field this addon's own (possibly newer) header
  /// added after `HNUI_EVENT_MIN_SIZE`.
  uint32_t size;

  /// Holds an `hnui_event_type` value, but is deliberately declared
  /// `int32_t` rather than `hnui_event_type` itself. This struct is read
  /// at fixed byte offsets on both sides of the ABI boundary: if this
  /// field's width ever disagreed between the host's compiler and an
  /// addon's -- and standard C leaves an enum's underlying type
  /// implementation-defined, so nothing rules that out -- every field
  /// after it (`x`, `y`, `dx`, `dy`, both borrowed pointers) would shift,
  /// silently, while `size` and `hnui_size_ok()` keep reporting success,
  /// because neither one can see a width disagreement in a field that
  /// isn't `size` itself. `int32_t` has a width both the C and C++
  /// standards guarantee; `hnui_event_type` alone does not, even with
  /// `HNUI_EVENT_TYPE_FORCE_I32` on the enum, which narrows a compiler's
  /// practical choice but does not mandate one. Convert at every use site:
  /// `(hnui_event_type)event.type` in C, `static_cast<hnui_event_type>(
  /// event.type)` in C++.
  int32_t type;

  /// Pointer position in logical pixels, and the movement since the
  /// previous event -- see `UiEvent`'s own fields for why the two are kept
  /// separate rather than one being derived from the other.
  double x;
  double y;
  double dx;
  double dy;

  /// `HNUI_BUTTON_*`, DOM numbering.
  int32_t button;
  /// `HNUI_MOD_*` bits.
  int32_t modifiers;
  /// `HNUI_EVENT_KEY_DOWN` / `KEY_UP` only: Sokol's `sapp_keycode` value,
  /// which is GLFW's key numbering (`GLFW_KEY_A` is 65, `GLFW_KEY_F1`
  /// 290). A physical key, not a character: what the key produces arrives
  /// as `HNUI_EVENT_TEXT_INPUT`.
  int32_t key_code;

  /// `HNUI_EVENT_TEXT_INPUT` (the character) and `HNUI_EVENT_PASTE` (the
  /// clipboard text); NULL otherwise. Borrowed from the
  /// `UiEvent` this was converted from -- specifically from that
  /// `UiEvent`'s own `std::string` storage -- and valid only while both the
  /// converter that produced this event AND that source `UiEvent` are
  /// still alive, which in practice means: for the duration of the one
  /// callback dispatch this event was built for. It must not be retained
  /// past that callback returning.
  const char *text;

  /// `HNUI_EVENT_FILE_DROP` only; NULL (with `path_count` 0) otherwise. An
  /// array of `path_count` NUL-terminated UTF-8 strings. Both the array
  /// itself and every string it points to are borrowed the same way `text`
  /// is, and valid for exactly the same span: the one callback dispatch
  /// this event was built for, and no longer.
  const char *const *paths;
  uint32_t path_count;

  /// Nonzero for an auto-repeated key event. Added after
  /// `HNUI_ABI_VERSION` 1's first draft but before any release, so
  /// `HNUI_EVENT_MIN_SIZE` moves to include it; from the first release on,
  /// a field appended here gets a `HNUI_EVENT_MIN_SIZE_V<n>` of its own.
  uint32_t repeat;
} hnui_event;

/// The minimum `size` an `hnui_event` must carry for every field above to
/// have been written. Computed from the offset and width of the final
/// field in *this* version of the struct, not from `sizeof(hnui_event)`,
/// so struct padding at the end can never inflate it.
///
/// If a later ABI version appends a field, this constant is untouched --
/// it still names exactly what an addon compiled against this version
/// filled in -- and the new version adds its own
/// `HNUI_EVENT_MIN_SIZE_V<n>` alongside it, one constant per version,
/// never a redefinition of an earlier one.
#define HNUI_EVENT_MIN_SIZE \
  ((uint32_t)(offsetof(hnui_event, repeat) + sizeof(uint32_t)))

/// The window's state, read with `hnui_get_window_state` -- it is not part
/// of an `hnui_event`. Window state changes travel with the event that
/// caused them and are applied before any listener for that event runs, so
/// a `HNUI_EVENT_RESIZE` (or FOCUS, BLUR, ...) listener that calls
/// `hnui_get_window_state` reads the new state.
typedef struct hnui_window_state {
  /// See `hnui_event::size`.
  uint32_t size;

  /// Logical pixels -- what the window manager and the program both call
  /// "the window size". `framebuffer_width`/`height` below are the
  /// physical-pixel size the GPU surface actually is, which differ under a
  /// non-1.0 `dpi_scale`.
  int32_t width;
  int32_t height;
  int32_t framebuffer_width;
  int32_t framebuffer_height;
  double dpi_scale;

  bool focused;
  bool fullscreen;
  bool minimized;
} hnui_window_state;

#define HNUI_WINDOW_STATE_MIN_SIZE \
  ((uint32_t)(offsetof(hnui_window_state, minimized) + sizeof(bool)))

//===========================================================================
// hnui_target
//===========================================================================

/// Mirrors the layout Sokol uses for every public handle type it declares
/// (`typedef struct sg_X { uint32_t id; } sg_X;`) -- `sg_view` included: a
/// single opaque id. Declared here, rather than borrowed from
/// `sokol_gfx.h` directly, because this header must stay includable by an
/// addon that has not included `sokol_gfx.h` at all, such as one that only
/// ever forwards this id back into calls it makes through the exported
/// `sg_*` symbols.
///
/// **Checked against the vendored `sg_view`, not assumed.** Static
/// assertions in `lib/compositor-gpu/gl_backend.cpp` and
/// `unittests/HnuiAbiTest.cpp` compare this type's size, alignment and
/// field offset with `sokol_gfx.h`'s (pinned commit `c0db757`), and
/// `HnuiAbiTest` checks that `sg_pixel_format`'s values fit
/// `hnui_target::color_format`, so a Sokol bump that changes either fails
/// the build.
///
/// An addon that has included `sokol_gfx.h` converts **by value**, never
/// by pointer cast: the two types have identical layout, but reinterpreting
/// one pointer as the other still violates C's effective-type rule even
/// when the layouts agree, and addons build at `-O2`, where that rule is
/// not a formality. The safe conversion costs nothing, since this is a
/// 4-byte handle passed by value -- `sg_view v; v.id =
/// target.color_view.id;` -- or, in C only (unlike C++, reading a union
/// member other than the last one written is well-defined there), `union {
/// hnui_gpu_view hnui; sg_view sg; }`.
typedef struct hnui_gpu_view {
  uint32_t id;
} hnui_gpu_view;

/// Returned by `hnui_frame_target`. Yields both a color-attachment view and
/// a resolve-attachment view for the current slot -- Sokol distinguishes
/// the two (`sg_attachments.colors[]` versus `.resolves[]`), and an MSAA
/// renderer needs the resolve view specifically -- plus the target's pixel
/// size, its color format, its sample count and the generation it belongs
/// to. Borrowed for the duration of the one frame callback it was obtained
/// in: it must not be retained, destroyed, or used after that callback
/// returns.
typedef struct hnui_target {
  /// See `hnui_event::size`.
  uint32_t size;

  hnui_gpu_view color_view;
  hnui_gpu_view resolve_view;

  /// Physical pixels.
  int32_t width;
  int32_t height;

  /// The value of Sokol's `sg_pixel_format` for the swapchain's color
  /// format, carried as a bare `int32_t` for the same reason
  /// `hnui_gpu_view` mirrors `sg_view` rather than naming it: this header
  /// does not depend on `sokol_gfx.h`. An addon that has included it may
  /// cast this back to `sg_pixel_format` directly -- the two are both
  /// plain C enums with `int` as their underlying type.
  int32_t color_format;
  int32_t sample_count;

  /// Matches `hermes::node_compat::GenerationId::value`
  /// (`compositor/surface_table.h`), which is also `uint32_t`.
  uint32_t generation;
} hnui_target;

#define HNUI_TARGET_MIN_SIZE \
  ((uint32_t)(offsetof(hnui_target, generation) + sizeof(uint32_t)))

//===========================================================================
// hnui_cursor
//===========================================================================

/// Named after the standard cursor taxonomy shared by CSS's `cursor`
/// property and Sokol's own `sapp_mouse_cursor` -- not required to share
/// either one's numeric values: the host maps each to Sokol's cursor through
/// an explicit table (`sappCursorFor` in tools/hermes-node-ui/
/// ui_module.cpp), never a cast.
/// `HNUI_CURSOR_DEFAULT` is zero because it is a legitimate steady state (a
/// window that never calls `hnui_set_cursor` still has one), unlike
/// `HNUI_EVENT_INVALID` above, whose zero exists to flag data nobody wrote.
typedef enum hnui_cursor {
  HNUI_CURSOR_DEFAULT = 0,
  HNUI_CURSOR_ARROW,
  HNUI_CURSOR_IBEAM,
  HNUI_CURSOR_CROSSHAIR,
  HNUI_CURSOR_POINTING_HAND,
  HNUI_CURSOR_RESIZE_EW,
  HNUI_CURSOR_RESIZE_NS,
  HNUI_CURSOR_RESIZE_NWSE,
  HNUI_CURSOR_RESIZE_NESW,
  HNUI_CURSOR_RESIZE_ALL,
  HNUI_CURSOR_NOT_ALLOWED,
  /// Never a real value. See `HNUI_STATUS_FORCE_I32` above.
  HNUI_CURSOR_FORCE_I32 = 0x7FFFFFFF,
} hnui_cursor;

//===========================================================================
// Callback typedefs
//===========================================================================

/// Registered with `hnui_request_animation_frame`. `w` is the window the
/// callback was registered on; `user` is the opaque pointer passed to that
/// call; `timestamp_ms` is shared with the same tick's JS-side
/// `requestAnimationFrame` callbacks. Call `hnui_frame_target` from inside
/// this callback to obtain the frame's render target -- it returns false
/// outside one.
typedef void (*hnui_frame_cb)(hnui_window *w, void *user, double timestamp_ms);

/// Registered with `hnui_add_event_listener`. `event` is borrowed for the
/// duration of this call only -- see `hnui_event`'s own field comments for
/// exactly which parts of it are themselves borrowed pointers.
typedef void (
    *hnui_event_cb)(hnui_window *w, void *user, const hnui_event *event);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // HERMES_NODE_COMPAT_UI_HNUI_ABI_H
