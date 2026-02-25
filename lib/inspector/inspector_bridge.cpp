/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <hermes/node-compat/inspector/inspector_bridge.h>
#include <hermes/node-compat/runtime/runtime_state.h>

#include <node_api.h>

namespace hermes {
namespace node_compat {

/// Retrieve the InspectorBridgeContext from the current env's RuntimeState.
/// Returns nullptr if not set (normal runtime, not inspector runtime).
static InspectorBridgeContext *getBridgeContext(napi_env env) {
  RuntimeState *state = getRuntimeState(env);
  if (!state)
    return nullptr;
  return static_cast<InspectorBridgeContext *>(state->inspectorBridgeContext);
}

// ---------------------------------------------------------------------------
// sendToMain(jsonString)
// ---------------------------------------------------------------------------

/// Push a CDP command (JSON string) to the main thread's inbound queue and
/// signal the main event loop via uv_async_send.
static napi_value sendToMain(napi_env env, napi_callback_info info) {
  InspectorBridgeContext *ctx = getBridgeContext(env);
  if (!ctx) {
    napi_throw_error(env, nullptr, "inspector_bridge: no bridge context");
    return nullptr;
  }

  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  if (argc < 1) {
    napi_throw_error(
        env, nullptr, "inspector_bridge.sendToMain: expected 1 argument");
    return nullptr;
  }

  // Get the JSON string from the argument.
  size_t len = 0;
  napi_get_value_string_utf8(env, argv[0], nullptr, 0, &len);
  std::string command(len, '\0');
  napi_get_value_string_utf8(env, argv[0], &command[0], len + 1, &len);

  // Push to the main thread's inbound queue.
  if (ctx->inboundMutex && ctx->inboundQueue) {
    {
      std::lock_guard<std::mutex> lock(*ctx->inboundMutex);
      ctx->inboundQueue->push(std::move(command));
    }
    // Signal the main event loop.
    if (ctx->mainAsync && ctx->mainAsyncActive &&
        ctx->mainAsyncActive->load(std::memory_order_acquire)) {
      uv_async_send(ctx->mainAsync);
    }
  }

  napi_value undef;
  napi_get_undefined(env, &undef);
  return undef;
}

// ---------------------------------------------------------------------------
// setMessageCallback(fn)
// ---------------------------------------------------------------------------

/// Register a JS callback function to receive CDP responses/events from the
/// main thread. The callback is stored as a persistent reference and invoked
/// from the inspector's uv_async_t handler when outbound messages arrive.
static napi_value setMessageCallback(napi_env env, napi_callback_info info) {
  InspectorBridgeContext *ctx = getBridgeContext(env);
  if (!ctx) {
    napi_throw_error(env, nullptr, "inspector_bridge: no bridge context");
    return nullptr;
  }

  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  if (argc < 1) {
    napi_throw_error(
        env,
        nullptr,
        "inspector_bridge.setMessageCallback: expected 1 argument");
    return nullptr;
  }

  // Verify the argument is a function.
  napi_valuetype argType;
  napi_typeof(env, argv[0], &argType);
  if (argType != napi_function) {
    napi_throw_type_error(
        env,
        nullptr,
        "inspector_bridge.setMessageCallback: argument must be a function");
    return nullptr;
  }

  // Clean up any existing callback reference.
  if (ctx->messageCallbackRef) {
    napi_delete_reference(env, ctx->messageCallbackRef);
    ctx->messageCallbackRef = nullptr;
  }

  // Create a persistent reference to the callback.
  napi_create_reference(env, argv[0], 1, &ctx->messageCallbackRef);
  ctx->inspectorEnv = env;

  napi_value undef;
  napi_get_undefined(env, &undef);
  return undef;
}

// ---------------------------------------------------------------------------
// getConfig()
// ---------------------------------------------------------------------------

/// Return the inspector configuration as a JS object:
/// { host, port, scriptName, sessionId }
static napi_value getConfig(napi_env env, napi_callback_info info) {
  InspectorBridgeContext *ctx = getBridgeContext(env);
  if (!ctx) {
    napi_throw_error(env, nullptr, "inspector_bridge: no bridge context");
    return nullptr;
  }

  napi_value result;
  napi_create_object(env, &result);

  napi_value hostVal;
  napi_create_string_utf8(env, ctx->host.c_str(), NAPI_AUTO_LENGTH, &hostVal);
  napi_set_named_property(env, result, "host", hostVal);

  napi_value portVal;
  napi_create_int32(env, ctx->port, &portVal);
  napi_set_named_property(env, result, "port", portVal);

  napi_value scriptNameVal;
  napi_create_string_utf8(
      env, ctx->scriptName.c_str(), NAPI_AUTO_LENGTH, &scriptNameVal);
  napi_set_named_property(env, result, "scriptName", scriptNameVal);

  napi_value sessionIdVal;
  napi_create_string_utf8(
      env, ctx->sessionId.c_str(), NAPI_AUTO_LENGTH, &sessionIdVal);
  napi_set_named_property(env, result, "sessionId", sessionIdVal);

  return result;
}

// ---------------------------------------------------------------------------
// notifyReady(actualPort)
// ---------------------------------------------------------------------------

/// Signal the main thread that the inspector server is listening.
/// Takes the actual bound port as an argument (for port-0 auto-assign).
static napi_value notifyReady(napi_env env, napi_callback_info info) {
  InspectorBridgeContext *ctx = getBridgeContext(env);
  if (!ctx) {
    napi_throw_error(env, nullptr, "inspector_bridge: no bridge context");
    return nullptr;
  }

  // Read optional actualPort argument.
  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  int actualPort = ctx->port;
  if (argc >= 1) {
    napi_get_value_int32(env, argv[0], &actualPort);
  }

  {
    std::lock_guard<std::mutex> lock(ctx->readyMutex);
    ctx->actualPort = actualPort;
    ctx->ready = true;
  }
  ctx->readyCv.notify_one();

  napi_value undef;
  napi_get_undefined(env, &undef);
  return undef;
}

// ---------------------------------------------------------------------------
// initInspectorBridgeBinding
// ---------------------------------------------------------------------------

napi_value initInspectorBridgeBinding(napi_env env, napi_value exports) {
  InspectorBridgeContext *ctx = getBridgeContext(env);

  // For normal runtimes (no bridge context), return exports unchanged (empty).
  if (!ctx) {
    return exports;
  }

  napi_value sendToMainFn;
  napi_create_function(
      env, "sendToMain", NAPI_AUTO_LENGTH, sendToMain, nullptr, &sendToMainFn);
  napi_set_named_property(env, exports, "sendToMain", sendToMainFn);

  napi_value setMessageCallbackFn;
  napi_create_function(
      env,
      "setMessageCallback",
      NAPI_AUTO_LENGTH,
      setMessageCallback,
      nullptr,
      &setMessageCallbackFn);
  napi_set_named_property(
      env, exports, "setMessageCallback", setMessageCallbackFn);

  napi_value getConfigFn;
  napi_create_function(
      env, "getConfig", NAPI_AUTO_LENGTH, getConfig, nullptr, &getConfigFn);
  napi_set_named_property(env, exports, "getConfig", getConfigFn);

  napi_value notifyReadyFn;
  napi_create_function(
      env,
      "notifyReady",
      NAPI_AUTO_LENGTH,
      notifyReady,
      nullptr,
      &notifyReadyFn);
  napi_set_named_property(env, exports, "notifyReady", notifyReadyFn);

  return exports;
}

} // namespace node_compat
} // namespace hermes
