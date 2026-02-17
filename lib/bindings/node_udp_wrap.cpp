/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <hermes/node-compat/bindings/node_udp_wrap.h>
#include <hermes/node-compat/bindings/handle_wrap_base.h>
#include <node_api.h>
#include <uv.h>

#include <cassert>
#include <cstring>

namespace hermes {
namespace node_compat {

// ---------------------------------------------------------------------------
// SendReqData — native data for uv_udp_send_t requests
// ---------------------------------------------------------------------------

struct SendReqData {
  uv_udp_send_t req;
  napi_env env;
  napi_ref reqRef; // prevent-GC ref to JS request object
  size_t msgSize;
  bool haveCallback;
  // Inline storage for buffers. Extra bufs allocated after struct if needed.
  uv_buf_t bufs[1];

  SendReqData() : env(nullptr), reqRef(nullptr), msgSize(0), haveCallback(false) {
    memset(&req, 0, sizeof(req));
  }
};

// ---------------------------------------------------------------------------
// UDPWrap — wraps uv_udp_t, inherits from HandleWrapBase
// ---------------------------------------------------------------------------

class UDPWrap : public HandleWrapBase {
 public:
  /// Construct a UDPWrap.
  UDPWrap(napi_env env, napi_value jsObj) : handle_() {
    int err = uv_udp_init(getHandleWrapEventLoop(), &handle_);
    if (err != 0) {
      return;
    }
    init(env, jsObj, reinterpret_cast<uv_handle_t *>(&handle_));
  }

 private:
  uv_udp_t handle_;

  // --- NAPI callbacks ---

  /// new UDP()
  static napi_value New(napi_env env, napi_callback_info info) {
    napi_handle_scope scope;
    napi_open_handle_scope(env, &scope);

    napi_value thisObj;
    napi_get_cb_info(env, info, nullptr, nullptr, &thisObj, nullptr);

    new UDPWrap(env, thisObj);

    napi_close_handle_scope(env, scope);
    return thisObj;
  }

  /// open(fd) -> err
  static napi_value Open(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1];
    napi_value thisObj;
    napi_get_cb_info(env, info, &argc, argv, &thisObj, nullptr);

    auto *wrap = static_cast<UDPWrap *>(HandleWrapBase::unwrap(env, thisObj));
    if (!wrap) {
      napi_value result;
      napi_create_int32(env, UV_EBADF, &result);
      return result;
    }

    int64_t fd = 0;
    napi_get_value_int64(env, argv[0], &fd);

    int err = uv_udp_open(&wrap->handle_, static_cast<uv_os_sock_t>(fd));

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

    auto *wrap = static_cast<UDPWrap *>(HandleWrapBase::unwrap(env, thisObj));
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

    sockaddr_in addr;
    int err = uv_ip4_addr(ipBuf, port, &addr);
    if (err == 0) {
      err = uv_udp_bind(
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

    auto *wrap = static_cast<UDPWrap *>(HandleWrapBase::unwrap(env, thisObj));
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
      err = uv_udp_bind(
          &wrap->handle_, reinterpret_cast<const sockaddr *>(&addr), flags);
    }

    napi_value result;
    napi_create_int32(env, err, &result);
    return result;
  }

  /// connect(addr, port) -> err
  static napi_value Connect(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2];
    napi_value thisObj;
    napi_get_cb_info(env, info, &argc, argv, &thisObj, nullptr);

    auto *wrap = static_cast<UDPWrap *>(HandleWrapBase::unwrap(env, thisObj));
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

    sockaddr_in addr;
    int err = uv_ip4_addr(ipBuf, port, &addr);
    if (err == 0) {
      err = uv_udp_connect(
          &wrap->handle_, reinterpret_cast<const sockaddr *>(&addr));
    }

    napi_value result;
    napi_create_int32(env, err, &result);
    return result;
  }

  /// connect6(addr, port) -> err
  static napi_value Connect6(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2];
    napi_value thisObj;
    napi_get_cb_info(env, info, &argc, argv, &thisObj, nullptr);

    auto *wrap = static_cast<UDPWrap *>(HandleWrapBase::unwrap(env, thisObj));
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

    sockaddr_in6 addr;
    int err = uv_ip6_addr(ipBuf, port, &addr);
    if (err == 0) {
      err = uv_udp_connect(
          &wrap->handle_, reinterpret_cast<const sockaddr *>(&addr));
    }

    napi_value result;
    napi_create_int32(env, err, &result);
    return result;
  }

  /// disconnect() -> err
  static napi_value Disconnect(napi_env env, napi_callback_info info) {
    napi_value thisObj;
    napi_get_cb_info(env, info, nullptr, nullptr, &thisObj, nullptr);

    auto *wrap = static_cast<UDPWrap *>(HandleWrapBase::unwrap(env, thisObj));
    if (!wrap) {
      napi_value result;
      napi_create_int32(env, UV_EBADF, &result);
      return result;
    }

    int err = uv_udp_connect(&wrap->handle_, nullptr);

    napi_value result;
    napi_create_int32(env, err, &result);
    return result;
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

    auto *wrap = static_cast<UDPWrap *>(HandleWrapBase::unwrap(env, thisObj));
    if (!wrap) {
      napi_value result;
      napi_create_int32(env, UV_EBADF, &result);
      return result;
    }

    sockaddr_storage storage;
    int addrLen = sizeof(storage);
    int err = uv_udp_getsockname(
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

    auto *wrap = static_cast<UDPWrap *>(HandleWrapBase::unwrap(env, thisObj));
    if (!wrap) {
      napi_value result;
      napi_create_int32(env, UV_EBADF, &result);
      return result;
    }

    sockaddr_storage storage;
    int addrLen = sizeof(storage);
    int err = uv_udp_getpeername(
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

  // --- Send ---

  /// Internal DoSend implementation.
  /// Connected mode: send(req, list, count, hasCallback) — 4 args
  /// Sendto mode: send(req, list, count, port, address, hasCallback) — 6 args
  static napi_value DoSend(napi_env env, napi_callback_info info, int family) {
    size_t argc = 6;
    napi_value argv[6];
    napi_value thisObj;
    napi_get_cb_info(env, info, &argc, argv, &thisObj, nullptr);

    auto *wrap = static_cast<UDPWrap *>(HandleWrapBase::unwrap(env, thisObj));
    if (!wrap) {
      napi_value result;
      napi_create_int32(env, UV_EBADF, &result);
      return result;
    }

    napi_value reqObj = argv[0];
    napi_value listVal = argv[1];

    uint32_t count = 0;
    napi_get_value_uint32(env, argv[2], &count);

    // Determine whether this is a sendto or connected send.
    bool haveDest = (argc >= 6);
    sockaddr_storage storage;
    const sockaddr *destAddr = nullptr;

    if (haveDest) {
      uint32_t port = 0;
      napi_get_value_uint32(env, argv[3], &port);

      char ipBuf[256];
      size_t ipLen = 0;
      napi_get_value_string_utf8(env, argv[4], ipBuf, sizeof(ipBuf), &ipLen);

      int err;
      if (family == AF_INET) {
        sockaddr_in *addr4 = reinterpret_cast<sockaddr_in *>(&storage);
        err = uv_ip4_addr(ipBuf, static_cast<int>(port), addr4);
      } else {
        sockaddr_in6 *addr6 = reinterpret_cast<sockaddr_in6 *>(&storage);
        err = uv_ip6_addr(ipBuf, static_cast<int>(port), addr6);
      }
      if (err != 0) {
        napi_value result;
        napi_create_int32(env, err, &result);
        return result;
      }
      destAddr = reinterpret_cast<const sockaddr *>(&storage);
    }

    bool haveCallback = false;
    {
      napi_value cbArg = haveDest ? argv[5] : argv[3];
      napi_get_value_bool(env, cbArg, &haveCallback);
    }

    // Build uv_buf_t array from JS list.
    // Allocate SendReqData with space for 'count' bufs.
    size_t allocSize =
        sizeof(SendReqData) + (count > 1 ? (count - 1) * sizeof(uv_buf_t) : 0);
    auto *reqData = static_cast<SendReqData *>(malloc(allocSize));
    memset(reqData, 0, sizeof(SendReqData));
    reqData->env = env;
    reqData->haveCallback = haveCallback;

    size_t totalSize = 0;
    for (uint32_t i = 0; i < count; ++i) {
      napi_value item;
      napi_get_element(env, listVal, i, &item);

      void *bufData = nullptr;
      size_t bufLen = 0;

      // Try ArrayBuffer view first, then Buffer.
      napi_typedarray_type arrType;
      napi_value arrBuf;
      size_t byteOffset = 0;
      napi_status st = napi_get_typedarray_info(
          env, item, &arrType, &bufLen, &bufData, &arrBuf, &byteOffset);
      if (st != napi_ok) {
        // Try as buffer.
        st = napi_get_buffer_info(env, item, &bufData, &bufLen);
      }
      if (st != napi_ok) {
        bufData = nullptr;
        bufLen = 0;
      }

      reqData->bufs[i] = uv_buf_init(static_cast<char *>(bufData), bufLen);
      totalSize += bufLen;
    }
    reqData->msgSize = totalSize;

    // Try synchronous send first.
    int err;
    if (destAddr != nullptr) {
      err = uv_udp_try_send(
          &wrap->handle_, reqData->bufs, count, destAddr);
    } else {
      err = uv_udp_try_send(
          &wrap->handle_, reqData->bufs, count, nullptr);
    }

    if (err == static_cast<int>(totalSize)) {
      // Synchronous send succeeded. Return msgSize + 1 to distinguish
      // from async (Node convention).
      free(reqData);
      napi_value result;
      napi_create_int32(env, static_cast<int>(totalSize) + 1, &result);
      return result;
    }

    // Fall through to async send if try_send returned EAGAIN or ENOSYS
    // or any other error we want to retry asynchronously.
    if (err != UV_EAGAIN && err != UV_ENOSYS) {
      // For errors other than EAGAIN/ENOSYS, return the error.
      if (err < 0) {
        free(reqData);
        napi_value result;
        napi_create_int32(env, err, &result);
        return result;
      }
    }

    // Async send.
    reqData->req.data = reqData;
    napi_create_reference(env, reqObj, 1, &reqData->reqRef);

    err = uv_udp_send(
        &reqData->req,
        &wrap->handle_,
        reqData->bufs,
        count,
        destAddr,
        AfterSend);

    if (err != 0) {
      napi_delete_reference(env, reqData->reqRef);
      free(reqData);
      napi_value result;
      napi_create_int32(env, err, &result);
      return result;
    }

    // Return 0 = async (queued).
    napi_value result;
    napi_create_int32(env, 0, &result);
    return result;
  }

  /// send(req, list, count, [port, address,] hasCallback)
  static napi_value Send(napi_env env, napi_callback_info info) {
    return DoSend(env, info, AF_INET);
  }

  /// send6(req, list, count, [port, address,] hasCallback)
  static napi_value Send6(napi_env env, napi_callback_info info) {
    return DoSend(env, info, AF_INET6);
  }

  /// libuv send callback.
  static void AfterSend(uv_udp_send_t *req, int status) {
    auto *reqData = static_cast<SendReqData *>(req->data);
    if (!reqData)
      return;

    napi_env env = reqData->env;

    napi_handle_scope scope;
    napi_open_handle_scope(env, &scope);

    if (reqData->haveCallback) {
      napi_value reqObj;
      napi_get_reference_value(env, reqData->reqRef, &reqObj);

      napi_value oncomplete;
      napi_get_named_property(env, reqObj, "oncomplete", &oncomplete);

      napi_valuetype oncompType;
      napi_typeof(env, oncomplete, &oncompType);
      if (oncompType == napi_function) {
        napi_value args[2];
        napi_create_int32(env, status, &args[0]);
        napi_create_int32(env, static_cast<int>(reqData->msgSize), &args[1]);
        napi_value retval;
        napi_call_function(env, reqObj, oncomplete, 2, args, &retval);

        bool hasPending = false;
        napi_is_exception_pending(env, &hasPending);
        if (hasPending) {
          napi_value exc;
          napi_get_and_clear_last_exception(env, &exc);
        }
      }
    }

    napi_delete_reference(env, reqData->reqRef);
    free(reqData);

    napi_close_handle_scope(env, scope);
  }

  // --- Recv ---

  /// recvStart() -> err
  static napi_value RecvStart(napi_env env, napi_callback_info info) {
    napi_value thisObj;
    napi_get_cb_info(env, info, nullptr, nullptr, &thisObj, nullptr);

    auto *wrap = static_cast<UDPWrap *>(HandleWrapBase::unwrap(env, thisObj));
    if (!wrap) {
      napi_value result;
      napi_create_int32(env, UV_EBADF, &result);
      return result;
    }

    int err = uv_udp_recv_start(&wrap->handle_, AllocCb, RecvCb);

    napi_value result;
    napi_create_int32(env, err, &result);
    return result;
  }

  /// recvStop() -> err
  static napi_value RecvStop(napi_env env, napi_callback_info info) {
    napi_value thisObj;
    napi_get_cb_info(env, info, nullptr, nullptr, &thisObj, nullptr);

    auto *wrap = static_cast<UDPWrap *>(HandleWrapBase::unwrap(env, thisObj));
    if (!wrap) {
      napi_value result;
      napi_create_int32(env, UV_EBADF, &result);
      return result;
    }

    int err = uv_udp_recv_stop(&wrap->handle_);

    napi_value result;
    napi_create_int32(env, err, &result);
    return result;
  }

  /// libuv alloc callback.
  static void AllocCb(
      uv_handle_t * /*handle*/,
      size_t suggested_size,
      uv_buf_t *buf) {
    buf->base = static_cast<char *>(malloc(suggested_size));
    buf->len = buf->base ? suggested_size : 0;
  }

  /// libuv recv callback.
  static void RecvCb(
      uv_udp_t *handle,
      ssize_t nread,
      const uv_buf_t *buf,
      const sockaddr *addr,
      unsigned /*flags*/) {
    auto *wrap = static_cast<UDPWrap *>(handle->data);
    if (!wrap || !wrap->env()) {
      free(buf->base);
      return;
    }

    napi_env env = wrap->env();
    napi_handle_scope scope;
    napi_open_handle_scope(env, &scope);

    napi_value handleObj = wrap->getJsObject();
    if (!handleObj) {
      free(buf->base);
      napi_close_handle_scope(env, scope);
      return;
    }

    // nread == 0 && addr == nullptr means nothing to report.
    if (nread == 0 && addr == nullptr) {
      free(buf->base);
      napi_close_handle_scope(env, scope);
      return;
    }

    // Build JS arguments for onmessage(nread, handle, buffer, rinfo).
    napi_value args[4];

    // args[0] = nread
    napi_create_int32(env, static_cast<int>(nread), &args[0]);

    // args[1] = handle
    args[1] = handleObj;

    if (nread > 0) {
      // args[2] = Buffer with received data.
      // napi_create_buffer_copy returns a plain Uint8Array in Hermes NAPI,
      // so wrap it with Buffer.from() to get a proper Node.js Buffer.
      void *bufCopy = nullptr;
      napi_value rawBuf;
      napi_create_buffer_copy(
          env,
          static_cast<size_t>(nread),
          buf->base,
          &bufCopy,
          &rawBuf);
      napi_value global, bufferCtor, bufferFrom;
      napi_get_global(env, &global);
      napi_get_named_property(env, global, "Buffer", &bufferCtor);
      napi_get_named_property(env, bufferCtor, "from", &bufferFrom);
      napi_call_function(env, bufferCtor, bufferFrom, 1, &rawBuf, &args[2]);
    } else {
      napi_get_undefined(env, &args[2]);
    }

    free(buf->base);

    // args[3] = rinfo {address, port, family} or undefined
    if (addr != nullptr && nread > 0) {
      napi_create_object(env, &args[3]);
      AddressToJS(env, addr, args[3]);
      // Also add size field for compatibility.
      napi_value sizeVal;
      napi_create_int32(env, static_cast<int>(nread), &sizeVal);
      napi_set_named_property(env, args[3], "size", sizeVal);
    } else {
      napi_get_undefined(env, &args[3]);
    }

    napi_value onmessage;
    napi_get_named_property(env, handleObj, "onmessage", &onmessage);

    napi_valuetype onmsgType;
    napi_typeof(env, onmessage, &onmsgType);
    if (onmsgType == napi_function) {
      napi_value retval;
      napi_call_function(env, handleObj, onmessage, 4, args, &retval);

      bool hasPending = false;
      napi_is_exception_pending(env, &hasPending);
      if (hasPending) {
        napi_value exc;
        napi_get_and_clear_last_exception(env, &exc);
      }
    }

    napi_close_handle_scope(env, scope);
  }

  // --- Multicast / TTL / Broadcast ---

  /// setMulticastTTL(ttl) -> err
  static napi_value SetMulticastTTL(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1];
    napi_value thisObj;
    napi_get_cb_info(env, info, &argc, argv, &thisObj, nullptr);

    auto *wrap = static_cast<UDPWrap *>(HandleWrapBase::unwrap(env, thisObj));
    if (!wrap) {
      napi_value result;
      napi_create_int32(env, UV_EBADF, &result);
      return result;
    }

    int32_t ttl = 0;
    napi_get_value_int32(env, argv[0], &ttl);

    int err = uv_udp_set_multicast_ttl(&wrap->handle_, ttl);

    napi_value result;
    napi_create_int32(env, err, &result);
    return result;
  }

  /// setMulticastLoopback(flag) -> err
  static napi_value SetMulticastLoopback(
      napi_env env,
      napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1];
    napi_value thisObj;
    napi_get_cb_info(env, info, &argc, argv, &thisObj, nullptr);

    auto *wrap = static_cast<UDPWrap *>(HandleWrapBase::unwrap(env, thisObj));
    if (!wrap) {
      napi_value result;
      napi_create_int32(env, UV_EBADF, &result);
      return result;
    }

    int32_t flag = 0;
    napi_get_value_int32(env, argv[0], &flag);

    int err = uv_udp_set_multicast_loop(&wrap->handle_, flag);

    napi_value result;
    napi_create_int32(env, err, &result);
    return result;
  }

  /// setBroadcast(flag) -> err
  static napi_value SetBroadcast(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1];
    napi_value thisObj;
    napi_get_cb_info(env, info, &argc, argv, &thisObj, nullptr);

    auto *wrap = static_cast<UDPWrap *>(HandleWrapBase::unwrap(env, thisObj));
    if (!wrap) {
      napi_value result;
      napi_create_int32(env, UV_EBADF, &result);
      return result;
    }

    int32_t flag = 0;
    napi_get_value_int32(env, argv[0], &flag);

    int err = uv_udp_set_broadcast(&wrap->handle_, flag);

    napi_value result;
    napi_create_int32(env, err, &result);
    return result;
  }

  /// setTTL(ttl) -> err
  static napi_value SetTTL(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1];
    napi_value thisObj;
    napi_get_cb_info(env, info, &argc, argv, &thisObj, nullptr);

    auto *wrap = static_cast<UDPWrap *>(HandleWrapBase::unwrap(env, thisObj));
    if (!wrap) {
      napi_value result;
      napi_create_int32(env, UV_EBADF, &result);
      return result;
    }

    int32_t ttl = 0;
    napi_get_value_int32(env, argv[0], &ttl);

    int err = uv_udp_set_ttl(&wrap->handle_, ttl);

    napi_value result;
    napi_create_int32(env, err, &result);
    return result;
  }

  /// setMulticastInterface(addr) -> err
  static napi_value SetMulticastInterface(
      napi_env env,
      napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1];
    napi_value thisObj;
    napi_get_cb_info(env, info, &argc, argv, &thisObj, nullptr);

    auto *wrap = static_cast<UDPWrap *>(HandleWrapBase::unwrap(env, thisObj));
    if (!wrap) {
      napi_value result;
      napi_create_int32(env, UV_EBADF, &result);
      return result;
    }

    char ifaceBuf[256];
    size_t ifaceLen = 0;
    napi_get_value_string_utf8(
        env, argv[0], ifaceBuf, sizeof(ifaceBuf), &ifaceLen);

    int err = uv_udp_set_multicast_interface(&wrap->handle_, ifaceBuf);

    napi_value result;
    napi_create_int32(env, err, &result);
    return result;
  }

  /// addMembership(group, iface) -> err
  static napi_value AddMembership(napi_env env, napi_callback_info info) {
    return SetMembership(env, info, UV_JOIN_GROUP);
  }

  /// dropMembership(group, iface) -> err
  static napi_value DropMembership(napi_env env, napi_callback_info info) {
    return SetMembership(env, info, UV_LEAVE_GROUP);
  }

  static napi_value SetMembership(
      napi_env env,
      napi_callback_info info,
      uv_membership membership) {
    size_t argc = 2;
    napi_value argv[2];
    napi_value thisObj;
    napi_get_cb_info(env, info, &argc, argv, &thisObj, nullptr);

    auto *wrap = static_cast<UDPWrap *>(HandleWrapBase::unwrap(env, thisObj));
    if (!wrap) {
      napi_value result;
      napi_create_int32(env, UV_EBADF, &result);
      return result;
    }

    char groupBuf[256];
    size_t groupLen = 0;
    napi_get_value_string_utf8(
        env, argv[0], groupBuf, sizeof(groupBuf), &groupLen);

    // iface can be undefined/null (any interface).
    char ifaceBuf[256];
    const char *iface = nullptr;
    if (argc >= 2) {
      napi_valuetype ifaceType;
      napi_typeof(env, argv[1], &ifaceType);
      if (ifaceType == napi_string) {
        size_t ifaceLen = 0;
        napi_get_value_string_utf8(
            env, argv[1], ifaceBuf, sizeof(ifaceBuf), &ifaceLen);
        iface = ifaceBuf;
      }
    }

    int err =
        uv_udp_set_membership(&wrap->handle_, groupBuf, iface, membership);

    napi_value result;
    napi_create_int32(env, err, &result);
    return result;
  }

  /// addSourceSpecificMembership(source, group, iface) -> err
  static napi_value AddSourceSpecificMembership(
      napi_env env,
      napi_callback_info info) {
    return SetSourceMembership(env, info, UV_JOIN_GROUP);
  }

  /// dropSourceSpecificMembership(source, group, iface) -> err
  static napi_value DropSourceSpecificMembership(
      napi_env env,
      napi_callback_info info) {
    return SetSourceMembership(env, info, UV_LEAVE_GROUP);
  }

  static napi_value SetSourceMembership(
      napi_env env,
      napi_callback_info info,
      uv_membership membership) {
    size_t argc = 3;
    napi_value argv[3];
    napi_value thisObj;
    napi_get_cb_info(env, info, &argc, argv, &thisObj, nullptr);

    auto *wrap = static_cast<UDPWrap *>(HandleWrapBase::unwrap(env, thisObj));
    if (!wrap) {
      napi_value result;
      napi_create_int32(env, UV_EBADF, &result);
      return result;
    }

    char sourceBuf[256];
    size_t sourceLen = 0;
    napi_get_value_string_utf8(
        env, argv[0], sourceBuf, sizeof(sourceBuf), &sourceLen);

    char groupBuf[256];
    size_t groupLen = 0;
    napi_get_value_string_utf8(
        env, argv[1], groupBuf, sizeof(groupBuf), &groupLen);

    // iface can be undefined/null.
    char ifaceBuf[256];
    const char *iface = nullptr;
    if (argc >= 3) {
      napi_valuetype ifaceType;
      napi_typeof(env, argv[2], &ifaceType);
      if (ifaceType == napi_string) {
        size_t ifaceLen = 0;
        napi_get_value_string_utf8(
            env, argv[2], ifaceBuf, sizeof(ifaceBuf), &ifaceLen);
        iface = ifaceBuf;
      }
    }

    int err = uv_udp_set_source_membership(
        &wrap->handle_, groupBuf, iface, sourceBuf, membership);

    napi_value result;
    napi_create_int32(env, err, &result);
    return result;
  }

  // --- Buffer size ---

  /// bufferSize(size, isRecv, ctx) -> number or undefined
  static napi_value BufferSize(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value argv[3];
    napi_value thisObj;
    napi_get_cb_info(env, info, &argc, argv, &thisObj, nullptr);

    auto *wrap = static_cast<UDPWrap *>(HandleWrapBase::unwrap(env, thisObj));
    if (!wrap) {
      napi_value undef;
      napi_get_undefined(env, &undef);
      return undef;
    }

    int32_t size = 0;
    napi_get_value_int32(env, argv[0], &size);

    bool isRecv = false;
    napi_get_value_bool(env, argv[1], &isRecv);

    napi_value ctx = argv[2];

    int value = size;
    int err;
    if (isRecv) {
      err = uv_recv_buffer_size(
          reinterpret_cast<uv_handle_t *>(&wrap->handle_), &value);
    } else {
      err = uv_send_buffer_size(
          reinterpret_cast<uv_handle_t *>(&wrap->handle_), &value);
    }

    if (err != 0) {
      // Set error on ctx object.
      const char *syscall = isRecv ? "uv_recv_buffer_size" : "uv_send_buffer_size";
      napi_value errnoVal, msgVal, syscallVal, codeVal;
      napi_create_int32(env, err, &errnoVal);
      napi_create_string_utf8(
          env, uv_strerror(err), NAPI_AUTO_LENGTH, &msgVal);
      napi_create_string_utf8(env, syscall, NAPI_AUTO_LENGTH, &syscallVal);
      napi_create_string_utf8(
          env, uv_err_name(err), NAPI_AUTO_LENGTH, &codeVal);
      napi_set_named_property(env, ctx, "errno", errnoVal);
      napi_set_named_property(env, ctx, "message", msgVal);
      napi_set_named_property(env, ctx, "syscall", syscallVal);
      napi_set_named_property(env, ctx, "code", codeVal);

      napi_value undef;
      napi_get_undefined(env, &undef);
      return undef;
    }

    napi_value result;
    napi_create_int32(env, value, &result);
    return result;
  }

  // --- Send queue info ---

  /// getSendQueueSize() -> number
  static napi_value GetSendQueueSize(napi_env env, napi_callback_info info) {
    napi_value thisObj;
    napi_get_cb_info(env, info, nullptr, nullptr, &thisObj, nullptr);

    auto *wrap = static_cast<UDPWrap *>(HandleWrapBase::unwrap(env, thisObj));
    if (!wrap) {
      napi_value result;
      napi_create_double(env, 0, &result);
      return result;
    }

    size_t size = uv_udp_get_send_queue_size(&wrap->handle_);

    napi_value result;
    napi_create_double(env, static_cast<double>(size), &result);
    return result;
  }

  /// getSendQueueCount() -> number
  static napi_value GetSendQueueCount(napi_env env, napi_callback_info info) {
    napi_value thisObj;
    napi_get_cb_info(env, info, nullptr, nullptr, &thisObj, nullptr);

    auto *wrap = static_cast<UDPWrap *>(HandleWrapBase::unwrap(env, thisObj));
    if (!wrap) {
      napi_value result;
      napi_create_double(env, 0, &result);
      return result;
    }

    size_t count = uv_udp_get_send_queue_count(&wrap->handle_);

    napi_value result;
    napi_create_double(env, static_cast<double>(count), &result);
    return result;
  }

  friend napi_value initUdpWrapBinding(napi_env env, napi_value exports);
};

// ---------------------------------------------------------------------------
// initUdpWrapBinding
// ---------------------------------------------------------------------------

napi_value initUdpWrapBinding(napi_env env, napi_value exports) {
  napi_handle_scope scope;
  napi_open_handle_scope(env, &scope);

  // --- UDP constructor ---
  napi_value udpCtor;
  napi_define_class(
      env,
      "UDP",
      NAPI_AUTO_LENGTH,
      UDPWrap::New,
      nullptr,
      0,
      nullptr,
      &udpCtor);

  // Get the prototype and add HandleWrap + UDP-specific methods.
  napi_value prototype;
  napi_get_named_property(env, udpCtor, "prototype", &prototype);

  // Add handle methods (ref/unref/hasRef/close/getAsyncId).
  HandleWrapBase::addHandleWrapMethods(env, prototype);

  // Add UDP-specific methods.
  napi_property_descriptor udpProps[] = {
      {"open", nullptr, UDPWrap::Open, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"bind", nullptr, UDPWrap::Bind, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"bind6", nullptr, UDPWrap::Bind6, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"connect", nullptr, UDPWrap::Connect, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"connect6", nullptr, UDPWrap::Connect6, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"disconnect", nullptr, UDPWrap::Disconnect, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"send", nullptr, UDPWrap::Send, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"send6", nullptr, UDPWrap::Send6, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"recvStart", nullptr, UDPWrap::RecvStart, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"recvStop", nullptr, UDPWrap::RecvStop, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"getsockname", nullptr, UDPWrap::GetSockName, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"getpeername", nullptr, UDPWrap::GetPeerName, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"addMembership", nullptr, UDPWrap::AddMembership, nullptr, nullptr,
       nullptr, napi_default, nullptr},
      {"dropMembership", nullptr, UDPWrap::DropMembership, nullptr, nullptr,
       nullptr, napi_default, nullptr},
      {"addSourceSpecificMembership", nullptr,
       UDPWrap::AddSourceSpecificMembership, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"dropSourceSpecificMembership", nullptr,
       UDPWrap::DropSourceSpecificMembership, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"setMulticastInterface", nullptr, UDPWrap::SetMulticastInterface,
       nullptr, nullptr, nullptr, napi_default, nullptr},
      {"setMulticastTTL", nullptr, UDPWrap::SetMulticastTTL, nullptr, nullptr,
       nullptr, napi_default, nullptr},
      {"setMulticastLoopback", nullptr, UDPWrap::SetMulticastLoopback, nullptr,
       nullptr, nullptr, napi_default, nullptr},
      {"setBroadcast", nullptr, UDPWrap::SetBroadcast, nullptr, nullptr,
       nullptr, napi_default, nullptr},
      {"setTTL", nullptr, UDPWrap::SetTTL, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"bufferSize", nullptr, UDPWrap::BufferSize, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"getSendQueueSize", nullptr, UDPWrap::GetSendQueueSize, nullptr,
       nullptr, nullptr, napi_default, nullptr},
      {"getSendQueueCount", nullptr, UDPWrap::GetSendQueueCount, nullptr,
       nullptr, nullptr, napi_default, nullptr},
  };
  napi_define_properties(
      env, prototype, sizeof(udpProps) / sizeof(udpProps[0]), udpProps);

  // --- Export UDP constructor ---
  napi_set_named_property(env, exports, "UDP", udpCtor);

  // --- SendWrap constructor ---
  napi_value sendWrapCtor;
  napi_define_class(
      env,
      "SendWrap",
      NAPI_AUTO_LENGTH,
      [](napi_env env, napi_callback_info info) -> napi_value {
        napi_value thisObj;
        napi_get_cb_info(env, info, nullptr, nullptr, &thisObj, nullptr);
        return thisObj;
      },
      nullptr,
      0,
      nullptr,
      &sendWrapCtor);
  napi_set_named_property(env, exports, "SendWrap", sendWrapCtor);

  // --- Constants ---
  napi_value constants;
  napi_create_object(env, &constants);

  napi_value ipv6onlyVal, reuseaddrVal, reuseportVal;
  napi_create_int32(env, UV_UDP_IPV6ONLY, &ipv6onlyVal);
  napi_create_int32(env, UV_UDP_REUSEADDR, &reuseaddrVal);
#ifdef UV_UDP_REUSEPORT
  napi_create_int32(env, UV_UDP_REUSEPORT, &reuseportVal);
#else
  napi_create_int32(env, 0, &reuseportVal);
#endif

  napi_set_named_property(env, constants, "UV_UDP_IPV6ONLY", ipv6onlyVal);
  napi_set_named_property(env, constants, "UV_UDP_REUSEADDR", reuseaddrVal);
  napi_set_named_property(env, constants, "UV_UDP_REUSEPORT", reuseportVal);

  napi_set_named_property(env, exports, "constants", constants);

  napi_close_handle_scope(env, scope);
  return exports;
}

} // namespace node_compat
} // namespace hermes
