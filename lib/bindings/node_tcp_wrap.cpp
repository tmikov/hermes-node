/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <hermes/node-compat/bindings/node_tcp_wrap.h>
#include <hermes/node-compat/bindings/handle_wrap_base.h>
#include <hermes/node-compat/bindings/libuv_stream_base.h>
#include <node_api.h>
#include <uv.h>

#include <cassert>
#include <cstring>

namespace hermes {
namespace node_compat {

// Module-level reference to the TCP constructor. Used by OnConnection
// to instantiate client TCP objects.
static napi_ref s_tcpCtorRef = nullptr;

// ---------------------------------------------------------------------------
// ConnectReqData — native data for uv_connect_t requests
// ---------------------------------------------------------------------------

struct ConnectReqData {
  uv_connect_t req;
  napi_env env;
  napi_ref reqRef; // prevent-GC ref to JS request object

  ConnectReqData() : env(nullptr), reqRef(nullptr) {
    memset(&req, 0, sizeof(req));
  }
};

// ---------------------------------------------------------------------------
// TCPWrap — wraps uv_tcp_t, inherits from LibuvStreamBase
// ---------------------------------------------------------------------------

class TCPWrap : public LibuvStreamBase {
 public:
  enum SocketType { SOCKET = 0, SERVER = 1 };

  /// Construct a TCPWrap.
  TCPWrap(napi_env env, napi_value jsObj) : handle_() {
    int err = uv_tcp_init(getHandleWrapEventLoop(), &handle_);
    if (err != 0) {
      // uv_tcp_init should not fail under normal circumstances.
      // Node also CHECK_EQ(r, 0) here. If it does fail, we don't
      // call initStream, and the HandleWrapBase destructor will
      // safely delete (state_ == kClosed).
      return;
    }
    initStream(env, jsObj, reinterpret_cast<uv_stream_t *>(&handle_));
  }

 private:
  uv_tcp_t handle_;

  // --- NAPI callbacks ---

  /// new TCP(type)
  static napi_value New(napi_env env, napi_callback_info info) {
    napi_handle_scope scope;
    napi_open_handle_scope(env, &scope);

    size_t argc = 1;
    napi_value argv[1];
    napi_value thisObj;
    napi_get_cb_info(env, info, &argc, argv, &thisObj, nullptr);

    // Create the TCPWrap (it self-registers via initStream).
    new TCPWrap(env, thisObj);

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

    auto *wrap = static_cast<TCPWrap *>(HandleWrapBase::unwrap(env, thisObj));
    if (!wrap) {
      napi_value result;
      napi_create_int32(env, UV_EBADF, &result);
      return result;
    }

    int64_t fd = 0;
    napi_get_value_int64(env, argv[0], &fd);

    int err = uv_tcp_open(&wrap->handle_, static_cast<uv_os_sock_t>(fd));

    napi_value result;
    napi_create_int32(env, err, &result);
    return result;
  }

  /// bind(addr, port, flags) -> err
  static napi_value Bind(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value argv[3];
    napi_value thisObj;
    napi_get_cb_info(env, info, &argc, argv, &thisObj, nullptr);

    auto *wrap = static_cast<TCPWrap *>(HandleWrapBase::unwrap(env, thisObj));
    if (!wrap) {
      napi_value result;
      napi_create_int32(env, UV_EBADF, &result);
      return result;
    }

    char ipBuf[256];
    size_t ipLen = 0;
    napi_get_value_string_utf8(env, argv[0], ipBuf, sizeof(ipBuf), &ipLen);

    int32_t port = 0;
    napi_get_value_int32(env, argv[1], &port);

    uint32_t flags = 0;
    if (argc >= 3) {
      napi_valuetype flagsType;
      napi_typeof(env, argv[2], &flagsType);
      if (flagsType == napi_number) {
        napi_get_value_uint32(env, argv[2], &flags);
        // Cannot set IPV6 flags on IPv4 socket.
        flags &= ~static_cast<uint32_t>(UV_TCP_IPV6ONLY);
      }
    }

    sockaddr_in addr;
    int err = uv_ip4_addr(ipBuf, port, &addr);
    if (err == 0) {
      err = uv_tcp_bind(
          &wrap->handle_, reinterpret_cast<const sockaddr *>(&addr), flags);
    }

    napi_value result;
    napi_create_int32(env, err, &result);
    return result;
  }

  /// bind6(addr, port, flags) -> err
  static napi_value Bind6(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value argv[3];
    napi_value thisObj;
    napi_get_cb_info(env, info, &argc, argv, &thisObj, nullptr);

    auto *wrap = static_cast<TCPWrap *>(HandleWrapBase::unwrap(env, thisObj));
    if (!wrap) {
      napi_value result;
      napi_create_int32(env, UV_EBADF, &result);
      return result;
    }

    char ipBuf[256];
    size_t ipLen = 0;
    napi_get_value_string_utf8(env, argv[0], ipBuf, sizeof(ipBuf), &ipLen);

    int32_t port = 0;
    napi_get_value_int32(env, argv[1], &port);

    uint32_t flags = 0;
    if (argc >= 3) {
      napi_valuetype flagsType;
      napi_typeof(env, argv[2], &flagsType);
      if (flagsType == napi_number)
        napi_get_value_uint32(env, argv[2], &flags);
    }

    sockaddr_in6 addr;
    int err = uv_ip6_addr(ipBuf, port, &addr);
    if (err == 0) {
      err = uv_tcp_bind(
          &wrap->handle_, reinterpret_cast<const sockaddr *>(&addr), flags);
    }

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

    auto *wrap = static_cast<TCPWrap *>(HandleWrapBase::unwrap(env, thisObj));
    if (!wrap) {
      napi_value result;
      napi_create_int32(env, UV_EBADF, &result);
      return result;
    }

    int32_t backlog = 128;
    napi_get_value_int32(env, argv[0], &backlog);

    int err = uv_listen(
        reinterpret_cast<uv_stream_t *>(&wrap->handle_),
        backlog,
        OnConnection);

    napi_value result;
    napi_create_int32(env, err, &result);
    return result;
  }

  /// libuv connection callback — fired when a new client connects.
  static void OnConnection(uv_stream_t *handle, int status) {
    auto *wrap = static_cast<TCPWrap *>(handle->data);
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
      // Instantiate a new TCP object for the client.
      napi_value tcpCtor;
      napi_get_reference_value(env, s_tcpCtorRef, &tcpCtor);

      napi_value typeArg;
      napi_create_int32(env, SOCKET, &typeArg);

      napi_value clientObj;
      napi_status st =
          napi_new_instance(env, tcpCtor, 1, &typeArg, &clientObj);
      if (st != napi_ok) {
        napi_close_handle_scope(env, scope);
        return;
      }

      // Unwrap the client and accept the connection.
      auto *clientWrap =
          static_cast<TCPWrap *>(HandleWrapBase::unwrap(env, clientObj));
      if (!clientWrap) {
        napi_close_handle_scope(env, scope);
        return;
      }

      int acceptErr = uv_accept(
          handle,
          reinterpret_cast<uv_stream_t *>(&clientWrap->handle_));
      if (acceptErr != 0) {
        // Accept failed (e.g. connection already closed). Just return.
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

  /// connect(req, addr, port) -> err
  static napi_value Connect(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value argv[3];
    napi_value thisObj;
    napi_get_cb_info(env, info, &argc, argv, &thisObj, nullptr);

    auto *wrap = static_cast<TCPWrap *>(HandleWrapBase::unwrap(env, thisObj));
    if (!wrap) {
      napi_value result;
      napi_create_int32(env, UV_EBADF, &result);
      return result;
    }

    napi_value reqObj = argv[0];

    char ipBuf[256];
    size_t ipLen = 0;
    napi_get_value_string_utf8(env, argv[1], ipBuf, sizeof(ipBuf), &ipLen);

    uint32_t port = 0;
    napi_get_value_uint32(env, argv[2], &port);

    sockaddr_in addr;
    int err = uv_ip4_addr(ipBuf, static_cast<int>(port), &addr);
    if (err != 0) {
      napi_value result;
      napi_create_int32(env, err, &result);
      return result;
    }

    auto *reqData = new ConnectReqData();
    reqData->env = env;
    reqData->req.data = reqData;
    napi_create_reference(env, reqObj, 1, &reqData->reqRef);

    err = uv_tcp_connect(
        &reqData->req,
        &wrap->handle_,
        reinterpret_cast<const sockaddr *>(&addr),
        AfterConnect);

    if (err != 0) {
      napi_delete_reference(env, reqData->reqRef);
      delete reqData;
    }

    napi_value result;
    napi_create_int32(env, err, &result);
    return result;
  }

  /// connect6(req, addr, port) -> err
  static napi_value Connect6(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value argv[3];
    napi_value thisObj;
    napi_get_cb_info(env, info, &argc, argv, &thisObj, nullptr);

    auto *wrap = static_cast<TCPWrap *>(HandleWrapBase::unwrap(env, thisObj));
    if (!wrap) {
      napi_value result;
      napi_create_int32(env, UV_EBADF, &result);
      return result;
    }

    napi_value reqObj = argv[0];

    char ipBuf[256];
    size_t ipLen = 0;
    napi_get_value_string_utf8(env, argv[1], ipBuf, sizeof(ipBuf), &ipLen);

    int32_t port = 0;
    napi_get_value_int32(env, argv[2], &port);

    sockaddr_in6 addr;
    int err = uv_ip6_addr(ipBuf, port, &addr);
    if (err != 0) {
      napi_value result;
      napi_create_int32(env, err, &result);
      return result;
    }

    auto *reqData = new ConnectReqData();
    reqData->env = env;
    reqData->req.data = reqData;
    napi_create_reference(env, reqObj, 1, &reqData->reqRef);

    err = uv_tcp_connect(
        &reqData->req,
        &wrap->handle_,
        reinterpret_cast<const sockaddr *>(&addr),
        AfterConnect);

    if (err != 0) {
      napi_delete_reference(env, reqData->reqRef);
      delete reqData;
    }

    napi_value result;
    napi_create_int32(env, err, &result);
    return result;
  }

  /// libuv connect callback.
  static void AfterConnect(uv_connect_t *req, int status) {
    auto *reqData = static_cast<ConnectReqData *>(req->data);
    if (!reqData)
      return;

    napi_env env = reqData->env;

    napi_handle_scope scope;
    napi_open_handle_scope(env, &scope);

    napi_value reqObj;
    napi_get_reference_value(env, reqData->reqRef, &reqObj);

    // Get the handle (TCP) object from req->handle->data.
    auto *wrap = static_cast<TCPWrap *>(req->handle->data);
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

  /// Helper to write sockaddr info into a JS object.
  static void AddressToJS(
      napi_env env,
      const sockaddr *addr,
      napi_value out) {
    char ip[INET6_ADDRSTRLEN];
    int port = 0;

    if (addr->sa_family == AF_INET) {
      auto *a4 = reinterpret_cast<const sockaddr_in *>(addr);
      uv_inet_ntop(AF_INET, &a4->sin_addr, ip, sizeof(ip));
      port = ntohs(a4->sin_port);

      napi_value addrVal, familyVal, portVal;
      napi_create_string_utf8(env, ip, NAPI_AUTO_LENGTH, &addrVal);
      napi_create_string_utf8(env, "IPv4", NAPI_AUTO_LENGTH, &familyVal);
      napi_create_int32(env, port, &portVal);
      napi_set_named_property(env, out, "address", addrVal);
      napi_set_named_property(env, out, "family", familyVal);
      napi_set_named_property(env, out, "port", portVal);
    } else if (addr->sa_family == AF_INET6) {
      auto *a6 = reinterpret_cast<const sockaddr_in6 *>(addr);
      uv_inet_ntop(AF_INET6, &a6->sin6_addr, ip, sizeof(ip));
      port = ntohs(a6->sin6_port);

      napi_value addrVal, familyVal, portVal;
      napi_create_string_utf8(env, ip, NAPI_AUTO_LENGTH, &addrVal);
      napi_create_string_utf8(env, "IPv6", NAPI_AUTO_LENGTH, &familyVal);
      napi_create_int32(env, port, &portVal);
      napi_set_named_property(env, out, "address", addrVal);
      napi_set_named_property(env, out, "family", familyVal);
      napi_set_named_property(env, out, "port", portVal);
    }
  }

  /// getsockname(out) -> err
  static napi_value GetSockName(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1];
    napi_value thisObj;
    napi_get_cb_info(env, info, &argc, argv, &thisObj, nullptr);

    auto *wrap = static_cast<TCPWrap *>(HandleWrapBase::unwrap(env, thisObj));
    if (!wrap) {
      napi_value result;
      napi_create_int32(env, UV_EBADF, &result);
      return result;
    }

    sockaddr_storage storage;
    int addrLen = sizeof(storage);
    int err = uv_tcp_getsockname(
        &wrap->handle_,
        reinterpret_cast<sockaddr *>(&storage),
        &addrLen);

    if (err == 0) {
      AddressToJS(env, reinterpret_cast<const sockaddr *>(&storage), argv[0]);
    }

    napi_value result;
    napi_create_int32(env, err, &result);
    return result;
  }

  /// getpeername(out) -> err
  static napi_value GetPeerName(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1];
    napi_value thisObj;
    napi_get_cb_info(env, info, &argc, argv, &thisObj, nullptr);

    auto *wrap = static_cast<TCPWrap *>(HandleWrapBase::unwrap(env, thisObj));
    if (!wrap) {
      napi_value result;
      napi_create_int32(env, UV_EBADF, &result);
      return result;
    }

    sockaddr_storage storage;
    int addrLen = sizeof(storage);
    int err = uv_tcp_getpeername(
        &wrap->handle_,
        reinterpret_cast<sockaddr *>(&storage),
        &addrLen);

    if (err == 0) {
      AddressToJS(env, reinterpret_cast<const sockaddr *>(&storage), argv[0]);
    }

    napi_value result;
    napi_create_int32(env, err, &result);
    return result;
  }

  /// setNoDelay(enable) -> err
  static napi_value SetNoDelay(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1];
    napi_value thisObj;
    napi_get_cb_info(env, info, &argc, argv, &thisObj, nullptr);

    auto *wrap = static_cast<TCPWrap *>(HandleWrapBase::unwrap(env, thisObj));
    if (!wrap) {
      napi_value result;
      napi_create_int32(env, UV_EBADF, &result);
      return result;
    }

    bool enable = false;
    napi_get_value_bool(env, argv[0], &enable);

    int err = uv_tcp_nodelay(&wrap->handle_, enable ? 1 : 0);

    napi_value result;
    napi_create_int32(env, err, &result);
    return result;
  }

  /// setKeepAlive(enable, delay) -> err
  static napi_value SetKeepAlive(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2];
    napi_value thisObj;
    napi_get_cb_info(env, info, &argc, argv, &thisObj, nullptr);

    auto *wrap = static_cast<TCPWrap *>(HandleWrapBase::unwrap(env, thisObj));
    if (!wrap) {
      napi_value result;
      napi_create_int32(env, UV_EBADF, &result);
      return result;
    }

    int32_t enable = 0;
    napi_get_value_int32(env, argv[0], &enable);

    uint32_t delay = 0;
    if (argc >= 2)
      napi_get_value_uint32(env, argv[1], &delay);

    int err = uv_tcp_keepalive(&wrap->handle_, enable, delay);

    napi_value result;
    napi_create_int32(env, err, &result);
    return result;
  }

  /// reset(callback) -> err
  /// Resets the TCP connection. We implement this as a regular close
  /// (FIN) since uv_tcp_close_reset requires careful state management
  /// and the RST vs FIN distinction rarely matters in practice.
  static napi_value Reset(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1];
    napi_value thisObj;
    napi_get_cb_info(env, info, &argc, argv, &thisObj, nullptr);

    auto *wrap = static_cast<TCPWrap *>(HandleWrapBase::unwrap(env, thisObj));
    if (!wrap) {
      napi_value result;
      napi_create_int32(env, UV_EBADF, &result);
      return result;
    }

    if (wrap->state() != kInitialized) {
      napi_value result;
      napi_create_int32(env, 0, &result);
      return result;
    }

    napi_value closeCallback = (argc > 0) ? argv[0] : nullptr;
    wrap->doClose(closeCallback);

    napi_value result;
    napi_create_int32(env, 0, &result);
    return result;
  }

  friend napi_value initTcpWrapBinding(napi_env env, napi_value exports);
};

// ---------------------------------------------------------------------------
// initTcpWrapBinding
// ---------------------------------------------------------------------------

napi_value initTcpWrapBinding(napi_env env, napi_value exports) {
  napi_handle_scope scope;
  napi_open_handle_scope(env, &scope);

  // --- TCP constructor ---
  napi_value tcpCtor;
  napi_define_class(
      env,
      "TCP",
      NAPI_AUTO_LENGTH,
      TCPWrap::New,
      nullptr,
      0,
      nullptr,
      &tcpCtor);

  // Get the prototype and add stream + TCP-specific methods.
  napi_value prototype;
  napi_get_named_property(env, tcpCtor, "prototype", &prototype);

  // Add all stream methods (readStart/readStop/write*/shutdown etc.)
  // plus handle methods (ref/unref/hasRef/close/getAsyncId).
  LibuvStreamBase::addStreamMethods(env, prototype);

  // Add TCP-specific methods.
  napi_property_descriptor tcpProps[] = {
      {"open",
       nullptr,
       TCPWrap::Open,
       nullptr,
       nullptr,
       nullptr,
       napi_default,
       nullptr},
      {"bind",
       nullptr,
       TCPWrap::Bind,
       nullptr,
       nullptr,
       nullptr,
       napi_default,
       nullptr},
      {"bind6",
       nullptr,
       TCPWrap::Bind6,
       nullptr,
       nullptr,
       nullptr,
       napi_default,
       nullptr},
      {"listen",
       nullptr,
       TCPWrap::Listen,
       nullptr,
       nullptr,
       nullptr,
       napi_default,
       nullptr},
      {"connect",
       nullptr,
       TCPWrap::Connect,
       nullptr,
       nullptr,
       nullptr,
       napi_default,
       nullptr},
      {"connect6",
       nullptr,
       TCPWrap::Connect6,
       nullptr,
       nullptr,
       nullptr,
       napi_default,
       nullptr},
      {"getsockname",
       nullptr,
       TCPWrap::GetSockName,
       nullptr,
       nullptr,
       nullptr,
       napi_default,
       nullptr},
      {"getpeername",
       nullptr,
       TCPWrap::GetPeerName,
       nullptr,
       nullptr,
       nullptr,
       napi_default,
       nullptr},
      {"setNoDelay",
       nullptr,
       TCPWrap::SetNoDelay,
       nullptr,
       nullptr,
       nullptr,
       napi_default,
       nullptr},
      {"setKeepAlive",
       nullptr,
       TCPWrap::SetKeepAlive,
       nullptr,
       nullptr,
       nullptr,
       napi_default,
       nullptr},
      {"reset",
       nullptr,
       TCPWrap::Reset,
       nullptr,
       nullptr,
       nullptr,
       napi_default,
       nullptr},
  };
  napi_define_properties(
      env, prototype, sizeof(tcpProps) / sizeof(tcpProps[0]), tcpProps);

  // --- Export TCP constructor ---
  napi_set_named_property(env, exports, "TCP", tcpCtor);

  // Store a reference to the constructor for use in OnConnection.
  napi_create_reference(env, tcpCtor, 1, &s_tcpCtorRef);

  // --- TCPConnectWrap constructor ---
  // A simple JS constructor for connect request objects.
  napi_value connectWrapCtor;
  napi_define_class(
      env,
      "TCPConnectWrap",
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
  napi_set_named_property(env, exports, "TCPConnectWrap", connectWrapCtor);

  // --- Constants ---
  napi_value constants;
  napi_create_object(env, &constants);

  napi_value socketVal, serverVal, ipv6onlyVal, reuseportVal;
  napi_create_int32(env, TCPWrap::SOCKET, &socketVal);
  napi_create_int32(env, TCPWrap::SERVER, &serverVal);
  napi_create_int32(env, UV_TCP_IPV6ONLY, &ipv6onlyVal);
#ifdef UV_TCP_REUSEPORT
  napi_create_int32(env, UV_TCP_REUSEPORT, &reuseportVal);
#else
  napi_create_int32(env, 0, &reuseportVal);
#endif

  napi_set_named_property(env, constants, "SOCKET", socketVal);
  napi_set_named_property(env, constants, "SERVER", serverVal);
  napi_set_named_property(env, constants, "UV_TCP_IPV6ONLY", ipv6onlyVal);
  napi_set_named_property(env, constants, "UV_TCP_REUSEPORT", reuseportVal);

  napi_set_named_property(env, exports, "constants", constants);

  napi_close_handle_scope(env, scope);
  return exports;
}

} // namespace node_compat
} // namespace hermes
