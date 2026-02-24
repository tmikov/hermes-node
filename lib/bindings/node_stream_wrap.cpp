/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <hermes/node-compat/bindings/libuv_stream_base.h>
#include <hermes/node-compat/bindings/node_stream_wrap.h>
#include <hermes/node-compat/runtime/runtime_state.h>
#include <node_api.h>

#include <cstdint>
#include <cstring>

namespace hermes {
namespace node_compat {

#define NAPI_CALL(call)                                             \
  do {                                                              \
    napi_status status_ = (call);                                   \
    if (status_ != napi_ok) {                                       \
      napi_throw_error(env, nullptr, "NAPI call failed in " #call); \
      return nullptr;                                               \
    }                                                               \
  } while (0)

// StreamBaseStateFields from Node's stream_base.h
enum StreamBaseStateFields {
  kReadBytesOrError,
  kArrayBufferOffset,
  kBytesWritten,
  kLastWriteWasAsync,
  kNumStreamBaseStateFields,
};

// WriteWrap constructor — creates a plain JS object.
// The JS side sets .handle, .oncomplete, .callback, etc.
// The native stream methods create their own uv_write_t internally.
static napi_value writeWrapConstructor(napi_env env, napi_callback_info info) {
  napi_value thisArg;
  napi_get_cb_info(env, info, nullptr, nullptr, &thisArg, nullptr);
  return thisArg;
}

// ShutdownWrap constructor — creates a plain JS object.
static napi_value shutdownWrapConstructor(
    napi_env env,
    napi_callback_info info) {
  napi_value thisArg;
  napi_get_cb_info(env, info, nullptr, nullptr, &thisArg, nullptr);
  return thisArg;
}

napi_value initStreamWrapBinding(napi_env env, napi_value exports) {
  // --- WriteWrap constructor ---
  {
    napi_value ctor;
    NAPI_CALL(napi_create_function(
        env,
        "WriteWrap",
        NAPI_AUTO_LENGTH,
        writeWrapConstructor,
        nullptr,
        &ctor));
    NAPI_CALL(napi_set_named_property(env, exports, "WriteWrap", ctor));
  }

  // --- ShutdownWrap constructor ---
  {
    napi_value ctor;
    NAPI_CALL(napi_create_function(
        env,
        "ShutdownWrap",
        NAPI_AUTO_LENGTH,
        shutdownWrapConstructor,
        nullptr,
        &ctor));
    NAPI_CALL(napi_set_named_property(env, exports, "ShutdownWrap", ctor));
  }

  // --- Constants ---
#define SET_CONSTANT(name, val)                                       \
  do {                                                                \
    napi_value v;                                                     \
    NAPI_CALL(napi_create_int32(env, static_cast<int32_t>(val), &v)); \
    NAPI_CALL(napi_set_named_property(env, exports, #name, v));       \
  } while (0)

  SET_CONSTANT(kReadBytesOrError, kReadBytesOrError);
  SET_CONSTANT(kArrayBufferOffset, kArrayBufferOffset);
  SET_CONSTANT(kBytesWritten, kBytesWritten);
  SET_CONSTANT(kLastWriteWasAsync, kLastWriteWasAsync);

#undef SET_CONSTANT

  // --- streamBaseState: Int32Array(kNumStreamBaseStateFields), all zeros ---
  {
    napi_value ab;
    void *data;
    size_t byteLen = kNumStreamBaseStateFields * sizeof(int32_t);
    NAPI_CALL(napi_create_arraybuffer(env, byteLen, &data, &ab));
    memset(data, 0, byteLen);
    napi_value arr;
    NAPI_CALL(napi_create_typedarray(
        env, napi_int32_array, kNumStreamBaseStateFields, ab, 0, &arr));
    NAPI_CALL(napi_set_named_property(env, exports, "streamBaseState", arr));

    // Share the state array pointer with RuntimeState so native stream
    // methods can update it directly.
    getRuntimeState(env)->streamBaseState = static_cast<int32_t *>(data);
  }

  return exports;
}

} // namespace node_compat
} // namespace hermes
