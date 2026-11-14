/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ui_module.h"

/// `hermes-node:ui` for a build with no GPU backend -- macOS until the Metal
/// backend lands, which is a configuration CI builds today.
///
/// **Why this file exists rather than no module at all.** `main.cpp` used to
/// register the module under `HERMES_NODE_UI_HAVE_WINDOW`, so on such a
/// build `require('hermes-node:ui')` failed with `MODULE_NOT_FOUND`: the
/// error a misspelled module name produces, for the one feature the binary
/// exists to provide. `UiHost::run()` already has a diagnostic for the same
/// situation -- "a window was requested, but hermes-node-ui has no window
/// support yet." -- and the two paths have to agree in substance. So the
/// module is registered everywhere and this is what answers for it where
/// there is nothing to open a window on.
///
/// **The two sentences are not identical, and nothing makes them so.** They
/// share the phrase "no window support", which is what
/// `test/ui-no-window-support.js` matches; the rest differs because one is
/// answering a request that arrived and the other is refusing before any
/// can. Do not read this comment, or the commit that added it, as saying
/// the string is reused -- an earlier draft of both said exactly that, and
/// it was wrong.
///
/// Compiled ONLY where there is no backend (see this directory's
/// CMakeLists.txt), which means a Linux build never sees it and no lit test
/// on Linux can execute it. `test/ui-no-window-support.js` asserts its
/// behaviour and is gated `REQUIRES: ui && !ui-gpu` so it runs exactly where
/// this does; after touching this file, syntax-check it by hand:
///
///     clang++ -fsyntax-only -Iinclude -Ihermes/include/hermes/napi \
///         -Itools/hermes-node-ui tools/hermes-node-ui/ui_module_nowindow.cpp

namespace hermes {
namespace node_compat {
namespace {

/// The refusal, in one place, because two copies of a sentence that a test
/// matches drift apart. Contains "no window support" verbatim, which is what
/// `UiHost::run()` prints and what the test matches on.
const char kNoWindowSupport[] =
    "hermes-node-ui has no window support in this build: no GPU backend was "
    "compiled for this platform, so there is nothing to open a window on. "
    "See docs/superpowers/specs/2026-09-18-ui-compositor-design.md. The "
    "module itself is present and this rejection is why -- a require() that "
    "failed instead would report MODULE_NOT_FOUND, which is what a "
    "misspelled module name reports.";

/// `createWindow`: a REJECTED PROMISE, not a synchronous throw.
///
/// The shape matches the real `createWindow`, which rejects when the request
/// fails and throws only for a misuse knowable without doing anything (a
/// second window, a non-positive size). Asking for a window on a platform
/// that has no backend is a request that cannot be served, not a misuse of
/// the API -- and a program that `await`s it sees the same thing either way,
/// while one that attached a `.catch()` sees the right one.
napi_value rejectNoWindow(napi_env env, napi_callback_info info) {
  (void)info;

  napi_deferred deferred = nullptr;
  napi_value promise = nullptr;
  napi_value message = nullptr;
  napi_value error = nullptr;

  // Every step is checked, and the fallback is a synchronous throw carrying
  // the same sentence: a caller must not be able to end up with neither a
  // rejection nor an exception, which is what returning nullptr without
  // throwing would produce.
  if (napi_create_string_utf8(
          env, kNoWindowSupport, NAPI_AUTO_LENGTH, &message) != napi_ok ||
      napi_create_error(env, nullptr, message, &error) != napi_ok ||
      napi_create_promise(env, &deferred, &promise) != napi_ok) {
    napi_throw_error(env, nullptr, kNoWindowSupport);
    return nullptr;
  }

  if (napi_reject_deferred(env, deferred, error) != napi_ok) {
    napi_throw_error(env, nullptr, kNoWindowSupport);
    return nullptr;
  }
  return promise;
}

/// `requestAnimationFrame`: a synchronous throw, matching what the real
/// module does when no window exists yet. Exported at all -- rather than
/// left off the object -- so the failure names the situation instead of
/// being a TypeError about calling undefined.
napi_value throwNoWindow(napi_env env, napi_callback_info info) {
  (void)info;
  napi_throw_error(env, nullptr, kNoWindowSupport);
  return nullptr;
}

/// Same helper, same shape, as `ui_module.cpp`'s. Duplicated rather than
/// shared because the two files never both exist in one build, and a header
/// holding four lines would have to be included by both to save nothing.
bool exportFunction(
    napi_env env,
    napi_value exports,
    const char *name,
    napi_callback callback) {
  napi_value fn = nullptr;
  if (napi_create_function(
          env, name, NAPI_AUTO_LENGTH, callback, nullptr, &fn) != napi_ok) {
    return false;
  }
  return napi_set_named_property(env, exports, name, fn) == napi_ok;
}

} // namespace

napi_value initUiModule(napi_env env, napi_value exports) {
  if (!exportFunction(env, exports, "createWindow", rejectNoWindow))
    return nullptr;
  if (!exportFunction(env, exports, "requestAnimationFrame", throwNoWindow))
    return nullptr;
  // No `__captureFrame`, with or without the test hooks: it captures a
  // presented frame, and this build presents none.
  return exports;
}

} // namespace node_compat
} // namespace hermes
