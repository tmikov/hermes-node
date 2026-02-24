/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_NODE_COMPAT_BINDINGS_HANDLE_WRAP_BASE_H
#define HERMES_NODE_COMPAT_BINDINGS_HANDLE_WRAP_BASE_H

#include <node_api_types.h>

struct uv_handle_s;
typedef struct uv_handle_s uv_handle_t;

namespace hermes {
namespace node_compat {
struct RuntimeState;
} // namespace node_compat
} // namespace hermes

namespace hermes {
namespace node_compat {

/// Base class for all libuv handle wraps (TCP, Pipe, TTY, UDP, etc.).
/// Provides ref/unref/hasRef/close lifecycle management.
///
/// Subclasses:
/// 1. Define their own constructor that initializes the specific uv handle
/// 2. Call HandleWrapBase::init() to set up the wrap
/// 3. Call HandleWrapBase::addHandleWrapMethods() on their prototype
///
/// Lifecycle:
///   Constructed -> Initialized -> Closing -> Closed
///
/// The handle is ref-counted via napi_ref to prevent GC while active.
/// On GC (finalizer), if not yet closed, we async-close the handle.
class HandleWrapBase {
 public:
  enum State { kInitialized, kClosing, kClosed };

  HandleWrapBase();
  virtual ~HandleWrapBase();

  uv_handle_t *handle() const {
    return handle_;
  }
  napi_env env() const {
    return env_;
  }
  State state() const {
    return state_;
  }

  /// Get the JS object associated with this wrap (from the prevent-GC ref).
  /// Returns nullptr if there is no reference.
  napi_value getJsObject() const;

  /// Initialize the wrap. Must be called after the uv handle has been
  /// initialized (e.g. after uv_tcp_init). Sets handle->data = this.
  /// Creates a prevent-GC reference.
  void init(napi_env env, napi_value jsObj, uv_handle_t *handle);

  /// Add ref/unref/hasRef/close/getAsyncId methods to a JS prototype.
  static void addHandleWrapMethods(napi_env env, napi_value prototype);

  /// NAPI callback implementations for prototype methods.
  static napi_value ref(napi_env env, napi_callback_info info);
  static napi_value unref(napi_env env, napi_callback_info info);
  static napi_value hasRef(napi_env env, napi_callback_info info);
  static napi_value close(napi_env env, napi_callback_info info);
  static napi_value getAsyncId(napi_env env, napi_callback_info info);

  /// Retrieve the HandleWrapBase from a JS object via napi_unwrap.
  static HandleWrapBase *unwrap(napi_env env, napi_value obj);

  /// Close the handle. If a callback is provided, it will be called
  /// after the handle is closed.
  void doClose(napi_value closeCallback = nullptr);

 protected:
  /// Called when uv_close completes. Subclasses can override for cleanup.
  virtual void onClose();

 private:
  static void uvCloseCb(uv_handle_t *handle);
  static void pointerCb(napi_env env, void *data, void *hint);
  // Check if close callback should be invoked, and invoke it.
  void invokeCloseCallback();

  uv_handle_t *handle_ = nullptr;
  napi_env env_ = nullptr;
  RuntimeState *rtState_ = nullptr; // cached for GC finalizer safety
  napi_ref selfRef_ = nullptr; // prevent-GC reference
  napi_ref closeCbRef_ = nullptr; // optional close callback
  State state_ = kClosed;
};

} // namespace node_compat
} // namespace hermes

#endif // HERMES_NODE_COMPAT_BINDINGS_HANDLE_WRAP_BASE_H
