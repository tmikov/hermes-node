/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <hermes/node-compat/bindings/handle_wrap_base.h>
#include <hermes/node-compat/bindings/libuv_stream_base.h>
#include <hermes/node-compat/bindings/node_pipe_wrap.h>
#include <hermes/node-compat/runtime/runtime_state.h>
#include <node_api.h>
#include <uv.h>

#include <cassert>
#include <cstring>

namespace hermes {
namespace node_compat {

// ---------------------------------------------------------------------------
// PipeConnectReqData -- native data for uv_connect_t requests
// ---------------------------------------------------------------------------

struct PipeConnectReqData {
  uv_connect_t req;
  napi_env env;
  napi_ref reqRef; // prevent-GC ref to JS request object

  PipeConnectReqData() : env(nullptr), reqRef(nullptr) {
    memset(&req, 0, sizeof(req));
  }
};

// ---------------------------------------------------------------------------
// PipeWrap -- wraps uv_pipe_t, inherits from LibuvStreamBase
// ---------------------------------------------------------------------------

class PipeWrap : public LibuvStreamBase {
 public:
  enum SocketType { SOCKET = 0, SERVER = 1, IPC = 2 };

  /// Construct a PipeWrap.
  PipeWrap(napi_env env, napi_value jsObj, bool ipc) : handle_() {
    int err = uv_pipe_init(getRuntimeState(env)->loop, &handle_, ipc ? 1 : 0);
    if (err != 0) {
      // uv_pipe_init should not fail under normal circumstances.
      // If it does, we don't call initStream, and HandleWrapBase
      // destructor will safely delete (state_ == kClosed).
      return;
    }
    initStream(env, jsObj, reinterpret_cast<uv_stream_t *>(&handle_));
  }

 private:
  uv_pipe_t handle_;

  // --- NAPI callbacks ---

  /// new Pipe(type)
  static napi_value New(napi_env env, napi_callback_info info) {
    napi_handle_scope scope;
    napi_open_handle_scope(env, &scope);

    size_t argc = 1;
    napi_value argv[1];
    napi_value thisObj;
    napi_get_cb_info(env, info, &argc, argv, &thisObj, nullptr);

    int32_t typeVal = SOCKET;
    if (argc >= 1)
      napi_get_value_int32(env, argv[0], &typeVal);

    bool ipc = (typeVal == IPC);

    // Create the PipeWrap (it self-registers via initStream).
    new PipeWrap(env, thisObj, ipc);

    // Set initial properties that Node's JS code expects.
    napi_value falseVal;
    napi_get_boolean(env, false, &falseVal);
    napi_set_named_property(env, thisObj, "reading", falseVal);

    napi_value nullVal;
    napi_get_null(env, &nullVal);
    napi_set_named_property(env, thisObj, "onconnection", nullVal);

    napi_close_handle_scope(env, scope);
    return thisObj;
  }

  /// open(fd) -> err
  static napi_value Open(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1];
    napi_value thisObj;
    napi_get_cb_info(env, info, &argc, argv, &thisObj, nullptr);

    auto *wrap = static_cast<PipeWrap *>(HandleWrapBase::unwrap(env, thisObj));
    if (!wrap) {
      napi_value result;
      napi_create_int32(env, UV_EBADF, &result);
      return result;
    }

    int64_t fd = 0;
    napi_get_value_int64(env, argv[0], &fd);

    int err = uv_pipe_open(&wrap->handle_, static_cast<uv_file>(fd));

    napi_value result;
    napi_create_int32(env, err, &result);
    return result;
  }

  /// bind(name) -> err
  static napi_value Bind(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1];
    napi_value thisObj;
    napi_get_cb_info(env, info, &argc, argv, &thisObj, nullptr);

    auto *wrap = static_cast<PipeWrap *>(HandleWrapBase::unwrap(env, thisObj));
    if (!wrap) {
      napi_value result;
      napi_create_int32(env, UV_EBADF, &result);
      return result;
    }

    char nameBuf[4096];
    size_t nameLen = 0;
    napi_get_value_string_utf8(
        env, argv[0], nameBuf, sizeof(nameBuf), &nameLen);

    int err = uv_pipe_bind(&wrap->handle_, nameBuf);

    napi_value result;
    napi_create_int32(env, err, &result);
    return result;
  }

  /// listen(backlog) -> err
  static napi_value Listen(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1];
    napi_value thisObj;
    napi_get_cb_info(env, info, &argc, argv, &thisObj, nullptr);

    auto *wrap = static_cast<PipeWrap *>(HandleWrapBase::unwrap(env, thisObj));
    if (!wrap) {
      napi_value result;
      napi_create_int32(env, UV_EBADF, &result);
      return result;
    }

    int32_t backlog = 128;
    napi_get_value_int32(env, argv[0], &backlog);

    int err = uv_listen(
        reinterpret_cast<uv_stream_t *>(&wrap->handle_), backlog, OnConnection);

    napi_value result;
    napi_create_int32(env, err, &result);
    return result;
  }

  /// libuv connection callback -- fired when a new client connects.
  static void OnConnection(uv_stream_t *handle, int status) {
    auto *wrap = static_cast<PipeWrap *>(handle->data);
    if (!wrap || !wrap->env())
      return;

    napi_env env = wrap->env();
    napi_handle_scope scope;
    napi_open_handle_scope(env, &scope);

    napi_value serverObj = wrap->getJsObject();
    if (!serverObj) {
      napi_close_handle_scope(env, scope);
      return;
    }

    napi_value clientHandle;
    if (status == 0) {
      // Instantiate a new Pipe object for the client.
      napi_value pipeCtor;
      napi_get_reference_value(
          env, getRuntimeState(env)->pipeCtorRef, &pipeCtor);

      napi_value typeArg;
      napi_create_int32(env, SOCKET, &typeArg);

      napi_value clientObj;
      napi_status st =
          napi_new_instance(env, pipeCtor, 1, &typeArg, &clientObj);
      if (st != napi_ok) {
        napi_close_handle_scope(env, scope);
        return;
      }

      // Unwrap the client and accept the connection.
      auto *clientWrap =
          static_cast<PipeWrap *>(HandleWrapBase::unwrap(env, clientObj));
      if (!clientWrap) {
        napi_close_handle_scope(env, scope);
        return;
      }

      int acceptErr = uv_accept(
          handle, reinterpret_cast<uv_stream_t *>(&clientWrap->handle_));
      if (acceptErr != 0) {
        napi_close_handle_scope(env, scope);
        return;
      }

      clientHandle = clientObj;
    } else {
      napi_get_undefined(env, &clientHandle);
    }

    // Call server.onconnection(status, clientHandle).
    napi_value onconnection;
    napi_get_named_property(env, serverObj, "onconnection", &onconnection);

    napi_valuetype onconnType;
    napi_typeof(env, onconnection, &onconnType);
    if (onconnType == napi_function) {
      napi_value args[2];
      napi_create_int32(env, status, &args[0]);
      args[1] = clientHandle;
      napi_value retval;
      napi_call_function(env, serverObj, onconnection, 2, args, &retval);

      bool hasPending = false;
      napi_is_exception_pending(env, &hasPending);
      if (hasPending) {
        napi_value exc;
        napi_get_and_clear_last_exception(env, &exc);
      }
    }

    napi_close_handle_scope(env, scope);
  }

  /// connect(req, name) -> err
  static napi_value Connect(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2];
    napi_value thisObj;
    napi_get_cb_info(env, info, &argc, argv, &thisObj, nullptr);

    auto *wrap = static_cast<PipeWrap *>(HandleWrapBase::unwrap(env, thisObj));
    if (!wrap) {
      napi_value result;
      napi_create_int32(env, UV_EBADF, &result);
      return result;
    }

    napi_value reqObj = argv[0];

    char nameBuf[4096];
    size_t nameLen = 0;
    napi_get_value_string_utf8(
        env, argv[1], nameBuf, sizeof(nameBuf), &nameLen);

    auto *reqData = new PipeConnectReqData();
    reqData->env = env;
    reqData->req.data = reqData;
    napi_create_reference(env, reqObj, 1, &reqData->reqRef);

    uv_pipe_connect(&reqData->req, &wrap->handle_, nameBuf, AfterConnect);

    // uv_pipe_connect does not return an error code; errors are
    // reported asynchronously via the callback.
    napi_value result;
    napi_create_int32(env, 0, &result);
    return result;
  }

  /// libuv connect callback.
  static void AfterConnect(uv_connect_t *req, int status) {
    auto *reqData = static_cast<PipeConnectReqData *>(req->data);
    if (!reqData)
      return;

    napi_env env = reqData->env;

    napi_handle_scope scope;
    napi_open_handle_scope(env, &scope);

    napi_value reqObj;
    napi_get_reference_value(env, reqData->reqRef, &reqObj);

    // Get the handle (Pipe) object from req->handle->data.
    auto *wrap = static_cast<PipeWrap *>(req->handle->data);
    napi_value handleObj = wrap ? wrap->getJsObject() : nullptr;
    if (!handleObj)
      napi_get_undefined(env, &handleObj);

    bool readable = false, writable = false;
    if (status == 0) {
      readable = uv_is_readable(req->handle) != 0;
      writable = uv_is_writable(req->handle) != 0;
    }

    // Call reqObj.oncomplete(status, handle, req, readable, writable).
    napi_value oncomplete;
    napi_get_named_property(env, reqObj, "oncomplete", &oncomplete);

    napi_valuetype oncompType;
    napi_typeof(env, oncomplete, &oncompType);
    if (oncompType == napi_function) {
      napi_value args[5];
      napi_create_int32(env, status, &args[0]);
      args[1] = handleObj;
      args[2] = reqObj;
      napi_get_boolean(env, readable, &args[3]);
      napi_get_boolean(env, writable, &args[4]);
      napi_value retval;
      napi_call_function(env, reqObj, oncomplete, 5, args, &retval);

      bool hasPending = false;
      napi_is_exception_pending(env, &hasPending);
      if (hasPending) {
        napi_value exc;
        napi_get_and_clear_last_exception(env, &exc);
      }
    }

    napi_delete_reference(env, reqData->reqRef);
    delete reqData;

    napi_close_handle_scope(env, scope);
  }

  /// fchmod(mode) -> err
  static napi_value Fchmod(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1];
    napi_value thisObj;
    napi_get_cb_info(env, info, &argc, argv, &thisObj, nullptr);

    auto *wrap = static_cast<PipeWrap *>(HandleWrapBase::unwrap(env, thisObj));
    if (!wrap) {
      napi_value result;
      napi_create_int32(env, UV_EBADF, &result);
      return result;
    }

    int32_t mode = 0;
    napi_get_value_int32(env, argv[0], &mode);

    int err = uv_pipe_chmod(&wrap->handle_, mode);

    napi_value result;
    napi_create_int32(env, err, &result);
    return result;
  }

  /// getsockname(out) -> err
  /// For pipes, this writes {address: <path>} to the output object.
  static napi_value GetSockName(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1];
    napi_value thisObj;
    napi_get_cb_info(env, info, &argc, argv, &thisObj, nullptr);

    auto *wrap = static_cast<PipeWrap *>(HandleWrapBase::unwrap(env, thisObj));
    if (!wrap) {
      napi_value result;
      napi_create_int32(env, UV_EBADF, &result);
      return result;
    }

    char nameBuf[4096];
    size_t nameLen = sizeof(nameBuf);
    int err = uv_pipe_getsockname(&wrap->handle_, nameBuf, &nameLen);

    if (err == 0) {
      napi_value addrVal;
      napi_create_string_utf8(env, nameBuf, nameLen, &addrVal);
      napi_set_named_property(env, argv[0], "address", addrVal);
    }

    napi_value result;
    napi_create_int32(env, err, &result);
    return result;
  }

  /// getpeername(out) -> err
  /// For pipes, this writes {address: <path>} to the output object.
  static napi_value GetPeerName(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1];
    napi_value thisObj;
    napi_get_cb_info(env, info, &argc, argv, &thisObj, nullptr);

    auto *wrap = static_cast<PipeWrap *>(HandleWrapBase::unwrap(env, thisObj));
    if (!wrap) {
      napi_value result;
      napi_create_int32(env, UV_EBADF, &result);
      return result;
    }

    char nameBuf[4096];
    size_t nameLen = sizeof(nameBuf);
    int err = uv_pipe_getpeername(&wrap->handle_, nameBuf, &nameLen);

    if (err == 0) {
      napi_value addrVal;
      napi_create_string_utf8(env, nameBuf, nameLen, &addrVal);
      napi_set_named_property(env, argv[0], "address", addrVal);
    }

    napi_value result;
    napi_create_int32(env, err, &result);
    return result;
  }

  friend napi_value initPipeWrapBinding(napi_env env, napi_value exports);
};

// ---------------------------------------------------------------------------
// initPipeWrapBinding
// ---------------------------------------------------------------------------

napi_value initPipeWrapBinding(napi_env env, napi_value exports) {
  napi_handle_scope scope;
  napi_open_handle_scope(env, &scope);

  // --- Pipe constructor ---
  napi_value pipeCtor;
  napi_define_class(
      env,
      "Pipe",
      NAPI_AUTO_LENGTH,
      PipeWrap::New,
      nullptr,
      0,
      nullptr,
      &pipeCtor);

  // Get the prototype and add stream + Pipe-specific methods.
  napi_value prototype;
  napi_get_named_property(env, pipeCtor, "prototype", &prototype);

  // Add all stream methods (readStart/readStop/write*/shutdown etc.)
  // plus handle methods (ref/unref/hasRef/close/getAsyncId).
  LibuvStreamBase::addStreamMethods(env, prototype);

  // Add Pipe-specific methods.
  napi_property_descriptor pipeProps[] = {
      {"open",
       nullptr,
       PipeWrap::Open,
       nullptr,
       nullptr,
       nullptr,
       napi_default,
       nullptr},
      {"bind",
       nullptr,
       PipeWrap::Bind,
       nullptr,
       nullptr,
       nullptr,
       napi_default,
       nullptr},
      {"listen",
       nullptr,
       PipeWrap::Listen,
       nullptr,
       nullptr,
       nullptr,
       napi_default,
       nullptr},
      {"connect",
       nullptr,
       PipeWrap::Connect,
       nullptr,
       nullptr,
       nullptr,
       napi_default,
       nullptr},
      {"fchmod",
       nullptr,
       PipeWrap::Fchmod,
       nullptr,
       nullptr,
       nullptr,
       napi_default,
       nullptr},
      {"getsockname",
       nullptr,
       PipeWrap::GetSockName,
       nullptr,
       nullptr,
       nullptr,
       napi_default,
       nullptr},
      {"getpeername",
       nullptr,
       PipeWrap::GetPeerName,
       nullptr,
       nullptr,
       nullptr,
       napi_default,
       nullptr},
  };
  napi_define_properties(
      env, prototype, sizeof(pipeProps) / sizeof(pipeProps[0]), pipeProps);

  // --- Export Pipe constructor ---
  napi_set_named_property(env, exports, "Pipe", pipeCtor);

  // Store a reference to the constructor for use in OnConnection.
  napi_create_reference(env, pipeCtor, 1, &getRuntimeState(env)->pipeCtorRef);

  // --- PipeConnectWrap constructor ---
  napi_value connectWrapCtor;
  napi_define_class(
      env,
      "PipeConnectWrap",
      NAPI_AUTO_LENGTH,
      [](napi_env env, napi_callback_info info) -> napi_value {
        napi_value thisObj;
        napi_get_cb_info(env, info, nullptr, nullptr, &thisObj, nullptr);
        return thisObj;
      },
      nullptr,
      0,
      nullptr,
      &connectWrapCtor);
  napi_set_named_property(env, exports, "PipeConnectWrap", connectWrapCtor);

  // --- Constants ---
  napi_value constants;
  napi_create_object(env, &constants);

  napi_value val;
  napi_create_int32(env, PipeWrap::SOCKET, &val);
  napi_set_named_property(env, constants, "SOCKET", val);
  napi_create_int32(env, PipeWrap::SERVER, &val);
  napi_set_named_property(env, constants, "SERVER", val);
  napi_create_int32(env, PipeWrap::IPC, &val);
  napi_set_named_property(env, constants, "IPC", val);
  napi_create_int32(env, UV_READABLE, &val);
  napi_set_named_property(env, constants, "UV_READABLE", val);
  napi_create_int32(env, UV_WRITABLE, &val);
  napi_set_named_property(env, constants, "UV_WRITABLE", val);

  napi_set_named_property(env, exports, "constants", constants);

  napi_close_handle_scope(env, scope);
  return exports;
}

} // namespace node_compat
} // namespace hermes
