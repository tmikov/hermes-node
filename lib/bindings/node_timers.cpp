/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <hermes/node-compat/bindings/node_timers.h>
#include <node_api.h>
#include <uv.h>

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace hermes {
namespace node_compat {

// ---------------------------------------------------------------------------
// Module-level state set by the host before binding init.
// ---------------------------------------------------------------------------

static uv_loop_t *s_loop = nullptr;

void setTimersEventLoop(uv_loop_t *loop) {
  s_loop = loop;
}

// ---------------------------------------------------------------------------
// Per-binding state attached to the exports object.
// ---------------------------------------------------------------------------

struct TimersState {
  napi_env env = nullptr;

  // libuv handles.
  uv_timer_t timerHandle{};
  uv_check_t immediateCheckHandle{};
  uv_idle_t immediateIdleHandle{};

  // Timer base: value of uv_now() when the binding was initialized.
  uint64_t timerBase = 0;

  // JS callbacks set by setupTimers().
  napi_ref processImmediateRef = nullptr;
  napi_ref processTimersRef = nullptr;

  // Shared typed arrays exposed to JS.
  // immediateInfo: Uint32Array(3) — [kCount, kRefCount, kHasOutstanding]
  uint32_t *immediateInfoData = nullptr;
  // timeoutInfo: Int32Array(1) — [refed timeout count]
  int32_t *timeoutInfoData = nullptr;

  bool timerHandleInited = false;
  bool checkHandleInited = false;
  bool idleHandleInited = false;
};

static TimersState *s_timersState = nullptr;

// ---------------------------------------------------------------------------
// Helper: get TimersState from napi_callback_info data.
// ---------------------------------------------------------------------------

static TimersState *getState(napi_env env, napi_callback_info info) {
  void *data = nullptr;
  napi_get_cb_info(env, info, nullptr, nullptr, nullptr, &data);
  return static_cast<TimersState *>(data);
}

// ---------------------------------------------------------------------------
// uv_timer callback: called when the timer fires.
// ---------------------------------------------------------------------------

static void onTimerFired(uv_timer_t *handle) {
  auto *state = static_cast<TimersState *>(handle->data);
  if (!state->processTimersRef)
    return;

  napi_handle_scope scope;
  napi_open_handle_scope(state->env, &scope);

  napi_value processTimersFn;
  napi_get_reference_value(
      state->env, state->processTimersRef, &processTimersFn);

  napi_valuetype fnType;
  napi_typeof(state->env, processTimersFn, &fnType);
  if (fnType != napi_function) {
    napi_close_handle_scope(state->env, scope);
    return;
  }

  // Pass current libuv time (relative to timer base) as argument.
  uv_update_time(s_loop);
  uint64_t now = uv_now(s_loop) - state->timerBase;

  napi_value nowArg;
  if (now <= 0xFFFFFFFF) {
    napi_create_uint32(state->env, static_cast<uint32_t>(now), &nowArg);
  } else {
    napi_create_double(state->env, static_cast<double>(now), &nowArg);
  }

  napi_value global;
  napi_get_global(state->env, &global);

  napi_value processObj;
  napi_get_named_property(state->env, global, "process", &processObj);

  napi_value result;
  napi_status st = napi_call_function(
      state->env, processObj, processTimersFn, 1, &nowArg, &result);

  if (st != napi_ok) {
    // Print and clear any exception.
    bool pending = false;
    napi_is_exception_pending(state->env, &pending);
    if (pending) {
      napi_value exc;
      napi_get_and_clear_last_exception(state->env, &exc);
      napi_value stack;
      napi_status st2 =
          napi_get_named_property(state->env, exc, "stack", &stack);
      napi_valuetype stackType = napi_undefined;
      if (st2 == napi_ok)
        napi_typeof(state->env, stack, &stackType);
      napi_value msg;
      if (stackType == napi_string)
        msg = stack;
      else
        napi_coerce_to_string(state->env, exc, &msg);
      char buf[4096];
      size_t len = 0;
      napi_get_value_string_utf8(state->env, msg, buf, sizeof(buf), &len);
      std::fprintf(stderr, "%.*s\n", static_cast<int>(len), buf);
    }
    napi_close_handle_scope(state->env, scope);
    return;
  }

  // Process the return value from processTimers():
  // 0 = no more timers, unref handle
  // >0 = next expiry (absolute from timer_base), refed timers exist
  // <0 = next expiry (negated), no refed timers
  double expiryMs = 0;
  napi_get_value_double(state->env, result, &expiryMs);

  auto *h = reinterpret_cast<uv_handle_t *>(&state->timerHandle);

  if (expiryMs != 0) {
    uv_update_time(s_loop);
    int64_t duration =
        static_cast<int64_t>((expiryMs < 0 ? -expiryMs : expiryMs)) -
        static_cast<int64_t>(uv_now(s_loop) - state->timerBase);

    uv_timer_start(
        &state->timerHandle,
        onTimerFired,
        static_cast<uint64_t>(duration > 0 ? duration : 1),
        0);

    if (expiryMs > 0)
      uv_ref(h);
    else
      uv_unref(h);
  } else {
    uv_unref(h);
  }

  napi_close_handle_scope(state->env, scope);
}

// ---------------------------------------------------------------------------
// uv_check callback: called after I/O polling to drain immediates.
// ---------------------------------------------------------------------------

static void onCheckImmediate(uv_check_t *handle) {
  auto *state = static_cast<TimersState *>(handle->data);
  if (!state->processImmediateRef)
    return;

  // Check if there are any immediates to process.
  if (state->immediateInfoData[0] == 0) // kCount
    return;

  napi_handle_scope scope;
  napi_open_handle_scope(state->env, &scope);

  napi_value processImmediateFn;
  napi_get_reference_value(
      state->env, state->processImmediateRef, &processImmediateFn);

  napi_valuetype fnType;
  napi_typeof(state->env, processImmediateFn, &fnType);
  if (fnType != napi_function) {
    napi_close_handle_scope(state->env, scope);
    return;
  }

  // Loop while there are outstanding immediates (exception recovery).
  do {
    napi_value global;
    napi_get_global(state->env, &global);
    napi_value processObj;
    napi_get_named_property(state->env, global, "process", &processObj);

    napi_value result;
    napi_status st = napi_call_function(
        state->env, processObj, processImmediateFn, 0, nullptr, &result);
    if (st != napi_ok) {
      bool pending = false;
      napi_is_exception_pending(state->env, &pending);
      if (pending) {
        napi_value exc;
        napi_get_and_clear_last_exception(state->env, &exc);
        napi_value stack;
        napi_status st2 =
            napi_get_named_property(state->env, exc, "stack", &stack);
        napi_valuetype stackType = napi_undefined;
        if (st2 == napi_ok)
          napi_typeof(state->env, stack, &stackType);
        napi_value msg;
        if (stackType == napi_string)
          msg = stack;
        else
          napi_coerce_to_string(state->env, exc, &msg);
        char buf[4096];
        size_t len = 0;
        napi_get_value_string_utf8(state->env, msg, buf, sizeof(buf), &len);
        std::fprintf(stderr, "%.*s\n", static_cast<int>(len), buf);
      }
      break;
    }
  } while (state->immediateInfoData[2] != 0); // kHasOutstanding

  // If no more refed immediates, unref the idle handle to let the loop exit.
  if (state->immediateInfoData[1] == 0) { // kRefCount
    uv_idle_stop(&state->immediateIdleHandle);
  }

  napi_close_handle_scope(state->env, scope);
}

// ---------------------------------------------------------------------------
// Binding functions exposed to JS.
// ---------------------------------------------------------------------------

/// setupTimers(processImmediate, processTimers)
static napi_value setupTimers(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  void *data = nullptr;
  napi_get_cb_info(env, info, &argc, argv, nullptr, &data);
  auto *state = static_cast<TimersState *>(data);

  // Store the two JS callback functions.
  if (state->processImmediateRef) {
    napi_delete_reference(env, state->processImmediateRef);
    state->processImmediateRef = nullptr;
  }
  if (state->processTimersRef) {
    napi_delete_reference(env, state->processTimersRef);
    state->processTimersRef = nullptr;
  }

  napi_create_reference(env, argv[0], 1, &state->processImmediateRef);
  napi_create_reference(env, argv[1], 1, &state->processTimersRef);

  napi_value undefined;
  napi_get_undefined(env, &undefined);
  return undefined;
}

/// getLibuvNow() — returns current libuv time relative to timer base.
static napi_value getLibuvNow(napi_env env, napi_callback_info info) {
  auto *state = getState(env, info);

  uv_update_time(s_loop);
  uint64_t now = uv_now(s_loop) - state->timerBase;

  napi_value result;
  napi_create_double(env, static_cast<double>(now), &result);
  return result;
}

/// scheduleTimer(duration) — start or restart the timer with given ms delay.
static napi_value scheduleTimer(napi_env env, napi_callback_info info) {
  auto *state = getState(env, info);

  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  int64_t duration = 0;
  napi_get_value_int64(env, argv[0], &duration);

  uv_timer_start(
      &state->timerHandle,
      onTimerFired,
      static_cast<uint64_t>(duration > 0 ? duration : 0),
      0);

  napi_value undefined;
  napi_get_undefined(env, &undefined);
  return undefined;
}

/// toggleTimerRef(ref) — ref or unref the timer handle.
static napi_value toggleTimerRef(napi_env env, napi_callback_info info) {
  auto *state = getState(env, info);

  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  bool ref = false;
  napi_get_value_bool(env, argv[0], &ref);

  auto *h = reinterpret_cast<uv_handle_t *>(&state->timerHandle);
  if (ref)
    uv_ref(h);
  else
    uv_unref(h);

  napi_value undefined;
  napi_get_undefined(env, &undefined);
  return undefined;
}

/// toggleImmediateRef(ref) — start/stop the idle handle to prevent/allow
/// the event loop from blocking in poll.
static napi_value toggleImmediateRef(napi_env env, napi_callback_info info) {
  auto *state = getState(env, info);

  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  bool ref = false;
  napi_get_value_bool(env, argv[0], &ref);

  if (ref) {
    // Start the idle handle so the event loop doesn't block in poll.
    uv_idle_start(&state->immediateIdleHandle, [](uv_idle_t *) {});
  } else {
    uv_idle_stop(&state->immediateIdleHandle);
  }

  napi_value undefined;
  napi_get_undefined(env, &undefined);
  return undefined;
}

// ---------------------------------------------------------------------------
// Close handles.
// ---------------------------------------------------------------------------

void closeTimersHandles() {
  if (!s_timersState)
    return;
  auto *state = s_timersState;

  if (state->timerHandleInited) {
    uv_timer_stop(&state->timerHandle);
    uv_close(reinterpret_cast<uv_handle_t *>(&state->timerHandle), nullptr);
  }
  if (state->checkHandleInited) {
    uv_check_stop(&state->immediateCheckHandle);
    uv_close(
        reinterpret_cast<uv_handle_t *>(&state->immediateCheckHandle), nullptr);
  }
  if (state->idleHandleInited) {
    uv_idle_stop(&state->immediateIdleHandle);
    uv_close(
        reinterpret_cast<uv_handle_t *>(&state->immediateIdleHandle), nullptr);
  }
}

// ---------------------------------------------------------------------------
// Cleanup (NAPI finalizer).
// ---------------------------------------------------------------------------

static void cleanupState(napi_env env, void *data, void * /*hint*/) {
  auto *state = static_cast<TimersState *>(data);

  if (state->processImmediateRef)
    napi_delete_reference(env, state->processImmediateRef);
  if (state->processTimersRef)
    napi_delete_reference(env, state->processTimersRef);

  // Note: libuv handles are stack-like in hermes-node.cpp and closed there.
  // We don't close them here because the binding may be finalized after
  // the event loop is already closed.

  if (s_timersState == state)
    s_timersState = nullptr;

  delete state;
}

// ---------------------------------------------------------------------------
// Binding init.
// ---------------------------------------------------------------------------

napi_value initTimersBinding(napi_env env, napi_value exports) {
  assert(s_loop && "setTimersEventLoop() must be called before init");

  auto *state = new TimersState();
  state->env = env;
  s_timersState = state;

  // Record the timer base (current libuv time).
  uv_update_time(s_loop);
  state->timerBase = uv_now(s_loop);

  // Initialize the timer handle (for setTimeout/setInterval scheduling).
  uv_timer_init(s_loop, &state->timerHandle);
  state->timerHandle.data = state;
  uv_unref(reinterpret_cast<uv_handle_t *>(&state->timerHandle));
  state->timerHandleInited = true;

  // Initialize the check handle (for draining immediates after I/O).
  uv_check_init(s_loop, &state->immediateCheckHandle);
  state->immediateCheckHandle.data = state;
  uv_check_start(&state->immediateCheckHandle, onCheckImmediate);
  uv_unref(reinterpret_cast<uv_handle_t *>(&state->immediateCheckHandle));
  state->checkHandleInited = true;

  // Initialize the idle handle (prevents event loop from blocking in poll
  // when there are refed immediates).
  uv_idle_init(s_loop, &state->immediateIdleHandle);
  state->immediateIdleHandle.data = state;
  // Starts stopped — toggleImmediateRef(true) starts it.
  state->idleHandleInited = true;

  // Attach cleanup finalizer.
  napi_add_finalizer(env, exports, state, cleanupState, nullptr, nullptr);

  // Create immediateInfo — Uint32Array(3).
  {
    napi_value ab;
    void *abData = nullptr;
    napi_create_arraybuffer(env, 3 * sizeof(uint32_t), &abData, &ab);
    state->immediateInfoData = static_cast<uint32_t *>(abData);
    std::memset(abData, 0, 3 * sizeof(uint32_t));

    napi_value typedArray;
    napi_create_typedarray(env, napi_uint32_array, 3, ab, 0, &typedArray);
    napi_set_named_property(env, exports, "immediateInfo", typedArray);
  }

  // Create timeoutInfo — Int32Array(1).
  {
    napi_value ab;
    void *abData = nullptr;
    napi_create_arraybuffer(env, 1 * sizeof(int32_t), &abData, &ab);
    state->timeoutInfoData = static_cast<int32_t *>(abData);
    std::memset(abData, 0, 1 * sizeof(int32_t));

    napi_value typedArray;
    napi_create_typedarray(env, napi_int32_array, 1, ab, 0, &typedArray);
    napi_set_named_property(env, exports, "timeoutInfo", typedArray);
  }

  // Create functions.
  auto setFn = [&](const char *name, napi_callback cb) {
    napi_value fn;
    napi_create_function(env, name, NAPI_AUTO_LENGTH, cb, state, &fn);
    napi_set_named_property(env, exports, name, fn);
  };

  setFn("setupTimers", setupTimers);
  setFn("getLibuvNow", getLibuvNow);
  setFn("scheduleTimer", scheduleTimer);
  setFn("toggleTimerRef", toggleTimerRef);
  setFn("toggleImmediateRef", toggleImmediateRef);

  return exports;
}

} // namespace node_compat
} // namespace hermes
