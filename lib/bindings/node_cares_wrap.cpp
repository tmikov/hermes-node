/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// c-ares 1.34+ deprecates the old ares_parse_*_reply and ares_query APIs
// in favor of ares_dns_parse/ares_query_dnsrec. Suppress until we migrate.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"

#include <hermes/node-compat/bindings/node_cares_wrap.h>

#include <node_api.h>
#include <uv.h>

#include <ada.h>

#include <cassert>
#include <cstring>
#include <string>

namespace hermes {
namespace node_compat {

// ---------------------------------------------------------------------------
// Module-level state
// ---------------------------------------------------------------------------

static uv_loop_t *s_caresLoop = nullptr;

void setCaresWrapEventLoop(uv_loop_t *loop) {
  s_caresLoop = loop;
}

// DNS order constants (matching Node's cares_wrap.h).
static constexpr uint32_t DNS_ORDER_VERBATIM = 0;
static constexpr uint32_t DNS_ORDER_IPV4_FIRST = 1;
static constexpr uint32_t DNS_ORDER_IPV6_FIRST = 2;

// ---------------------------------------------------------------------------
// GetAddrInfoReqWrap — async request for uv_getaddrinfo
// ---------------------------------------------------------------------------

struct GetAddrInfoReq {
  uv_getaddrinfo_t req{};
  napi_env env = nullptr;
  napi_ref reqObjRef = nullptr; // prevent-GC ref to JS request object
  uint32_t order = DNS_ORDER_VERBATIM;
};

/// libuv callback after uv_getaddrinfo completes.
static void
afterGetAddrInfo(uv_getaddrinfo_t *req, int status, struct addrinfo *res) {
  auto *wrap = static_cast<GetAddrInfoReq *>(req->data);
  napi_env env = wrap->env;

  napi_handle_scope scope;
  napi_open_handle_scope(env, &scope);

  // Get the JS request object.
  napi_value reqObj;
  napi_get_reference_value(env, wrap->reqObjRef, &reqObj);

  // Build result: oncomplete(status, addresses)
  // argv[0] = status (int32), argv[1] = addresses array or null
  napi_value argv[2];
  napi_create_int32(env, status, &argv[0]);
  napi_get_null(env, &argv[1]);

  uint32_t n = 0;
  const uint32_t order = wrap->order;

  if (status == 0) {
    napi_value results;
    napi_create_array(env, &results);

    // Lambda to iterate addrinfo and add addresses for selected families.
    auto addAddresses = [&](bool wantV4, bool wantV6) {
      for (struct addrinfo *p = res; p != nullptr; p = p->ai_next) {
        const char *addr = nullptr;
        int family = p->ai_family;

        if (wantV4 && family == AF_INET) {
          addr = reinterpret_cast<const char *>(
              &reinterpret_cast<struct sockaddr_in *>(p->ai_addr)->sin_addr);
        } else if (wantV6 && family == AF_INET6) {
          addr = reinterpret_cast<const char *>(
              &reinterpret_cast<struct sockaddr_in6 *>(p->ai_addr)->sin6_addr);
        } else {
          continue;
        }

        char ip[INET6_ADDRSTRLEN];
        if (uv_inet_ntop(family, addr, ip, sizeof(ip)) != 0)
          continue;

        napi_value ipStr;
        napi_create_string_utf8(env, ip, NAPI_AUTO_LENGTH, &ipStr);
        napi_set_element(env, results, n, ipStr);
        n++;
      }
    };

    switch (order) {
      case DNS_ORDER_IPV4_FIRST:
        addAddresses(true, false);
        addAddresses(false, true);
        break;
      case DNS_ORDER_IPV6_FIRST:
        addAddresses(false, true);
        addAddresses(true, false);
        break;
      default: // DNS_ORDER_VERBATIM
        addAddresses(true, true);
        break;
    }

    if (n == 0) {
      // No results found — report as UV_EAI_NODATA.
      napi_create_int32(env, UV_EAI_NODATA, &argv[0]);
    }

    argv[1] = results;
  }

  // Free the addrinfo linked list.
  uv_freeaddrinfo(res);

  // Call reqObj.oncomplete(status, addresses).
  napi_value oncomplete;
  napi_get_named_property(env, reqObj, "oncomplete", &oncomplete);

  napi_valuetype oncompType;
  napi_typeof(env, oncomplete, &oncompType);
  if (oncompType == napi_function) {
    napi_value cbResult;
    napi_call_function(env, reqObj, oncomplete, 2, argv, &cbResult);
  }

  // Cleanup: release the prevent-GC ref and delete the wrap.
  napi_delete_reference(env, wrap->reqObjRef);
  delete wrap;

  napi_close_handle_scope(env, scope);
}

/// getaddrinfo(req, hostname, family, hints, order) -> err_code
static napi_value getaddrinfoFn(napi_env env, napi_callback_info info) {
  size_t argc = 5;
  napi_value argv[5];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  // argv[0] = req object
  // argv[1] = hostname (string)
  // argv[2] = family (int32: 0, 4, or 6)
  // argv[3] = hints/flags (int32)
  // argv[4] = order (uint32: 0, 1, or 2)

  // Extract hostname.
  size_t hostnameLen = 0;
  napi_get_value_string_utf8(env, argv[1], nullptr, 0, &hostnameLen);
  std::string hostname(hostnameLen, '\0');
  napi_get_value_string_utf8(
      env, argv[1], &hostname[0], hostnameLen + 1, &hostnameLen);

  // Convert to ASCII via Ada IDNA (internationalized domain names).
  std::string asciiHostname = ada::idna::to_ascii(hostname);

  // Extract family.
  int32_t familyArg = 0;
  napi_get_value_int32(env, argv[2], &familyArg);

  int family;
  switch (familyArg) {
    case 4:
      family = AF_INET;
      break;
    case 6:
      family = AF_INET6;
      break;
    default:
      family = AF_UNSPEC;
      break;
  }

  // Extract hints/flags.
  int32_t flags = 0;
  napi_valuetype flagsType;
  napi_typeof(env, argv[3], &flagsType);
  if (flagsType == napi_number)
    napi_get_value_int32(env, argv[3], &flags);

  // Extract order.
  uint32_t order = DNS_ORDER_VERBATIM;
  napi_get_value_uint32(env, argv[4], &order);

  // Create the native request.
  auto *wrap = new GetAddrInfoReq();
  wrap->env = env;
  wrap->req.data = wrap;
  wrap->order = order;

  // Create a prevent-GC reference to the JS request object.
  napi_create_reference(env, argv[0], 1, &wrap->reqObjRef);

  // Set up addrinfo hints.
  struct addrinfo hints {};
  hints.ai_family = family;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = flags;

  int err = uv_getaddrinfo(
      s_caresLoop,
      &wrap->req,
      afterGetAddrInfo,
      asciiHostname.c_str(),
      nullptr,
      &hints);

  if (err != 0) {
    // Immediate error — cleanup and return error code.
    napi_delete_reference(env, wrap->reqObjRef);
    delete wrap;
  }

  napi_value result;
  napi_create_int32(env, err, &result);
  return result;
}

// ---------------------------------------------------------------------------
// GetNameInfoReqWrap — async request for uv_getnameinfo
// ---------------------------------------------------------------------------

struct GetNameInfoReq {
  uv_getnameinfo_t req{};
  napi_env env = nullptr;
  napi_ref reqObjRef = nullptr;
};

/// libuv callback after uv_getnameinfo completes.
static void afterGetNameInfo(
    uv_getnameinfo_t *req,
    int status,
    const char *hostname,
    const char *service) {
  auto *wrap = static_cast<GetNameInfoReq *>(req->data);
  napi_env env = wrap->env;

  napi_handle_scope scope;
  napi_open_handle_scope(env, &scope);

  napi_value reqObj;
  napi_get_reference_value(env, wrap->reqObjRef, &reqObj);

  // oncomplete(status, hostname, service)
  napi_value argv[3];
  napi_create_int32(env, status, &argv[0]);
  napi_get_null(env, &argv[1]);
  napi_get_null(env, &argv[2]);

  if (status == 0) {
    napi_create_string_utf8(env, hostname, NAPI_AUTO_LENGTH, &argv[1]);
    napi_create_string_utf8(env, service, NAPI_AUTO_LENGTH, &argv[2]);
  }

  napi_value oncomplete;
  napi_get_named_property(env, reqObj, "oncomplete", &oncomplete);
  napi_valuetype oncompType;
  napi_typeof(env, oncomplete, &oncompType);
  if (oncompType == napi_function) {
    napi_value cbResult;
    napi_call_function(env, reqObj, oncomplete, 3, argv, &cbResult);
  }

  napi_delete_reference(env, wrap->reqObjRef);
  delete wrap;

  napi_close_handle_scope(env, scope);
}

/// getnameinfo(req, ip, port) -> err_code
static napi_value getnameinfoFn(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  // argv[0] = req object
  // argv[1] = ip string
  // argv[2] = port (uint32)

  char ip[256];
  size_t ipLen = 0;
  napi_get_value_string_utf8(env, argv[1], ip, sizeof(ip), &ipLen);

  uint32_t port = 0;
  napi_get_value_uint32(env, argv[2], &port);

  // Parse the IP address.
  struct sockaddr_storage addr {};
  int r = uv_ip4_addr(
      ip,
      static_cast<int>(port),
      reinterpret_cast<struct sockaddr_in *>(&addr));
  if (r != 0) {
    r = uv_ip6_addr(
        ip,
        static_cast<int>(port),
        reinterpret_cast<struct sockaddr_in6 *>(&addr));
  }

  if (r != 0) {
    napi_value result;
    napi_create_int32(env, UV_EINVAL, &result);
    return result;
  }

  auto *wrap = new GetNameInfoReq();
  wrap->env = env;
  wrap->req.data = wrap;
  napi_create_reference(env, argv[0], 1, &wrap->reqObjRef);

  int err = uv_getnameinfo(
      s_caresLoop,
      &wrap->req,
      afterGetNameInfo,
      reinterpret_cast<struct sockaddr *>(&addr),
      NI_NAMEREQD);

  if (err != 0) {
    napi_delete_reference(env, wrap->reqObjRef);
    delete wrap;
  }

  napi_value result;
  napi_create_int32(env, err, &result);
  return result;
}

// ---------------------------------------------------------------------------
// canonicalizeIP(ip) -> canonical_string | undefined
// ---------------------------------------------------------------------------

static napi_value canonicalizeIPFn(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  char ip[256];
  size_t ipLen = 0;
  napi_get_value_string_utf8(env, argv[0], ip, sizeof(ip), &ipLen);

  // Try IPv4.
  unsigned char buf4[sizeof(struct in_addr)];
  if (uv_inet_pton(AF_INET, ip, buf4) == 0) {
    char canonical[INET_ADDRSTRLEN];
    uv_inet_ntop(AF_INET, buf4, canonical, sizeof(canonical));
    napi_value result;
    napi_create_string_utf8(env, canonical, NAPI_AUTO_LENGTH, &result);
    return result;
  }

  // Try IPv6.
  unsigned char buf6[sizeof(struct in6_addr)];
  if (uv_inet_pton(AF_INET6, ip, buf6) == 0) {
    char canonical[INET6_ADDRSTRLEN];
    uv_inet_ntop(AF_INET6, buf6, canonical, sizeof(canonical));
    napi_value result;
    napi_create_string_utf8(env, canonical, NAPI_AUTO_LENGTH, &result);
    return result;
  }

  napi_value undef;
  napi_get_undefined(env, &undef);
  return undef;
}

// ---------------------------------------------------------------------------
// strerror(code) -> string
// ---------------------------------------------------------------------------

static napi_value strerrorFn(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  int32_t code = 0;
  napi_get_value_int32(env, argv[0], &code);

  const char *msg = uv_strerror(code);
  napi_value result;
  napi_create_string_utf8(env, msg, NAPI_AUTO_LENGTH, &result);
  return result;
}

// ---------------------------------------------------------------------------
// ChannelWrap stub (for N5.9 — c-ares DNS queries)
// ---------------------------------------------------------------------------

static napi_value channelWrapCtor(napi_env env, napi_callback_info info) {
  napi_value thisObj;
  napi_get_cb_info(env, info, nullptr, nullptr, &thisObj, nullptr);
  return thisObj;
}

/// Stub: getServers() -> []
static napi_value channelWrapGetServers(napi_env env, napi_callback_info) {
  napi_value arr;
  napi_create_array_with_length(env, 0, &arr);
  return arr;
}

/// Stub: setServers(servers) -> 0
static napi_value channelWrapSetServers(napi_env env, napi_callback_info) {
  napi_value zero;
  napi_create_int32(env, 0, &zero);
  return zero;
}

/// Stub: cancel() -> undefined
static napi_value channelWrapCancel(napi_env env, napi_callback_info) {
  napi_value undef;
  napi_get_undefined(env, &undef);
  return undef;
}

/// Stub: setLocalAddress() -> undefined
static napi_value channelWrapSetLocalAddress(napi_env env, napi_callback_info) {
  napi_value undef;
  napi_get_undefined(env, &undef);
  return undef;
}

// ---------------------------------------------------------------------------
// Simple constructors for request wraps (no-op, just return `this`)
// ---------------------------------------------------------------------------

static napi_value reqWrapCtor(napi_env env, napi_callback_info info) {
  napi_value thisObj;
  napi_get_cb_info(env, info, nullptr, nullptr, &thisObj, nullptr);
  return thisObj;
}

// ---------------------------------------------------------------------------
// initCaresWrapBinding
// ---------------------------------------------------------------------------

napi_value initCaresWrapBinding(napi_env env, napi_value exports) {
  // Functions.
  napi_property_descriptor props[] = {
      {"getaddrinfo",
       nullptr,
       getaddrinfoFn,
       nullptr,
       nullptr,
       nullptr,
       napi_default,
       nullptr},
      {"getnameinfo",
       nullptr,
       getnameinfoFn,
       nullptr,
       nullptr,
       nullptr,
       napi_default,
       nullptr},
      {"canonicalizeIP",
       nullptr,
       canonicalizeIPFn,
       nullptr,
       nullptr,
       nullptr,
       napi_default,
       nullptr},
      {"strerror",
       nullptr,
       strerrorFn,
       nullptr,
       nullptr,
       nullptr,
       napi_default,
       nullptr},
  };
  napi_define_properties(env, exports, sizeof(props) / sizeof(props[0]), props);

  // Constants.
  auto setInt = [&](const char *name, int32_t val) {
    napi_value v;
    napi_create_int32(env, val, &v);
    napi_set_named_property(env, exports, name, v);
  };
  auto setUint = [&](const char *name, uint32_t val) {
    napi_value v;
    napi_create_uint32(env, val, &v);
    napi_set_named_property(env, exports, name, v);
  };

  setInt("AF_INET", AF_INET);
  setInt("AF_INET6", AF_INET6);
  setInt("AF_UNSPEC", AF_UNSPEC);
  setInt("AI_ADDRCONFIG", AI_ADDRCONFIG);
  setInt("AI_ALL", AI_ALL);
  setInt("AI_V4MAPPED", AI_V4MAPPED);
  setUint("DNS_ORDER_VERBATIM", DNS_ORDER_VERBATIM);
  setUint("DNS_ORDER_IPV4_FIRST", DNS_ORDER_IPV4_FIRST);
  setUint("DNS_ORDER_IPV6_FIRST", DNS_ORDER_IPV6_FIRST);

  // GetAddrInfoReqWrap constructor (plain JS object, just returns `this`).
  napi_value getAddrInfoReqWrapCtor;
  napi_define_class(
      env,
      "GetAddrInfoReqWrap",
      NAPI_AUTO_LENGTH,
      reqWrapCtor,
      nullptr,
      0,
      nullptr,
      &getAddrInfoReqWrapCtor);
  napi_set_named_property(
      env, exports, "GetAddrInfoReqWrap", getAddrInfoReqWrapCtor);

  // GetNameInfoReqWrap constructor.
  napi_value getNameInfoReqWrapCtor;
  napi_define_class(
      env,
      "GetNameInfoReqWrap",
      NAPI_AUTO_LENGTH,
      reqWrapCtor,
      nullptr,
      0,
      nullptr,
      &getNameInfoReqWrapCtor);
  napi_set_named_property(
      env, exports, "GetNameInfoReqWrap", getNameInfoReqWrapCtor);

  // QueryReqWrap constructor (stub for N5.9 — c-ares DNS queries).
  napi_value queryReqWrapCtor;
  napi_define_class(
      env,
      "QueryReqWrap",
      NAPI_AUTO_LENGTH,
      reqWrapCtor,
      nullptr,
      0,
      nullptr,
      &queryReqWrapCtor);
  napi_set_named_property(env, exports, "QueryReqWrap", queryReqWrapCtor);

  // ChannelWrap constructor (stub for N5.9).
  // Prototype needs getServers, setServers, cancel, setLocalAddress stubs.
  napi_property_descriptor channelProtoProps[] = {
      {"getServers",
       nullptr,
       channelWrapGetServers,
       nullptr,
       nullptr,
       nullptr,
       napi_default,
       nullptr},
      {"setServers",
       nullptr,
       channelWrapSetServers,
       nullptr,
       nullptr,
       nullptr,
       napi_default,
       nullptr},
      {"cancel",
       nullptr,
       channelWrapCancel,
       nullptr,
       nullptr,
       nullptr,
       napi_default,
       nullptr},
      {"setLocalAddress",
       nullptr,
       channelWrapSetLocalAddress,
       nullptr,
       nullptr,
       nullptr,
       napi_default,
       nullptr},
  };
  napi_value channelWrapCtorVal;
  napi_define_class(
      env,
      "ChannelWrap",
      NAPI_AUTO_LENGTH,
      channelWrapCtor,
      nullptr,
      sizeof(channelProtoProps) / sizeof(channelProtoProps[0]),
      channelProtoProps,
      &channelWrapCtorVal);
  napi_set_named_property(env, exports, "ChannelWrap", channelWrapCtorVal);

  return exports;
}

} // namespace node_compat
} // namespace hermes

#pragma GCC diagnostic pop
