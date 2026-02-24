/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <hermes/node-compat/bindings/handle_wrap_base.h>
#include <hermes/node-compat/bindings/libuv_stream_base.h>
#include <hermes/node-compat/bindings/node_tty_wrap.h>
#include <hermes/node-compat/runtime/runtime_state.h>
#include <node_api.h>
#include <uv.h>

#include <cassert>

namespace hermes {
namespace node_compat {

// ---------------------------------------------------------------------------
// TTYWrap — wraps uv_tty_t, inherits from LibuvStreamBase
// ---------------------------------------------------------------------------

class TTYWrap : public LibuvStreamBase {
 public:
  /// Construct a TTYWrap for the given file descriptor.
  /// On error, sets error info on the ctx object.
  TTYWrap(napi_env env, napi_value jsObj, int fd, napi_value ctx) : handle_() {
    int err = uv_tty_init(getRuntimeState(env)->loop, &handle_, fd, 0);
    if (err != 0) {
      // Set error info on ctx object for JS to detect.
      napi_value val;
      napi_create_int32(env, err, &val);
      napi_set_named_property(env, ctx, "errno", val);
      napi_create_string_utf8(env, uv_strerror(err), NAPI_AUTO_LENGTH, &val);
      napi_set_named_property(env, ctx, "message", val);
      napi_create_string_utf8(env, "uv_tty_init", NAPI_AUTO_LENGTH, &val);
      napi_set_named_property(env, ctx, "syscall", val);
      napi_create_string_utf8(env, uv_err_name(err), NAPI_AUTO_LENGTH, &val);
      napi_set_named_property(env, ctx, "code", val);
      return;
    }
    initStream(env, jsObj, reinterpret_cast<uv_stream_t *>(&handle_));
  }

 private:
  uv_tty_t handle_;

  // --- NAPI callbacks ---

  /// new TTY(fd, ctx)
  static napi_value New(napi_env env, napi_callback_info info) {
    napi_handle_scope scope;
    napi_open_handle_scope(env, &scope);

    size_t argc = 2;
    napi_value argv[2];
    napi_value thisObj;
    napi_get_cb_info(env, info, &argc, argv, &thisObj, nullptr);

    int32_t fd = 0;
    napi_get_value_int32(env, argv[0], &fd);

    // argv[1] is the error context object.
    napi_value ctx = argv[1];

    // Create the TTYWrap (it self-registers via initStream on success).
    new TTYWrap(env, thisObj, fd, ctx);

    napi_close_handle_scope(env, scope);
    return thisObj;
  }

  /// isTTY(fd) -> boolean
  static napi_value IsTTY(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1];
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

    int32_t fd = 0;
    napi_get_value_int32(env, argv[0], &fd);

    bool isTty = (uv_guess_handle(fd) == UV_TTY);

    napi_value result;
    napi_get_boolean(env, isTty, &result);
    return result;
  }

  /// getWindowSize(out) -> err
  /// Writes [width, height] into the out array on success.
  static napi_value GetWindowSize(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1];
    napi_value thisObj;
    napi_get_cb_info(env, info, &argc, argv, &thisObj, nullptr);

    auto *wrap = static_cast<TTYWrap *>(HandleWrapBase::unwrap(env, thisObj));
    if (!wrap) {
      napi_value result;
      napi_create_int32(env, UV_EBADF, &result);
      return result;
    }

    int width = 0, height = 0;
    int err = uv_tty_get_winsize(&wrap->handle_, &width, &height);

    if (err == 0) {
      napi_value wVal, hVal;
      napi_create_int32(env, width, &wVal);
      napi_create_int32(env, height, &hVal);
      napi_set_element(env, argv[0], 0, wVal);
      napi_set_element(env, argv[0], 1, hVal);
    }

    napi_value result;
    napi_create_int32(env, err, &result);
    return result;
  }

  /// setRawMode(flag) -> err
  static napi_value SetRawMode(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1];
    napi_value thisObj;
    napi_get_cb_info(env, info, &argc, argv, &thisObj, nullptr);

    auto *wrap = static_cast<TTYWrap *>(HandleWrapBase::unwrap(env, thisObj));
    if (!wrap) {
      napi_value result;
      napi_create_int32(env, UV_EBADF, &result);
      return result;
    }

    bool flag = false;
    napi_get_value_bool(env, argv[0], &flag);

    int err = uv_tty_set_mode(
        &wrap->handle_, flag ? UV_TTY_MODE_RAW_VT : UV_TTY_MODE_NORMAL);

    napi_value result;
    napi_create_int32(env, err, &result);
    return result;
  }

  friend napi_value initTtyWrapBinding(napi_env env, napi_value exports);
};

// ---------------------------------------------------------------------------
// initTtyWrapBinding
// ---------------------------------------------------------------------------

napi_value initTtyWrapBinding(napi_env env, napi_value exports) {
  napi_handle_scope scope;
  napi_open_handle_scope(env, &scope);

  // --- TTY constructor ---
  napi_value ttyCtor;
  napi_define_class(
      env,
      "TTY",
      NAPI_AUTO_LENGTH,
      TTYWrap::New,
      nullptr,
      0,
      nullptr,
      &ttyCtor);

  // Get the prototype and add stream + TTY-specific methods.
  napi_value prototype;
  napi_get_named_property(env, ttyCtor, "prototype", &prototype);

  // Add all stream methods (readStart/readStop/write*/shutdown etc.)
  // plus handle methods (ref/unref/hasRef/close/getAsyncId).
  LibuvStreamBase::addStreamMethods(env, prototype);

  // Add TTY-specific methods.
  napi_property_descriptor ttyProps[] = {
      {"getWindowSize",
       nullptr,
       TTYWrap::GetWindowSize,
       nullptr,
       nullptr,
       nullptr,
       napi_default,
       nullptr},
      {"setRawMode",
       nullptr,
       TTYWrap::SetRawMode,
       nullptr,
       nullptr,
       nullptr,
       napi_default,
       nullptr},
  };
  napi_define_properties(
      env, prototype, sizeof(ttyProps) / sizeof(ttyProps[0]), ttyProps);

  // --- Export TTY constructor ---
  napi_set_named_property(env, exports, "TTY", ttyCtor);

  // --- Export isTTY function ---
  napi_value isTTYFn;
  napi_create_function(
      env, "isTTY", NAPI_AUTO_LENGTH, TTYWrap::IsTTY, nullptr, &isTTYFn);
  napi_set_named_property(env, exports, "isTTY", isTTYFn);

  napi_close_handle_scope(env, scope);
  return exports;
}

} // namespace node_compat
} // namespace hermes
