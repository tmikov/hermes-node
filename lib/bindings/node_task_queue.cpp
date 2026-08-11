/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <hermes/node-compat/bindings/node_task_queue.h>
#include <hermes/node-compat/runtime/runtime_state.h>

#include <node_api.h>

#include <cstdio>
#include <cstring>

namespace hermes {
namespace node_compat {

// Promise rejection events, matching V8's PromiseRejectEvent. Node's
// internal/process/promises.js switches on these.
static constexpr int32_t kPromiseRejectWithNoHandler = 0;
static constexpr int32_t kPromiseHandlerAddedAfterReject = 1;
static constexpr int32_t kPromiseResolveAfterResolved = 2;
static constexpr int32_t kPromiseRejectAfterResolved = 3;

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
// Promise rejection tracking
//
// V8 reports rejections to the host directly, which is what Node's
// internal/process/promises.js is written against. Hermes instead exposes
// HermesInternal.enablePromiseRejectionTracker(opts), calling
// opts.onUnhandled(id, error, promise) once a rejection has gone unhandled
// and opts.onHandled(id, error, promise) if a handler shows up later. Those
// two map onto the only V8 events Node needs here, so we arm the tracker and
// forward.
//
// One behavioral difference is worth knowing: Hermes decides a rejection is
// unhandled on a timer (100ms for the error types it considers likely
// programmer mistakes, 2s otherwise) rather than at the end of the microtask
// checkpoint, so reports arrive later than they would in Node. Detection is
// strictly more forgiving, never less, so nothing Node would accept is
// reported here.
// ---------------------------------------------------------------------------

/// Call the registered JS rejection callback, if any, with the V8-shaped
/// (type, promise, reason) arguments Node expects.
static void invokeRejectCallback(
    napi_env env,
    TaskQueueState *state,
    int32_t type,
    napi_value promise,
    napi_value reason) {
  if (!state->promiseRejectCallbackRef)
    return;

  napi_value callback;
  if (napi_get_reference_value(
          env, state->promiseRejectCallbackRef, &callback) != napi_ok)
    return;

  napi_valuetype cbType;
  if (napi_typeof(env, callback, &cbType) != napi_ok || cbType != napi_function)
    return;

  napi_value typeVal;
  if (napi_create_int32(env, type, &typeVal) != napi_ok)
    return;

  napi_value undef;
  napi_get_undefined(env, &undef);

  napi_value args[3] = {
      typeVal, promise ? promise : undef, reason ? reason : undef};
  napi_value result;
  napi_call_function(env, undef, callback, 3, args, &result);
}

/// Hermes calls this with (id, error, promise) once a rejection is deemed
/// unhandled.
static napi_value onUnhandledRejection(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3] = {nullptr, nullptr, nullptr};
  void *data = nullptr;
  napi_get_cb_info(env, info, &argc, argv, nullptr, &data);

  invokeRejectCallback(
      env,
      static_cast<TaskQueueState *>(data),
      kPromiseRejectWithNoHandler,
      argc > 2 ? argv[2] : nullptr,
      argc > 1 ? argv[1] : nullptr);

  napi_value undef;
  napi_get_undefined(env, &undef);
  return undef;
}

/// Hermes calls this with (id, error, promise) when a handler is attached to
/// a promise already reported as unhandled.
static napi_value onRejectionHandled(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3] = {nullptr, nullptr, nullptr};
  void *data = nullptr;
  napi_get_cb_info(env, info, &argc, argv, nullptr, &data);

  invokeRejectCallback(
      env,
      static_cast<TaskQueueState *>(data),
      kPromiseHandlerAddedAfterReject,
      argc > 2 ? argv[2] : nullptr,
      nullptr);

  napi_value undef;
  napi_get_undefined(env, &undef);
  return undef;
}

/// Point Hermes's rejection tracker at the callbacks above. Quietly does
/// nothing if the runtime does not expose the tracker; a runtime that has it
/// but refuses to arm it is reported, since the alternative is losing every
/// unhandled rejection without a word.
static void armRejectionTracker(napi_env env, TaskQueueState *state) {
  napi_value global;
  if (napi_get_global(env, &global) != napi_ok)
    return;

  napi_value hermesInternal;
  napi_valuetype type;
  if (napi_get_named_property(env, global, "HermesInternal", &hermesInternal) !=
          napi_ok ||
      napi_typeof(env, hermesInternal, &type) != napi_ok || type != napi_object)
    return;

  napi_value enableFn;
  if (napi_get_named_property(
          env, hermesInternal, "enablePromiseRejectionTracker", &enableFn) !=
          napi_ok ||
      napi_typeof(env, enableFn, &type) != napi_ok || type != napi_function)
    return;

  napi_value opts;
  if (napi_create_object(env, &opts) != napi_ok)
    return;

  napi_value trueVal;
  napi_get_boolean(env, true, &trueVal);
  napi_set_named_property(env, opts, "allRejections", trueVal);

  napi_value fn;
  if (napi_create_function(
          env,
          "onUnhandled",
          NAPI_AUTO_LENGTH,
          onUnhandledRejection,
          state,
          &fn) != napi_ok)
    return;
  napi_set_named_property(env, opts, "onUnhandled", fn);

  if (napi_create_function(
          env, "onHandled", NAPI_AUTO_LENGTH, onRejectionHandled, state, &fn) !=
      napi_ok)
    return;
  napi_set_named_property(env, opts, "onHandled", fn);

  napi_value result;
  if (napi_call_function(env, hermesInternal, enableFn, 1, &opts, &result) !=
      napi_ok) {
    bool pending = false;
    napi_is_exception_pending(env, &pending);
    if (pending) {
      napi_value ignored;
      napi_get_and_clear_last_exception(env, &ignored);
    }
    std::fprintf(
        stderr,
        "Warning: could not enable promise rejection tracking; "
        "unhandled rejections will go unreported\n");
  }
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

  // Nothing reports rejections until the tracker is armed, and this is the
  // point at which there is somewhere to report them to.
  armRejectionTracker(env, state);

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

    setConst("kPromiseRejectWithNoHandler", kPromiseRejectWithNoHandler);
    setConst(
        "kPromiseHandlerAddedAfterReject", kPromiseHandlerAddedAfterReject);
    setConst("kPromiseResolveAfterResolved", kPromiseResolveAfterResolved);
    setConst("kPromiseRejectAfterResolved", kPromiseRejectAfterResolved);

    napi_set_named_property(env, exports, "promiseRejectEvents", events);
  }

  return exports;
}

} // namespace node_compat
} // namespace hermes
