/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <hermes/node-compat/bindings/node_async_wrap.h>
#include <node_api.h>

#include <cstdint>
#include <cstring>

namespace hermes {
namespace node_compat {

#define NAPI_CALL(call)                                              \
  do {                                                               \
    napi_status status_ = (call);                                    \
    if (status_ != napi_ok) {                                        \
      napi_throw_error(env, nullptr, "NAPI call failed in " #call);  \
      return nullptr;                                                \
    }                                                                \
  } while (0)

// ---------------------------------------------------------------------------
// AsyncHooks::Fields constants (from Node's env.h)
// ---------------------------------------------------------------------------
enum Fields {
  kInit,
  kBefore,
  kAfter,
  kDestroy,
  kPromiseResolve,
  kTotals,
  kCheck,
  kStackLength,
  kUsesExecutionAsyncResource,
  kFieldsCount,
};

// ---------------------------------------------------------------------------
// AsyncHooks::UidFields constants (from Node's env.h)
// ---------------------------------------------------------------------------
enum UidFields {
  kExecutionAsyncId,
  kTriggerAsyncId,
  kAsyncIdCounter,
  kDefaultTriggerAsyncId,
  kUidFieldsCount,
};

// ---------------------------------------------------------------------------
// Provider types (from Node's async_wrap.h NODE_ASYNC_PROVIDER_TYPES)
// ---------------------------------------------------------------------------
#define NODE_ASYNC_PROVIDER_TYPES(V) \
  V(NONE)                           \
  V(DIRHANDLE)                      \
  V(DNSCHANNEL)                     \
  V(ELDHISTOGRAM)                   \
  V(FILEHANDLE)                     \
  V(FILEHANDLECLOSEREQ)             \
  V(BLOBREADER)                     \
  V(FSEVENTWRAP)                    \
  V(FSREQCALLBACK)                  \
  V(FSREQPROMISE)                   \
  V(GETADDRINFOREQWRAP)             \
  V(GETNAMEINFOREQWRAP)             \
  V(HEAPSNAPSHOT)                   \
  V(HTTP2SESSION)                   \
  V(HTTP2STREAM)                    \
  V(HTTP2PING)                      \
  V(HTTP2SETTINGS)                  \
  V(HTTPINCOMINGMESSAGE)            \
  V(HTTPCLIENTREQUEST)              \
  V(LOCKS)                          \
  V(JSSTREAM)                       \
  V(JSUDPWRAP)                      \
  V(MESSAGEPORT)                    \
  V(PIPECONNECTWRAP)                \
  V(PIPESERVERWRAP)                 \
  V(PIPEWRAP)                       \
  V(PROCESSWRAP)                    \
  V(PROMISE)                        \
  V(QUERYWRAP)                      \
  V(QUIC_ENDPOINT)                  \
  V(QUIC_LOGSTREAM)                 \
  V(QUIC_PACKET)                    \
  V(QUIC_SESSION)                   \
  V(QUIC_STREAM)                    \
  V(QUIC_UDP)                       \
  V(SHUTDOWNWRAP)                   \
  V(SIGNALWRAP)                     \
  V(STATWATCHER)                    \
  V(STREAMPIPE)                     \
  V(TCPCONNECTWRAP)                 \
  V(TCPSERVERWRAP)                  \
  V(TCPWRAP)                        \
  V(TTYWRAP)                        \
  V(UDPSENDWRAP)                    \
  V(UDPWRAP)                        \
  V(SIGINTWATCHDOG)                 \
  V(WORKER)                         \
  V(WORKERCPUPROFILE)               \
  V(WORKERCPUUSAGE)                 \
  V(WORKERHEAPPROFILE)              \
  V(WORKERHEAPSNAPSHOT)             \
  V(WORKERHEAPSTATISTICS)           \
  V(WRITEWRAP)                      \
  V(ZLIB)

enum ProviderType {
#define V(p) PROVIDER_##p,
  NODE_ASYNC_PROVIDER_TYPES(V)
#undef V
      PROVIDERS_LENGTH,
};

// ---------------------------------------------------------------------------
// Stub callback functions — all no-ops
// ---------------------------------------------------------------------------

/// setupHooks(hooks) — stores init/before/after/destroy/promise_resolve
/// callbacks. No-op since we don't fire async hooks.
static napi_value setupHooks(napi_env env, napi_callback_info /*info*/) {
  return nullptr;
}

/// setCallbackTrampoline(trampoline) — sets the callback trampoline
/// function. No-op.
static napi_value setCallbackTrampoline(
    napi_env env,
    napi_callback_info /*info*/) {
  return nullptr;
}

/// pushAsyncContext(asyncId, triggerAsyncId) — pushes an async context
/// onto the stack. No-op stub.
static napi_value pushAsyncContext(
    napi_env env,
    napi_callback_info /*info*/) {
  return nullptr;
}

/// popAsyncContext(asyncId) — pops an async context from the stack. No-op.
static napi_value popAsyncContext(napi_env env, napi_callback_info /*info*/) {
  return nullptr;
}

/// executionAsyncResource(index) — returns the async resource for
/// the given stack index. Returns undefined.
static napi_value executionAsyncResource(
    napi_env env,
    napi_callback_info /*info*/) {
  napi_value undef;
  napi_get_undefined(env, &undef);
  return undef;
}

/// clearAsyncIdStack() — clears the async ID stack. No-op.
static napi_value clearAsyncIdStack(
    napi_env env,
    napi_callback_info /*info*/) {
  return nullptr;
}

/// queueDestroyAsyncId(asyncId) — queues an async ID for destroy hook.
/// No-op.
static napi_value queueDestroyAsyncId(
    napi_env env,
    napi_callback_info /*info*/) {
  return nullptr;
}

/// setPromiseHooks(init, before, after, resolve) — sets promise lifecycle
/// hooks. No-op.
static napi_value setPromiseHooks(
    napi_env env,
    napi_callback_info /*info*/) {
  return nullptr;
}

/// getPromiseHooks() — returns current promise hooks. Returns undefined.
static napi_value getPromiseHooks(
    napi_env env,
    napi_callback_info /*info*/) {
  napi_value undef;
  napi_get_undefined(env, &undef);
  return undef;
}

/// registerDestroyHook(promise, asyncId) — registers a destroy hook for
/// weak reference tracking. No-op.
static napi_value registerDestroyHook(
    napi_env env,
    napi_callback_info /*info*/) {
  return nullptr;
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------

napi_value initAsyncWrapBinding(napi_env env, napi_value exports) {
  // --- Stub functions ---
  napi_property_descriptor props[] = {
      {"setupHooks", nullptr, setupHooks, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"setCallbackTrampoline", nullptr, setCallbackTrampoline, nullptr,
       nullptr, nullptr, napi_default, nullptr},
      {"pushAsyncContext", nullptr, pushAsyncContext, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"popAsyncContext", nullptr, popAsyncContext, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"executionAsyncResource", nullptr, executionAsyncResource, nullptr,
       nullptr, nullptr, napi_default, nullptr},
      {"clearAsyncIdStack", nullptr, clearAsyncIdStack, nullptr, nullptr,
       nullptr, napi_default, nullptr},
      {"queueDestroyAsyncId", nullptr, queueDestroyAsyncId, nullptr, nullptr,
       nullptr, napi_default, nullptr},
      {"setPromiseHooks", nullptr, setPromiseHooks, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"getPromiseHooks", nullptr, getPromiseHooks, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"registerDestroyHook", nullptr, registerDestroyHook, nullptr, nullptr,
       nullptr, napi_default, nullptr},
  };
  NAPI_CALL(napi_define_properties(
      env, exports, sizeof(props) / sizeof(props[0]), props));

  // --- async_hook_fields: Uint32Array(kFieldsCount), all zeros ---
  {
    napi_value ab;
    void *data;
    NAPI_CALL(napi_create_arraybuffer(
        env, kFieldsCount * sizeof(uint32_t), &data, &ab));
    memset(data, 0, kFieldsCount * sizeof(uint32_t));
    napi_value arr;
    NAPI_CALL(
        napi_create_typedarray(env, napi_uint32_array, kFieldsCount, ab, 0, &arr));
    NAPI_CALL(napi_set_named_property(env, exports, "async_hook_fields", arr));
  }

  // --- async_id_fields: Float64Array(kUidFieldsCount), all zeros ---
  {
    napi_value ab;
    void *data;
    NAPI_CALL(napi_create_arraybuffer(
        env, kUidFieldsCount * sizeof(double), &data, &ab));
    memset(data, 0, kUidFieldsCount * sizeof(double));
    napi_value arr;
    NAPI_CALL(napi_create_typedarray(
        env, napi_float64_array, kUidFieldsCount, ab, 0, &arr));
    NAPI_CALL(napi_set_named_property(env, exports, "async_id_fields", arr));
  }

  // --- execution_async_resources: empty JS array ---
  {
    napi_value arr;
    NAPI_CALL(napi_create_array(env, &arr));
    NAPI_CALL(napi_set_named_property(
        env, exports, "execution_async_resources", arr));
  }

  // --- async_ids_stack: Float64Array(0), empty ---
  {
    napi_value ab;
    void *data;
    NAPI_CALL(napi_create_arraybuffer(env, 0, &data, &ab));
    napi_value arr;
    NAPI_CALL(
        napi_create_typedarray(env, napi_float64_array, 0, ab, 0, &arr));
    NAPI_CALL(napi_set_named_property(env, exports, "async_ids_stack", arr));
  }

  // --- constants object ---
  {
    napi_value constants;
    NAPI_CALL(napi_create_object(env, &constants));

#define SET_CONSTANT(name, val)                                     \
  do {                                                              \
    napi_value v;                                                   \
    NAPI_CALL(napi_create_int32(env, static_cast<int32_t>(val), &v)); \
    NAPI_CALL(napi_set_named_property(env, constants, #name, v));   \
  } while (0)

    SET_CONSTANT(kInit, kInit);
    SET_CONSTANT(kBefore, kBefore);
    SET_CONSTANT(kAfter, kAfter);
    SET_CONSTANT(kDestroy, kDestroy);
    SET_CONSTANT(kPromiseResolve, kPromiseResolve);
    SET_CONSTANT(kTotals, kTotals);
    SET_CONSTANT(kCheck, kCheck);
    SET_CONSTANT(kStackLength, kStackLength);
    SET_CONSTANT(kUsesExecutionAsyncResource, kUsesExecutionAsyncResource);
    SET_CONSTANT(kExecutionAsyncId, kExecutionAsyncId);
    SET_CONSTANT(kTriggerAsyncId, kTriggerAsyncId);
    SET_CONSTANT(kAsyncIdCounter, kAsyncIdCounter);
    SET_CONSTANT(kDefaultTriggerAsyncId, kDefaultTriggerAsyncId);

#undef SET_CONSTANT

    NAPI_CALL(napi_set_named_property(env, exports, "constants", constants));
  }

  // --- Providers object ---
  {
    napi_value providers;
    NAPI_CALL(napi_create_object(env, &providers));

#define V(p)                                                          \
  do {                                                                \
    napi_value v;                                                     \
    NAPI_CALL(napi_create_int32(env, static_cast<int32_t>(PROVIDER_##p), &v)); \
    NAPI_CALL(napi_set_named_property(env, providers, #p, v));        \
  } while (0);
    NODE_ASYNC_PROVIDER_TYPES(V)
#undef V

    NAPI_CALL(napi_set_named_property(env, exports, "Providers", providers));
  }

  return exports;
}

} // namespace node_compat
} // namespace hermes
