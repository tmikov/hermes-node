/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <hermes/node-compat/bindings/node_async_context_frame.h>

#include <node_api.h>

namespace hermes {
namespace node_compat {

// ---------------------------------------------------------------------------
// getContinuationPreservedEmbedderData — stub returns undefined
// ---------------------------------------------------------------------------

static napi_value getContinuationPreservedEmbedderData(
    napi_env env,
    napi_callback_info /*info*/) {
  napi_value undef;
  napi_get_undefined(env, &undef);
  return undef;
}

// ---------------------------------------------------------------------------
// setContinuationPreservedEmbedderData — stub no-op
// ---------------------------------------------------------------------------

static napi_value setContinuationPreservedEmbedderData(
    napi_env env,
    napi_callback_info /*info*/) {
  napi_value undef;
  napi_get_undefined(env, &undef);
  return undef;
}

// ---------------------------------------------------------------------------
// Binding init
// ---------------------------------------------------------------------------

napi_value initAsyncContextFrameBinding(napi_env env, napi_value exports) {
  napi_value fn;

  napi_create_function(
      env,
      "getContinuationPreservedEmbedderData",
      NAPI_AUTO_LENGTH,
      getContinuationPreservedEmbedderData,
      nullptr,
      &fn);
  napi_set_named_property(
      env, exports, "getContinuationPreservedEmbedderData", fn);

  napi_create_function(
      env,
      "setContinuationPreservedEmbedderData",
      NAPI_AUTO_LENGTH,
      setContinuationPreservedEmbedderData,
      nullptr,
      &fn);
  napi_set_named_property(
      env, exports, "setContinuationPreservedEmbedderData", fn);

  return exports;
}

} // namespace node_compat
} // namespace hermes
