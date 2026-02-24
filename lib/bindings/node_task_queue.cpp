/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <hermes/node-compat/bindings/node_task_queue.h>
#include <hermes/node-compat/runtime/runtime_state.h>

#include <node_api.h>

#include <cstring>

namespace hermes {
namespace node_compat {

// ---------------------------------------------------------------------------
// Per-binding state stored as callback data via napi_ref
// ---------------------------------------------------------------------------

struct TaskQueueState {
  napi_ref tickCallbackRef = nullptr;
  napi_ref promiseRejectCallbackRef = nullptr;
};

// ---------------------------------------------------------------------------
// Helper to get TaskQueueState from callback data
// ---------------------------------------------------------------------------

static TaskQueueState *getState(napi_env env, napi_callback_info info) {
  void *data = nullptr;
  napi_get_cb_info(env, info, nullptr, nullptr, nullptr, &data);
  return static_cast<TaskQueueState *>(data);
}

// ---------------------------------------------------------------------------
// runMicrotasks — drain the microtask queue via the host-provided callback
// ---------------------------------------------------------------------------

static napi_value runMicrotasks(napi_env env, napi_callback_info /*info*/) {
  auto *rtState = getRuntimeState(env);
  if (rtState && rtState->drainMicrotasksFn)
    rtState->drainMicrotasksFn(rtState->drainMicrotasksData);

  napi_value undef;
  napi_get_undefined(env, &undef);
  return undef;
}

// ---------------------------------------------------------------------------
// enqueueMicrotask(fn) — enqueue a function as a microtask
// ---------------------------------------------------------------------------

static napi_value enqueueMicrotask(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  // Validate: must be a function
  napi_valuetype type;
  napi_typeof(env, argv[0], &type);
  if (type != napi_function) {
    napi_throw_type_error(env, nullptr, "Microtask must be a function");
    return nullptr;
  }

  // Use Promise.resolve().then(fn) to enqueue fn as a microtask.
  // This is the portable way to enqueue microtasks via NAPI.
  napi_value global;
  napi_get_global(env, &global);

  napi_value promiseCtor;
  napi_get_named_property(env, global, "Promise", &promiseCtor);

  napi_value resolveFn;
  napi_get_named_property(env, promiseCtor, "resolve", &resolveFn);

  napi_value undef;
  napi_get_undefined(env, &undef);

  napi_value resolved;
  napi_call_function(env, promiseCtor, resolveFn, 1, &undef, &resolved);

  napi_value thenFn;
  napi_get_named_property(env, resolved, "then", &thenFn);

  napi_value result;
  napi_call_function(env, resolved, thenFn, 1, argv, &result);

  return undef;
}

// ---------------------------------------------------------------------------
// setTickCallback(fn) — register the JS tick drain function
// ---------------------------------------------------------------------------

static napi_value setTickCallback(napi_env env, napi_callback_info info) {
  auto *state = getState(env, info);

  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  if (state->tickCallbackRef) {
    napi_delete_reference(env, state->tickCallbackRef);
    state->tickCallbackRef = nullptr;
  }

  napi_create_reference(env, argv[0], 1, &state->tickCallbackRef);

  napi_value undef;
  napi_get_undefined(env, &undef);
  return undef;
}

// ---------------------------------------------------------------------------
// setPromiseRejectCallback(fn) — register the promise rejection handler
// ---------------------------------------------------------------------------

static napi_value setPromiseRejectCallback(
    napi_env env,
    napi_callback_info info) {
  auto *state = getState(env, info);

  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  if (state->promiseRejectCallbackRef) {
    napi_delete_reference(env, state->promiseRejectCallbackRef);
    state->promiseRejectCallbackRef = nullptr;
  }

  napi_create_reference(env, argv[0], 1, &state->promiseRejectCallbackRef);

  napi_value undef;
  napi_get_undefined(env, &undef);
  return undef;
}

// ---------------------------------------------------------------------------
// Cleanup
// ---------------------------------------------------------------------------

static void cleanupState(napi_env env, void *data, void * /*hint*/) {
  auto *state = static_cast<TaskQueueState *>(data);
  if (state->tickCallbackRef)
    napi_delete_reference(env, state->tickCallbackRef);
  if (state->promiseRejectCallbackRef)
    napi_delete_reference(env, state->promiseRejectCallbackRef);
  delete state;
}

// ---------------------------------------------------------------------------
// Binding init
// ---------------------------------------------------------------------------

napi_value initTaskQueueBinding(napi_env env, napi_value exports) {
  auto *state = new TaskQueueState();

  napi_add_finalizer(env, exports, state, cleanupState, nullptr, nullptr);

  // tickInfo: Uint32Array(2)
  // [0] = kHasTickScheduled, [1] = kHasRejectionToWarn
  {
    napi_value ab;
    void *data;
    napi_create_arraybuffer(env, 2 * sizeof(uint32_t), &data, &ab);
    std::memset(data, 0, 2 * sizeof(uint32_t));

    napi_value tickInfo;
    napi_create_typedarray(env, napi_uint32_array, 2, ab, 0, &tickInfo);
    napi_set_named_property(env, exports, "tickInfo", tickInfo);
  }

  // runMicrotasks
  {
    napi_value fn;
    napi_create_function(
        env, "runMicrotasks", NAPI_AUTO_LENGTH, runMicrotasks, nullptr, &fn);
    napi_set_named_property(env, exports, "runMicrotasks", fn);
  }

  // setTickCallback
  {
    napi_value fn;
    napi_create_function(
        env, "setTickCallback", NAPI_AUTO_LENGTH, setTickCallback, state, &fn);
    napi_set_named_property(env, exports, "setTickCallback", fn);
  }

  // enqueueMicrotask
  {
    napi_value fn;
    napi_create_function(
        env,
        "enqueueMicrotask",
        NAPI_AUTO_LENGTH,
        enqueueMicrotask,
        nullptr,
        &fn);
    napi_set_named_property(env, exports, "enqueueMicrotask", fn);
  }

  // setPromiseRejectCallback
  {
    napi_value fn;
    napi_create_function(
        env,
        "setPromiseRejectCallback",
        NAPI_AUTO_LENGTH,
        setPromiseRejectCallback,
        state,
        &fn);
    napi_set_named_property(env, exports, "setPromiseRejectCallback", fn);
  }

  // promiseRejectEvents — constants matching V8's PromiseRejectEvent
  {
    napi_value events;
    napi_create_object(env, &events);

    auto setConst = [&](const char *name, int32_t val) {
      napi_value v;
      napi_create_int32(env, val, &v);
      napi_set_named_property(env, events, name, v);
    };

    setConst("kPromiseRejectWithNoHandler", 0);
    setConst("kPromiseHandlerAddedAfterReject", 1);
    setConst("kPromiseResolveAfterResolved", 2);
    setConst("kPromiseRejectAfterResolved", 3);

    napi_set_named_property(env, exports, "promiseRejectEvents", events);
  }

  return exports;
}

} // namespace node_compat
} // namespace hermes
