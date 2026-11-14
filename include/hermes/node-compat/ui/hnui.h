/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_NODE_COMPAT_UI_HNUI_H
#define HERMES_NODE_COMPAT_UI_HNUI_H

/// The public C API for hermes-node's UI compositor: the entry points an
/// addon calls. The POD types and enums they operate on live in
/// `hnui_abi.h`, which this header includes and which an addon may also
/// include on its own.
///
/// **This is the minimal subset, not the API** (plan Task 10 Step 1). The
/// design's "The native C API" section lists sixteen entry points; three
/// are declared here, because three are what the reference addon needs to
/// draw a frame, and the rest arrive with Task 14 together with the
/// conformance mode that gives each of them a caller. Declaring the other
/// thirteen now and implementing them later would put a symbol in this
/// header that links and then does nothing -- the swallow-and-continue
/// shape this codebase removes on sight -- so they are absent rather than
/// stubbed. An addon that references one fails to load, naming it.
///
/// **Two things this header does NOT yet carry, deliberately**, both owned
/// by Task 14 and both recorded here so the gap is visible rather than
/// forgotten: `hnui_api_version()` and the `HNUI_CHECK_VERSION(env)` macro
/// that spec:838-841 requires, which must bake in the vendored Sokol
/// commit alongside `HNUI_ABI_VERSION`. Until they exist, an addon and a
/// host that disagree about Sokol's ABI are caught by nothing.
///
/// # Threading
///
/// Every entry point here belongs to the **window's owner thread** -- the
/// JS thread that called `createWindow`, which is also the thread that
/// owns the producing `sg_*` instance and issues every drawing call. The
/// platform thread has no business calling any of them. A window handle
/// carries its owner thread so that a wrong-thread call can be refused by
/// name rather than corrupting the other instance's state (spec: Errors,
/// "Misuse of a checked `hnui_*` call from the wrong thread: abort naming
/// the function").
///
/// # Linking
///
/// The host exports these symbols from its own executable
/// (`hermes_node_link_setup()` in `tools/hermes-node/CMakeLists.txt` gives
/// hermes-node-ui the same `-rdynamic` treatment hermes-node has), so a
/// `dlopen`ed addon resolves them at load time. It exports the producing
/// instance's `sg_*` symbols the same way, which is what lets an addon
/// link against `sokol_gfx.h`'s declarations without carrying an
/// implementation of its own.

#include <hermes/node-compat/ui/hnui_abi.h>

#include <node_api.h>

#ifdef __cplusplus
extern "C" {
#endif

/// The window this `napi_env` owns, or NULL if it has none.
///
/// NULL is the ordinary answer for a runtime that never called
/// `createWindow`, or called it and was rejected -- not an error, and an
/// addon that can render headless may carry on. It is also what an addon
/// loaded into plain hermes-node would get, except that such an addon does
/// not load at all: the symbol is not there to resolve.
///
/// The handle is owned by the host and lives as long as the window does.
/// The caller must not free it.
hnui_window *hnui_get_window(napi_env env);

/// The render target for the frame currently being drawn: both view
/// handles, the target's pixel size, its color format, its sample count
/// and its generation (see `hnui_target`).
///
/// **Calling this is what marks the frame as drawn.** A tick in which no
/// callback asks for a target rendered nothing, so its slot is reclaimed
/// and the tick machine returns to idle immediately rather than waiting
/// for a publication that will never come (spec: Frames, "A tick in which
/// nothing drew ... returns to IDLE immediately"). That is why this is a
/// single call that both answers and records, rather than a query plus a
/// separate "I drew" flag an addon could forget to set.
///
/// \return false outside a frame callback -- which is a legitimate
///   question with a legitimate answer, not a misuse -- false if
///   \p out carries a `size` below `HNUI_TARGET_MIN_SIZE`, and false for a
///   NULL \p w, which is the same ordinary answer `hnui_get_window()` gives
///   and must not be a fatal one: an addon told it has no window is
///   entitled to hand that NULL straight back. A wrong-thread call on a
///   real window aborts instead.
///
/// The views are **borrowed for the duration of this frame callback
/// only**. They must not be retained, destroyed, or used after it returns:
/// the slot they name goes back into rotation, and a later frame's
/// callback will be drawing into it.
bool hnui_frame_target(hnui_window *w, hnui_target *out);

/// Register \p cb to run on the next frame tick, with \p user as its
/// opaque argument. Native callbacks share the tick and the timestamp with
/// the JS-side `requestAnimationFrame` callbacks and run in registration
/// order among them.
///
/// One tick, one call: a callback registered from inside a callback runs
/// on the next tick, exactly as `requestAnimationFrame` does in a browser.
///
/// \return an id for `hnui_cancel_animation_frame` -- which Task 14 adds,
///   so in this build the id is returnable and not yet usable. It is
///   returned anyway rather than made `void`, because the signature is the
///   one spec:757 fixes and changing it later would break every addon
///   compiled against this header. Returns 0 -- "no id" -- for a NULL \p w
///   or a NULL \p cb, for the reason `hnui_frame_target` returns false for
///   the first of those.
int hnui_request_animation_frame(hnui_window *w, hnui_frame_cb cb, void *user);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // HERMES_NODE_COMPAT_UI_HNUI_H
