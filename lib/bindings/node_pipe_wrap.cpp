/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <hermes/node-compat/bindings/node_pipe_wrap.h>
#include <node_api.h>

namespace hermes {
namespace node_compat {

// ---------------------------------------------------------------------------
// Pipe stub — minimal exports so net.js can load.
// The full implementation is deferred to step N5.11.
// ---------------------------------------------------------------------------

/// Stub Pipe constructor — throws an error if actually called.
static napi_value PipeNew(napi_env env, napi_callback_info info) {
  napi_throw_error(
      env,
      "ERR_NOT_IMPLEMENTED",
      "Pipe is not yet implemented (step N5.11)");
  return nullptr;
}

/// Stub PipeConnectWrap constructor.
static napi_value PipeConnectWrapNew(napi_env env, napi_callback_info info) {
  napi_value thisObj;
  napi_get_cb_info(env, info, nullptr, nullptr, &thisObj, nullptr);
  return thisObj;
}

napi_value initPipeWrapBinding(napi_env env, napi_value exports) {
  // --- Pipe constructor ---
  napi_value pipeCtor;
  napi_define_class(
      env,
      "Pipe",
      NAPI_AUTO_LENGTH,
      PipeNew,
      nullptr,
      0,
      nullptr,
      &pipeCtor);
  napi_set_named_property(env, exports, "Pipe", pipeCtor);

  // --- PipeConnectWrap constructor ---
  napi_value connectWrapCtor;
  napi_define_class(
      env,
      "PipeConnectWrap",
      NAPI_AUTO_LENGTH,
      PipeConnectWrapNew,
      nullptr,
      0,
      nullptr,
      &connectWrapCtor);
  napi_set_named_property(env, exports, "PipeConnectWrap", connectWrapCtor);

  // --- Constants ---
  napi_value constants;
  napi_create_object(env, &constants);

  napi_value val;
  napi_create_int32(env, 0, &val);
  napi_set_named_property(env, constants, "SOCKET", val);
  napi_create_int32(env, 1, &val);
  napi_set_named_property(env, constants, "SERVER", val);
  napi_create_int32(env, 2, &val);
  napi_set_named_property(env, constants, "IPC", val);
  napi_create_int32(env, 1, &val);
  napi_set_named_property(env, constants, "UV_READABLE", val);
  napi_create_int32(env, 2, &val);
  napi_set_named_property(env, constants, "UV_WRITABLE", val);

  napi_set_named_property(env, exports, "constants", constants);

  return exports;
}

} // namespace node_compat
} // namespace hermes
