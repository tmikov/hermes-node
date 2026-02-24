/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <hermes/node-compat/bindings/node_fs_event_wrap.h>
#include <hermes/node-compat/runtime/runtime_state.h>
#include <node_api.h>
#include <uv.h>

#include <cstring>
#include <string>

namespace hermes {
namespace node_compat {

// ---------------------------------------------------------------------------
// FSEventWrap — native data attached to FSEvent JS objects
// ---------------------------------------------------------------------------

struct FSEventWrap {
  uv_fs_event_t handle;
  napi_env env;
  napi_ref selfRef; // prevent GC while handle is active
  bool initialized; // whether start() was called successfully
  bool closing; // whether close() was called

  FSEventWrap()
      : env(nullptr), selfRef(nullptr), initialized(false), closing(false) {
    memset(&handle, 0, sizeof(handle));
  }
};

// Forward declaration.
static void
onFsEvent(uv_fs_event_t *handle, const char *filename, int events, int status);

// ---------------------------------------------------------------------------
// FSEvent constructor: new FSEvent()
// ---------------------------------------------------------------------------

static napi_value fsEventNew(napi_env env, napi_callback_info info) {
  napi_value thisObj;
  napi_get_cb_info(env, info, nullptr, nullptr, &thisObj, nullptr);

  auto *wrap = new FSEventWrap();
  wrap->env = env;

  napi_wrap(
      env,
      thisObj,
      wrap,
      [](napi_env, void *data, void *) {
        auto *w = static_cast<FSEventWrap *>(data);
        if (w->initialized) {
          // uv_close is async — the close callback will delete.
          uv_fs_event_stop(&w->handle);
          uv_close(
              reinterpret_cast<uv_handle_t *>(&w->handle), [](uv_handle_t *h) {
                delete static_cast<FSEventWrap *>(h->data);
              });
          return;
        }
        delete w;
      },
      nullptr,
      nullptr);

  return thisObj;
}

// ---------------------------------------------------------------------------
// FSEvent.prototype.start(path, persistent, recursive, encoding)
// Returns 0 on success, or a negative UV error code.
// ---------------------------------------------------------------------------

static napi_value fsEventStart(napi_env env, napi_callback_info info) {
  size_t argc = 4;
  napi_value argv[4], thisObj;
  napi_get_cb_info(env, info, &argc, argv, &thisObj, nullptr);

  FSEventWrap *wrap = nullptr;
  napi_unwrap(env, thisObj, reinterpret_cast<void **>(&wrap));
  if (!wrap) {
    napi_value zero;
    napi_create_int32(env, UV_EINVAL, &zero);
    return zero;
  }

  // Already started.
  if (wrap->initialized) {
    napi_value zero;
    napi_create_int32(env, 0, &zero);
    return zero;
  }

  // Get path string.
  size_t pathLen = 0;
  napi_get_value_string_utf8(env, argv[0], nullptr, 0, &pathLen);
  std::string path(pathLen, '\0');
  napi_get_value_string_utf8(env, argv[0], &path[0], pathLen + 1, &pathLen);

  // Get persistent flag.
  bool persistent = true;
  napi_get_value_bool(env, argv[1], &persistent);

  // Get recursive flag.
  bool recursive = false;
  napi_get_value_bool(env, argv[2], &recursive);

  // encoding arg[3] is accepted but we always use UTF-8.

  unsigned int flags = 0;
  if (recursive)
    flags |= UV_FS_EVENT_RECURSIVE;

  int err = uv_fs_event_init(getRuntimeState(env)->loop, &wrap->handle);
  if (err != 0) {
    napi_value result;
    napi_create_int32(env, err, &result);
    return result;
  }

  wrap->handle.data = wrap;

  err = uv_fs_event_start(&wrap->handle, onFsEvent, path.c_str(), flags);
  if (err != 0) {
    // Close the handle since init succeeded but start failed.
    uv_close(reinterpret_cast<uv_handle_t *>(&wrap->handle), nullptr);
    napi_value result;
    napi_create_int32(env, err, &result);
    return result;
  }

  wrap->initialized = true;

  // If not persistent, unref the handle so it doesn't keep the loop alive.
  if (!persistent) {
    uv_unref(reinterpret_cast<uv_handle_t *>(&wrap->handle));
  }

  // Create a ref to prevent GC while the handle is active.
  napi_create_reference(env, thisObj, 1, &wrap->selfRef);

  napi_value result;
  napi_create_int32(env, 0, &result);
  return result;
}

// ---------------------------------------------------------------------------
// FSEvent.prototype.close()
// ---------------------------------------------------------------------------

static void onFsEventClose(uv_handle_t *handle) {
  auto *wrap = static_cast<FSEventWrap *>(handle->data);
  if (!wrap)
    return;

  // Release the prevent-GC reference.
  if (wrap->selfRef) {
    napi_delete_reference(wrap->env, wrap->selfRef);
    wrap->selfRef = nullptr;
  }
  delete wrap;
}

static napi_value fsEventClose(napi_env env, napi_callback_info info) {
  napi_value thisObj;
  napi_get_cb_info(env, info, nullptr, nullptr, &thisObj, nullptr);

  FSEventWrap *wrap = nullptr;
  napi_unwrap(env, thisObj, reinterpret_cast<void **>(&wrap));
  if (!wrap || wrap->closing)
    return nullptr;

  wrap->closing = true;

  if (wrap->initialized &&
      !uv_is_closing(reinterpret_cast<uv_handle_t *>(&wrap->handle))) {
    uv_close(reinterpret_cast<uv_handle_t *>(&wrap->handle), onFsEventClose);
    // The close callback now owns 'wrap'. Remove the GC wrap so the
    // finalizer won't run and try to double-delete.
    napi_remove_wrap(env, thisObj, nullptr);
  } else {
    // Not initialized or already closing, just release ref.
    if (wrap->selfRef) {
      napi_delete_reference(env, wrap->selfRef);
      wrap->selfRef = nullptr;
    }
  }

  return nullptr;
}

// ---------------------------------------------------------------------------
// FSEvent.prototype.ref() / unref() / hasRef()
// ---------------------------------------------------------------------------

static napi_value fsEventRef(napi_env env, napi_callback_info info) {
  napi_value thisObj;
  napi_get_cb_info(env, info, nullptr, nullptr, &thisObj, nullptr);

  FSEventWrap *wrap = nullptr;
  napi_unwrap(env, thisObj, reinterpret_cast<void **>(&wrap));
  if (wrap && wrap->initialized && !wrap->closing) {
    uv_ref(reinterpret_cast<uv_handle_t *>(&wrap->handle));
  }
  return thisObj;
}

static napi_value fsEventUnref(napi_env env, napi_callback_info info) {
  napi_value thisObj;
  napi_get_cb_info(env, info, nullptr, nullptr, &thisObj, nullptr);

  FSEventWrap *wrap = nullptr;
  napi_unwrap(env, thisObj, reinterpret_cast<void **>(&wrap));
  if (wrap && wrap->initialized && !wrap->closing) {
    uv_unref(reinterpret_cast<uv_handle_t *>(&wrap->handle));
  }
  return thisObj;
}

static napi_value fsEventHasRef(napi_env env, napi_callback_info info) {
  napi_value thisObj;
  napi_get_cb_info(env, info, nullptr, nullptr, &thisObj, nullptr);

  FSEventWrap *wrap = nullptr;
  napi_unwrap(env, thisObj, reinterpret_cast<void **>(&wrap));

  bool has = false;
  if (wrap && wrap->initialized && !wrap->closing) {
    has = uv_has_ref(reinterpret_cast<uv_handle_t *>(&wrap->handle)) != 0;
  }

  napi_value result;
  napi_get_boolean(env, has, &result);
  return result;
}

// ---------------------------------------------------------------------------
// FSEvent.prototype.initialized (getter)
// ---------------------------------------------------------------------------

static napi_value fsEventGetInitialized(napi_env env, napi_callback_info info) {
  napi_value thisObj;
  napi_get_cb_info(env, info, nullptr, nullptr, &thisObj, nullptr);

  FSEventWrap *wrap = nullptr;
  napi_unwrap(env, thisObj, reinterpret_cast<void **>(&wrap));

  bool val = wrap && wrap->initialized && !wrap->closing;

  napi_value result;
  napi_get_boolean(env, val, &result);
  return result;
}

// ---------------------------------------------------------------------------
// FSEvent.prototype.getAsyncId() — stub returning 0
// ---------------------------------------------------------------------------

static napi_value fsEventGetAsyncId(napi_env env, napi_callback_info) {
  napi_value result;
  napi_create_double(env, 0.0, &result);
  return result;
}

// ---------------------------------------------------------------------------
// libuv callback: fires FSEvent.onchange(status, eventType, filename)
// ---------------------------------------------------------------------------

static void
onFsEvent(uv_fs_event_t *handle, const char *filename, int events, int status) {
  auto *wrap = static_cast<FSEventWrap *>(handle->data);
  if (!wrap || !wrap->env)
    return;

  napi_env env = wrap->env;

  // Open a handle scope for creating JS values in the callback.
  napi_handle_scope scope;
  napi_open_handle_scope(env, &scope);

  // Get the JS FSEvent object.
  napi_value thisObj;
  if (wrap->selfRef) {
    napi_get_reference_value(env, wrap->selfRef, &thisObj);
  } else {
    napi_get_undefined(env, &thisObj);
  }

  // Get the onchange callback.
  napi_value onchange;
  napi_get_named_property(env, thisObj, "onchange", &onchange);

  napi_valuetype onchangeType;
  napi_typeof(env, onchange, &onchangeType);
  if (onchangeType != napi_function) {
    napi_close_handle_scope(env, scope);
    return;
  }

  // Determine event type string.
  const char *eventStr = "";
  if (status != 0) {
    eventStr = "";
  } else if (events & UV_RENAME) {
    eventStr = "rename";
  } else if (events & UV_CHANGE) {
    eventStr = "change";
  }

  // Build arguments: (status, eventType, filename)
  napi_value args[3];
  napi_create_int32(env, status, &args[0]);
  napi_create_string_utf8(env, eventStr, NAPI_AUTO_LENGTH, &args[1]);
  if (filename) {
    napi_create_string_utf8(env, filename, NAPI_AUTO_LENGTH, &args[2]);
  } else {
    napi_get_null(env, &args[2]);
  }

  // Call onchange with the FSEvent object as `this`.
  napi_value retval;
  napi_call_function(env, thisObj, onchange, 3, args, &retval);

  // Check for exception and clear it (don't crash).
  bool hasPending = false;
  napi_is_exception_pending(env, &hasPending);
  if (hasPending) {
    napi_value exc;
    napi_get_and_clear_last_exception(env, &exc);
    // TODO: route to uncaughtException handler
  }

  napi_close_handle_scope(env, scope);
}

// ---------------------------------------------------------------------------
// initFsEventWrapBinding
// ---------------------------------------------------------------------------

napi_value initFsEventWrapBinding(napi_env env, napi_value exports) {
  // Create the FSEvent constructor function.
  napi_value ctorFn;
  napi_create_function(
      env, "FSEvent", NAPI_AUTO_LENGTH, fsEventNew, nullptr, &ctorFn);

  // Get the prototype object from the constructor.
  napi_value prototype;
  napi_get_named_property(env, ctorFn, "prototype", &prototype);

  // Set methods on prototype.
  napi_value fn;

  napi_create_function(
      env, "start", NAPI_AUTO_LENGTH, fsEventStart, nullptr, &fn);
  napi_set_named_property(env, prototype, "start", fn);

  napi_create_function(
      env, "close", NAPI_AUTO_LENGTH, fsEventClose, nullptr, &fn);
  napi_set_named_property(env, prototype, "close", fn);

  napi_create_function(env, "ref", NAPI_AUTO_LENGTH, fsEventRef, nullptr, &fn);
  napi_set_named_property(env, prototype, "ref", fn);

  napi_create_function(
      env, "unref", NAPI_AUTO_LENGTH, fsEventUnref, nullptr, &fn);
  napi_set_named_property(env, prototype, "unref", fn);

  napi_create_function(
      env, "hasRef", NAPI_AUTO_LENGTH, fsEventHasRef, nullptr, &fn);
  napi_set_named_property(env, prototype, "hasRef", fn);

  napi_create_function(
      env, "getAsyncId", NAPI_AUTO_LENGTH, fsEventGetAsyncId, nullptr, &fn);
  napi_set_named_property(env, prototype, "getAsyncId", fn);

  // Set "initialized" as a getter on the prototype.
  napi_property_descriptor initProp = {
      "initialized",
      nullptr,
      nullptr,
      fsEventGetInitialized,
      nullptr,
      nullptr,
      napi_enumerable,
      nullptr};
  napi_define_properties(env, prototype, 1, &initProp);

  // Export the constructor.
  napi_set_named_property(env, exports, "FSEvent", ctorFn);

  return exports;
}

} // namespace node_compat
} // namespace hermes
