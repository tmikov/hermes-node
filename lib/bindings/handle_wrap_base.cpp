/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <hermes/node-compat/bindings/handle_wrap_base.h>
#include <hermes/node-compat/runtime/runtime_state.h>
#include <node_api.h>
#include <uv.h>

#include <cassert>

namespace hermes {
namespace node_compat {

// ---------------------------------------------------------------------------
// HandleWrapBase
// ---------------------------------------------------------------------------

HandleWrapBase::HandleWrapBase() = default;

HandleWrapBase::~HandleWrapBase() {
  // Should have been cleaned up by close or finalizer.
  assert(state_ == kClosed || state_ == kClosing);
}

void HandleWrapBase::init(napi_env env, napi_value jsObj, uv_handle_t *handle) {
  env_ = env;
  rtState_ = getRuntimeState(env);
  handle_ = handle;
  handle_->data = this;
  state_ = kInitialized;

  // Wrap native data with the JS object. The pointerCb finalizer handles
  // cleanup if the JS object is garbage collected before close().
  napi_wrap(env, jsObj, this, pointerCb, nullptr, nullptr);

  // Create a prevent-GC reference so the handle stays alive while active.
  napi_create_reference(env, jsObj, 1, &selfRef_);
}

HandleWrapBase *HandleWrapBase::unwrap(napi_env env, napi_value obj) {
  void *data = nullptr;
  napi_unwrap(env, obj, &data);
  return static_cast<HandleWrapBase *>(data);
}

napi_value HandleWrapBase::getJsObject() const {
  if (!selfRef_)
    return nullptr;
  napi_value obj = nullptr;
  napi_get_reference_value(env_, selfRef_, &obj);
  return obj;
}

void HandleWrapBase::doClose(napi_value closeCallback) {
  if (state_ != kInitialized)
    return;

  state_ = kClosing;

  // Store close callback if provided.
  if (closeCallback) {
    napi_valuetype cbType;
    napi_typeof(env_, closeCallback, &cbType);
    if (cbType == napi_function) {
      napi_create_reference(env_, closeCallback, 1, &closeCbRef_);
    }
  }

  // Remove the napi_wrap so the GC finalizer won't try to double-close.
  // We now take ownership of the native data via the uv_close callback.
  napi_value jsObj = nullptr;
  if (selfRef_) {
    napi_get_reference_value(env_, selfRef_, &jsObj);
  }
  if (jsObj) {
    napi_remove_wrap(env_, jsObj, nullptr);
  }

  uv_close(handle_, uvCloseCb);
}

void HandleWrapBase::uvCloseCb(uv_handle_t *handle) {
  auto *wrap = static_cast<HandleWrapBase *>(handle->data);
  if (!wrap)
    return;

  wrap->state_ = kClosed;
  wrap->onClose();
  wrap->invokeCloseCallback();

  // Release the prevent-GC reference.
  if (wrap->selfRef_) {
    napi_delete_reference(wrap->env_, wrap->selfRef_);
    wrap->selfRef_ = nullptr;
  }
  if (wrap->closeCbRef_) {
    napi_delete_reference(wrap->env_, wrap->closeCbRef_);
    wrap->closeCbRef_ = nullptr;
  }

  delete wrap;
}

void HandleWrapBase::invokeCloseCallback() {
  if (!closeCbRef_)
    return;

  napi_handle_scope scope;
  napi_open_handle_scope(env_, &scope);

  napi_value cb;
  napi_get_reference_value(env_, closeCbRef_, &cb);
  if (cb) {
    napi_value undefined;
    napi_get_undefined(env_, &undefined);
    napi_value retval;
    napi_call_function(env_, undefined, cb, 0, nullptr, &retval);

    // Clear any pending exception.
    bool hasPending = false;
    napi_is_exception_pending(env_, &hasPending);
    if (hasPending) {
      napi_value exc;
      napi_get_and_clear_last_exception(env_, &exc);
    }
  }

  napi_close_handle_scope(env_, scope);
}

void HandleWrapBase::pointerCb(napi_env env, void *data, void *) {
  // GC finalizer: clean up the HandleWrapBase.
  auto *wrap = static_cast<HandleWrapBase *>(data);

  if (wrap->state_ == kInitialized && wrap->handle_) {
    auto *rtState = wrap->rtState_;
    if (!uv_is_closing(wrap->handle_) && rtState && rtState->loop) {
      // Handle is still active and loop is alive — async-close it.
      wrap->state_ = kClosing;
      uv_close(wrap->handle_, [](uv_handle_t *h) {
        auto *w = static_cast<HandleWrapBase *>(h->data);
        w->state_ = kClosed;
        w->onClose();
        if (w->selfRef_) {
          napi_delete_reference(w->env_, w->selfRef_);
          w->selfRef_ = nullptr;
        }
        if (w->closeCbRef_) {
          napi_delete_reference(w->env_, w->closeCbRef_);
          w->closeCbRef_ = nullptr;
        }
        delete w;
      });
      return;
    }
    // Handle was force-closed during shutdown (via uv_walk in
    // UvEventLoop::close) or the loop is already destroyed.
    // The NAPI env is being torn down so references will be cleaned up
    // automatically — just delete the wrap.
    wrap->state_ = kClosed;
    delete wrap;
    return;
  }

  if (wrap->state_ == kClosed) {
    delete wrap;
  }
  // If kClosing, uv_close callback will handle deletion.
}

void HandleWrapBase::onClose() {
  // Default: nothing. Subclasses override.
}

// ---------------------------------------------------------------------------
// JS method implementations
// ---------------------------------------------------------------------------

napi_value HandleWrapBase::ref(napi_env env, napi_callback_info info) {
  napi_value thisObj;
  napi_get_cb_info(env, info, nullptr, nullptr, &thisObj, nullptr);

  auto *wrap = unwrap(env, thisObj);
  if (wrap && wrap->state_ == kInitialized) {
    uv_ref(wrap->handle_);
  }
  return thisObj;
}

napi_value HandleWrapBase::unref(napi_env env, napi_callback_info info) {
  napi_value thisObj;
  napi_get_cb_info(env, info, nullptr, nullptr, &thisObj, nullptr);

  auto *wrap = unwrap(env, thisObj);
  if (wrap && wrap->state_ == kInitialized) {
    uv_unref(wrap->handle_);
  }
  return thisObj;
}

napi_value HandleWrapBase::hasRef(napi_env env, napi_callback_info info) {
  napi_value thisObj;
  napi_get_cb_info(env, info, nullptr, nullptr, &thisObj, nullptr);

  auto *wrap = unwrap(env, thisObj);
  bool has = false;
  if (wrap && wrap->state_ == kInitialized) {
    has = uv_has_ref(wrap->handle_) != 0;
  }

  napi_value result;
  napi_get_boolean(env, has, &result);
  return result;
}

napi_value HandleWrapBase::close(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_value thisObj;
  napi_get_cb_info(env, info, &argc, argv, &thisObj, nullptr);

  auto *wrap = unwrap(env, thisObj);
  if (!wrap)
    return nullptr;

  napi_value closeCallback = (argc > 0) ? argv[0] : nullptr;
  wrap->doClose(closeCallback);
  return nullptr;
}

napi_value HandleWrapBase::getAsyncId(napi_env env, napi_callback_info) {
  // Stub: async hooks are not implemented.
  napi_value result;
  napi_create_double(env, 0.0, &result);
  return result;
}

// ---------------------------------------------------------------------------
// addHandleWrapMethods
// ---------------------------------------------------------------------------

void HandleWrapBase::addHandleWrapMethods(napi_env env, napi_value prototype) {
  napi_value fn;

  napi_create_function(
      env, "ref", NAPI_AUTO_LENGTH, HandleWrapBase::ref, nullptr, &fn);
  napi_set_named_property(env, prototype, "ref", fn);

  napi_create_function(
      env, "unref", NAPI_AUTO_LENGTH, HandleWrapBase::unref, nullptr, &fn);
  napi_set_named_property(env, prototype, "unref", fn);

  napi_create_function(
      env, "hasRef", NAPI_AUTO_LENGTH, HandleWrapBase::hasRef, nullptr, &fn);
  napi_set_named_property(env, prototype, "hasRef", fn);

  napi_create_function(
      env, "close", NAPI_AUTO_LENGTH, HandleWrapBase::close, nullptr, &fn);
  napi_set_named_property(env, prototype, "close", fn);

  napi_create_function(
      env,
      "getAsyncId",
      NAPI_AUTO_LENGTH,
      HandleWrapBase::getAsyncId,
      nullptr,
      &fn);
  napi_set_named_property(env, prototype, "getAsyncId", fn);
}

} // namespace node_compat
} // namespace hermes
