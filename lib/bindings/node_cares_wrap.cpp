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
#include <ares.h>

#include <cassert>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace hermes {
namespace node_compat {

// ---------------------------------------------------------------------------
// Module-level state
// ---------------------------------------------------------------------------

static uv_loop_t *s_caresLoop = nullptr;

/// Set of all live ChannelWrap instances, for shutdown cleanup.
class ChannelWrap;
static std::unordered_set<ChannelWrap *> s_channels;

void setCaresWrapEventLoop(uv_loop_t *loop) {
  s_caresLoop = loop;
}

// caresWrapShutdown() is defined after ChannelWrap (needs complete type).

// DNS order constants (matching Node's cares_wrap.h).
static constexpr uint32_t DNS_ORDER_VERBATIM = 0;
static constexpr uint32_t DNS_ORDER_IPV4_FIRST = 1;
static constexpr uint32_t DNS_ORDER_IPV6_FIRST = 2;

// ---------------------------------------------------------------------------
// Helper: extract JS string to std::string
// ---------------------------------------------------------------------------

static std::string napiStringToStd(napi_env env, napi_value val) {
  size_t len = 0;
  napi_get_value_string_utf8(env, val, nullptr, 0, &len);
  std::string s(len, '\0');
  napi_get_value_string_utf8(env, val, &s[0], len + 1, &len);
  return s;
}

// ---------------------------------------------------------------------------
// Helper: call oncomplete on a JS request object
// ---------------------------------------------------------------------------

static void
callOncomplete(napi_env env, napi_value reqObj, int argc, napi_value *argv) {
  napi_value oncomplete;
  napi_get_named_property(env, reqObj, "oncomplete", &oncomplete);
  napi_valuetype t;
  napi_typeof(env, oncomplete, &t);
  if (t == napi_function) {
    napi_value result;
    napi_call_function(env, reqObj, oncomplete, argc, argv, &result);
  }
}

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
  napi_value argv[2];
  napi_create_int32(env, status, &argv[0]);
  napi_get_null(env, &argv[1]);

  uint32_t n = 0;
  const uint32_t order = wrap->order;

  if (status == 0) {
    napi_value results;
    napi_create_array(env, &results);

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
      napi_create_int32(env, UV_EAI_NODATA, &argv[0]);
    }

    argv[1] = results;
  }

  uv_freeaddrinfo(res);
  callOncomplete(env, reqObj, 2, argv);

  napi_delete_reference(env, wrap->reqObjRef);
  delete wrap;

  napi_close_handle_scope(env, scope);
}

/// getaddrinfo(req, hostname, family, hints, order) -> err_code
static napi_value getaddrinfoFn(napi_env env, napi_callback_info info) {
  size_t argc = 5;
  napi_value argv[5];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  std::string hostname = napiStringToStd(env, argv[1]);
  std::string asciiHostname = ada::idna::to_ascii(hostname);

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

  int32_t flags = 0;
  napi_valuetype flagsType;
  napi_typeof(env, argv[3], &flagsType);
  if (flagsType == napi_number)
    napi_get_value_int32(env, argv[3], &flags);

  uint32_t order = DNS_ORDER_VERBATIM;
  napi_get_value_uint32(env, argv[4], &order);

  auto *wrap = new GetAddrInfoReq();
  wrap->env = env;
  wrap->req.data = wrap;
  wrap->order = order;
  napi_create_reference(env, argv[0], 1, &wrap->reqObjRef);

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

  napi_value argv[3];
  napi_create_int32(env, status, &argv[0]);
  napi_get_null(env, &argv[1]);
  napi_get_null(env, &argv[2]);

  if (status == 0) {
    napi_create_string_utf8(env, hostname, NAPI_AUTO_LENGTH, &argv[1]);
    napi_create_string_utf8(env, service, NAPI_AUTO_LENGTH, &argv[2]);
  }

  callOncomplete(env, reqObj, 3, argv);

  napi_delete_reference(env, wrap->reqObjRef);
  delete wrap;

  napi_close_handle_scope(env, scope);
}

/// getnameinfo(req, ip, port) -> err_code
static napi_value getnameinfoFn(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  char ip[256];
  size_t ipLen = 0;
  napi_get_value_string_utf8(env, argv[1], ip, sizeof(ip), &ipLen);

  uint32_t port = 0;
  napi_get_value_uint32(env, argv[2], &port);

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

  unsigned char buf4[sizeof(struct in_addr)];
  if (uv_inet_pton(AF_INET, ip, buf4) == 0) {
    char canonical[INET_ADDRSTRLEN];
    uv_inet_ntop(AF_INET, buf4, canonical, sizeof(canonical));
    napi_value result;
    napi_create_string_utf8(env, canonical, NAPI_AUTO_LENGTH, &result);
    return result;
  }

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
// Uses ares_strerror for c-ares error codes, uv_strerror for libuv codes.
// ---------------------------------------------------------------------------

static napi_value strerrorFn(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  int32_t code = 0;
  napi_get_value_int32(env, argv[0], &code);

  // c-ares error codes are >= 0, libuv error codes are < 0.
  const char *msg;
  if (code >= 0 && code <= ARES_ECANCELLED) {
    msg = ares_strerror(code);
  } else {
    msg = uv_strerror(code);
  }
  napi_value result;
  napi_create_string_utf8(env, msg, NAPI_AUTO_LENGTH, &result);
  return result;
}

// ---------------------------------------------------------------------------
// ChannelWrap — wraps ares_channel_t with uv_poll_t socket management
// ---------------------------------------------------------------------------

class ChannelWrap;

/// Per-socket tracking structure for c-ares I/O integration with libuv.
struct AresTask {
  ChannelWrap *channel;
  ares_socket_t sock;
  uv_poll_t poll_watcher;
};

class ChannelWrap {
 public:
  ChannelWrap(napi_env env, napi_value jsObj);
  ~ChannelWrap();

  ares_channel_t *channel() const {
    return channel_;
  }
  napi_env env() const {
    return env_;
  }

  void setup(int timeout, int tries);

  /// Close the channel and release all resources. After this, the ChannelWrap
  /// must not be used. The prevent-GC ref is released when all libuv handles
  /// have finished closing.
  void closeChannel();

  // Socket state callback from c-ares.
  static void
  sockStateCb(void *data, ares_socket_t sock, int readable, int writable);

  // Timer callback for c-ares timeout processing.
  static void timerCb(uv_timer_t *timer);

  // Poll callback from libuv.
  static void pollCb(uv_poll_t *watcher, int status, int events);

  void startTimer();

  // NAPI getter for native pointer.
  static ChannelWrap *unwrap(napi_env env, napi_value jsObj);

 private:
  /// Release the prevent-GC ref, allowing GC to collect us.
  void releaseSelfRef();

  napi_env env_;
  ares_channel_t *channel_ = nullptr;
  uv_timer_t timer_{};
  bool timerInitialized_ = false;
  bool closed_ = false;
  napi_ref selfRef_ = nullptr;
  std::unordered_map<ares_socket_t, AresTask *> tasks_;
};

ChannelWrap::ChannelWrap(napi_env env, napi_value jsObj) : env_(env) {
  napi_wrap(
      env,
      jsObj,
      this,
      [](napi_env, void *data, void *) {
        delete static_cast<ChannelWrap *>(data);
      },
      nullptr,
      nullptr);
  // Prevent GC from collecting us while we have active libuv handles.
  napi_create_reference(env, jsObj, 1, &selfRef_);
  s_channels.insert(this);
}

ChannelWrap::~ChannelWrap() {
  s_channels.erase(this);
  // By the time the destructor runs, closeChannel() should have been called
  // (either explicitly or via caresWrapShutdown). If not (e.g. loop already
  // dead), do a best-effort cleanup without touching libuv.
  if (channel_) {
    for (auto &pair : tasks_)
      delete pair.second;
    tasks_.clear();
    ares_destroy(channel_);
    channel_ = nullptr;
  }
}

void ChannelWrap::releaseSelfRef() {
  if (selfRef_) {
    napi_delete_reference(env_, selfRef_);
    selfRef_ = nullptr;
  }
}

void ChannelWrap::closeChannel() {
  if (closed_)
    return;
  closed_ = true;

  // Cancel all pending queries — their callbacks will fire with ARES_ECANCELLED
  // and free their QueryReq.
  if (channel_)
    ares_cancel(channel_);

  // Close all poll watchers.
  for (auto &pair : tasks_) {
    AresTask *task = pair.second;
    uv_poll_stop(&task->poll_watcher);
    uv_close(
        reinterpret_cast<uv_handle_t *>(&task->poll_watcher),
        [](uv_handle_t *h) { delete reinterpret_cast<AresTask *>(h->data); });
  }
  tasks_.clear();

  // Destroy c-ares channel. sockStateCb may fire with readable=0,writable=0
  // for any remaining sockets, but tasks_ is already empty so those are no-ops.
  if (channel_) {
    ares_destroy(channel_);
    channel_ = nullptr;
  }

  // Close the timer. The close callback releases the prevent-GC ref.
  if (timerInitialized_) {
    uv_timer_stop(&timer_);
    uv_close(reinterpret_cast<uv_handle_t *>(&timer_), [](uv_handle_t *h) {
      auto *self = reinterpret_cast<ChannelWrap *>(h->data);
      self->releaseSelfRef();
    });
    timerInitialized_ = false;
  } else {
    // No timer was initialized, release ref immediately.
    releaseSelfRef();
  }
}

void caresWrapShutdown() {
  // Close all live channels before the event loop is destroyed.
  // closeChannel() does NOT remove from s_channels (that happens in dtor).
  auto channels = s_channels;
  for (auto *ch : channels)
    ch->closeChannel();
}

void ChannelWrap::setup(int timeout, int tries) {
  struct ares_options opts {};
  int optmask = ARES_OPT_FLAGS | ARES_OPT_SOCK_STATE_CB;

  opts.flags = ARES_FLAG_NOCHECKRESP;
  opts.sock_state_cb = sockStateCb;
  opts.sock_state_cb_data = this;

  if (timeout > 0) {
    opts.timeout = timeout;
    optmask |= ARES_OPT_TIMEOUTMS;
  }
  if (tries > 0) {
    opts.tries = tries;
    optmask |= ARES_OPT_TRIES;
  }

  int r = ares_init_options(&channel_, &opts, optmask);
  assert(r == ARES_SUCCESS);
  (void)r;

  // Initialize timeout timer.
  uv_timer_init(s_caresLoop, &timer_);
  timer_.data = this;
  uv_unref(reinterpret_cast<uv_handle_t *>(&timer_));
  timerInitialized_ = true;
}

void ChannelWrap::sockStateCb(
    void *data,
    ares_socket_t sock,
    int readable,
    int writable) {
  auto *self = static_cast<ChannelWrap *>(data);
  if (self->closed_)
    return;

  auto it = self->tasks_.find(sock);

  if (readable == 0 && writable == 0) {
    // c-ares is done with this socket. Remove it.
    if (it != self->tasks_.end()) {
      AresTask *task = it->second;
      self->tasks_.erase(it);
      uv_close(
          reinterpret_cast<uv_handle_t *>(&task->poll_watcher),
          [](uv_handle_t *h) { delete reinterpret_cast<AresTask *>(h->data); });
    }
    return;
  }

  AresTask *task;
  if (it == self->tasks_.end()) {
    // New socket — create poll watcher.
    task = new AresTask();
    task->channel = self;
    task->sock = sock;
    uv_poll_init_socket(s_caresLoop, &task->poll_watcher, sock);
    task->poll_watcher.data = task;
    self->tasks_[sock] = task;
  } else {
    task = it->second;
  }

  int events = 0;
  if (readable)
    events |= UV_READABLE;
  if (writable)
    events |= UV_WRITABLE;

  uv_poll_start(&task->poll_watcher, events, pollCb);

  self->startTimer();
}

void ChannelWrap::pollCb(uv_poll_t *watcher, int status, int events) {
  auto *task = static_cast<AresTask *>(watcher->data);
  ChannelWrap *self = task->channel;

  ares_socket_t read_fd = ARES_SOCKET_BAD;
  ares_socket_t write_fd = ARES_SOCKET_BAD;

  if (status == 0) {
    if (events & UV_READABLE)
      read_fd = task->sock;
    if (events & UV_WRITABLE)
      write_fd = task->sock;
  } else {
    // Error on poll — notify c-ares for both directions.
    read_fd = task->sock;
    write_fd = task->sock;
  }

  ares_process_fd(self->channel_, read_fd, write_fd);

  self->startTimer();
}

void ChannelWrap::timerCb(uv_timer_t *timer) {
  auto *self = static_cast<ChannelWrap *>(timer->data);
  // Process timeouts.
  ares_process_fd(self->channel_, ARES_SOCKET_BAD, ARES_SOCKET_BAD);
}

void ChannelWrap::startTimer() {
  // Use ares_timeout to compute next timeout. Default 1 second.
  struct timeval tv;
  struct timeval maxtv;
  maxtv.tv_sec = 1;
  maxtv.tv_usec = 0;
  struct timeval *tvp = ares_timeout(channel_, &maxtv, &tv);
  uint64_t ms = static_cast<uint64_t>(tvp->tv_sec) * 1000 + tvp->tv_usec / 1000;
  if (ms == 0)
    ms = 1;
  uv_timer_start(&timer_, timerCb, ms, 0);
}

ChannelWrap *ChannelWrap::unwrap(napi_env env, napi_value jsObj) {
  void *data = nullptr;
  napi_unwrap(env, jsObj, &data);
  return static_cast<ChannelWrap *>(data);
}

// ---------------------------------------------------------------------------
// QueryReq — wraps an async c-ares query
// ---------------------------------------------------------------------------

/// Enum of query types.
enum class QueryType {
  A,
  AAAA,
  CNAME,
  MX,
  NS,
  TXT,
  SRV,
  PTR,
  NAPTR,
  SOA,
  CAA,
  ANY,
  REVERSE, // getHostByAddr
};

struct QueryReq {
  ChannelWrap *channel;
  napi_env env;
  napi_ref reqObjRef;
  QueryType type;

  // For reverse DNS: stored binary address.
  unsigned char addrBuf[sizeof(struct in6_addr)];
  int addrFamily;
  int addrLen;
};

// Forward declaration.
static void queryCallback(
    void *arg,
    int status,
    int timeouts,
    unsigned char *abuf,
    int alen);
static void
hostCallback(void *arg, int status, int timeouts, struct hostent *host);

// ---------------------------------------------------------------------------
// Query response parsing — builds JS values from c-ares parsed structures
// ---------------------------------------------------------------------------

/// Parse A records (IPv4 addresses + TTLs).
static void parseAReply(
    napi_env env,
    unsigned char *abuf,
    int alen,
    napi_value *result,
    napi_value *ttls) {
  struct ares_addrttl addrttls[256];
  int naddrttls = 256;
  struct hostent *host = nullptr;

  int status = ares_parse_a_reply(abuf, alen, &host, addrttls, &naddrttls);
  if (host)
    ares_free_hostent(host);

  if (status != ARES_SUCCESS) {
    napi_get_null(env, result);
    napi_get_null(env, ttls);
    return;
  }

  napi_create_array_with_length(env, naddrttls, result);
  napi_create_array_with_length(env, naddrttls, ttls);

  for (int i = 0; i < naddrttls; i++) {
    char ip[INET_ADDRSTRLEN];
    uv_inet_ntop(AF_INET, &addrttls[i].ipaddr, ip, sizeof(ip));

    napi_value ipStr, ttlVal;
    napi_create_string_utf8(env, ip, NAPI_AUTO_LENGTH, &ipStr);
    napi_create_int32(env, addrttls[i].ttl, &ttlVal);
    napi_set_element(env, *result, i, ipStr);
    napi_set_element(env, *ttls, i, ttlVal);
  }
}

/// Parse AAAA records (IPv6 addresses + TTLs).
static void parseAAAAReply(
    napi_env env,
    unsigned char *abuf,
    int alen,
    napi_value *result,
    napi_value *ttls) {
  struct ares_addr6ttl addr6ttls[256];
  int naddr6ttls = 256;
  struct hostent *host = nullptr;

  int status = ares_parse_aaaa_reply(abuf, alen, &host, addr6ttls, &naddr6ttls);
  if (host)
    ares_free_hostent(host);

  if (status != ARES_SUCCESS) {
    napi_get_null(env, result);
    napi_get_null(env, ttls);
    return;
  }

  napi_create_array_with_length(env, naddr6ttls, result);
  napi_create_array_with_length(env, naddr6ttls, ttls);

  for (int i = 0; i < naddr6ttls; i++) {
    char ip[INET6_ADDRSTRLEN];
    uv_inet_ntop(AF_INET6, &addr6ttls[i].ip6addr, ip, sizeof(ip));

    napi_value ipStr, ttlVal;
    napi_create_string_utf8(env, ip, NAPI_AUTO_LENGTH, &ipStr);
    napi_create_int32(env, addr6ttls[i].ttl, &ttlVal);
    napi_set_element(env, *result, i, ipStr);
    napi_set_element(env, *ttls, i, ttlVal);
  }
}

/// Parse MX records.
static void
parseMxReply(napi_env env, unsigned char *abuf, int alen, napi_value *result) {
  struct ares_mx_reply *mx_out = nullptr;
  int status = ares_parse_mx_reply(abuf, alen, &mx_out);

  napi_create_array(env, result);

  if (status == ARES_SUCCESS) {
    uint32_t i = 0;
    for (struct ares_mx_reply *mx = mx_out; mx; mx = mx->next, i++) {
      napi_value obj;
      napi_create_object(env, &obj);

      napi_value exchange, priority;
      napi_create_string_utf8(env, mx->host, NAPI_AUTO_LENGTH, &exchange);
      napi_create_int32(env, mx->priority, &priority);
      napi_set_named_property(env, obj, "exchange", exchange);
      napi_set_named_property(env, obj, "priority", priority);

      napi_set_element(env, *result, i, obj);
    }
  }

  if (mx_out)
    ares_free_data(mx_out);
}

/// Parse NS records.
static void
parseNsReply(napi_env env, unsigned char *abuf, int alen, napi_value *result) {
  struct hostent *host = nullptr;
  int status = ares_parse_ns_reply(abuf, alen, &host);

  napi_create_array(env, result);

  if (status == ARES_SUCCESS && host) {
    uint32_t i = 0;
    for (char **p = host->h_aliases; *p; p++, i++) {
      napi_value name;
      napi_create_string_utf8(env, *p, NAPI_AUTO_LENGTH, &name);
      napi_set_element(env, *result, i, name);
    }
    ares_free_hostent(host);
  }
}

/// Parse TXT records.
static void
parseTxtReply(napi_env env, unsigned char *abuf, int alen, napi_value *result) {
  struct ares_txt_ext *txt_out = nullptr;
  int status = ares_parse_txt_reply_ext(abuf, alen, &txt_out);

  napi_create_array(env, result);

  if (status == ARES_SUCCESS) {
    uint32_t ri = 0; // result index
    napi_value currentChunk = nullptr;

    for (struct ares_txt_ext *txt = txt_out; txt; txt = txt->next) {
      if (txt->record_start || !currentChunk) {
        // New TXT record boundary.
        if (currentChunk) {
          napi_set_element(env, *result, ri++, currentChunk);
        }
        napi_create_array(env, &currentChunk);
      }

      // Get length of current chunk array.
      uint32_t chunkLen = 0;
      napi_get_array_length(env, currentChunk, &chunkLen);

      napi_value txtStr;
      napi_create_string_utf8(
          env, reinterpret_cast<const char *>(txt->txt), txt->length, &txtStr);
      napi_set_element(env, currentChunk, chunkLen, txtStr);
    }

    if (currentChunk) {
      napi_set_element(env, *result, ri, currentChunk);
    }
  }

  if (txt_out)
    ares_free_data(txt_out);
}

/// Parse SRV records.
static void
parseSrvReply(napi_env env, unsigned char *abuf, int alen, napi_value *result) {
  struct ares_srv_reply *srv_out = nullptr;
  int status = ares_parse_srv_reply(abuf, alen, &srv_out);

  napi_create_array(env, result);

  if (status == ARES_SUCCESS) {
    uint32_t i = 0;
    for (struct ares_srv_reply *srv = srv_out; srv; srv = srv->next, i++) {
      napi_value obj;
      napi_create_object(env, &obj);

      napi_value name, port, priority, weight;
      napi_create_string_utf8(env, srv->host, NAPI_AUTO_LENGTH, &name);
      napi_create_int32(env, srv->port, &port);
      napi_create_int32(env, srv->priority, &priority);
      napi_create_int32(env, srv->weight, &weight);

      napi_set_named_property(env, obj, "name", name);
      napi_set_named_property(env, obj, "port", port);
      napi_set_named_property(env, obj, "priority", priority);
      napi_set_named_property(env, obj, "weight", weight);

      napi_set_element(env, *result, i, obj);
    }
  }

  if (srv_out)
    ares_free_data(srv_out);
}

/// Parse CNAME records (from the host->h_name of an A query parse).
static void parseCnameReply(
    napi_env env,
    unsigned char *abuf,
    int alen,
    napi_value *result) {
  struct hostent *host = nullptr;
  // Parse as A record to get CNAME from host->h_name.
  int status = ares_parse_a_reply(abuf, alen, &host, nullptr, nullptr);

  napi_create_array(env, result);

  if (status == ARES_SUCCESS && host && host->h_name) {
    napi_value name;
    napi_create_string_utf8(env, host->h_name, NAPI_AUTO_LENGTH, &name);
    napi_set_element(env, *result, 0, name);
    ares_free_hostent(host);
  } else if (host) {
    ares_free_hostent(host);
  }
}

/// Parse PTR records.
static void
parsePtrReply(napi_env env, unsigned char *abuf, int alen, napi_value *result) {
  struct hostent *host = nullptr;
  // ares_parse_ptr_reply needs addr/addrlen/family but we pass null to just
  // parse PTR records from the answer.
  int status = ares_parse_ptr_reply(abuf, alen, nullptr, 0, AF_INET, &host);

  napi_create_array(env, result);

  if (status == ARES_SUCCESS && host) {
    uint32_t i = 0;
    // h_name is the primary hostname.
    if (host->h_name) {
      napi_value name;
      napi_create_string_utf8(env, host->h_name, NAPI_AUTO_LENGTH, &name);
      napi_set_element(env, *result, i++, name);
    }
    // h_aliases contains additional hostnames.
    if (host->h_aliases) {
      for (char **p = host->h_aliases; *p; p++, i++) {
        napi_value name;
        napi_create_string_utf8(env, *p, NAPI_AUTO_LENGTH, &name);
        napi_set_element(env, *result, i, name);
      }
    }
    ares_free_hostent(host);
  }
}

/// Parse NAPTR records.
static void parseNaptrReply(
    napi_env env,
    unsigned char *abuf,
    int alen,
    napi_value *result) {
  struct ares_naptr_reply *naptr_out = nullptr;
  int status = ares_parse_naptr_reply(abuf, alen, &naptr_out);

  napi_create_array(env, result);

  if (status == ARES_SUCCESS) {
    uint32_t i = 0;
    for (struct ares_naptr_reply *n = naptr_out; n; n = n->next, i++) {
      napi_value obj;
      napi_create_object(env, &obj);

      napi_value flags, service, regexp, replacement, order, preference;
      napi_create_string_utf8(
          env,
          reinterpret_cast<const char *>(n->flags),
          NAPI_AUTO_LENGTH,
          &flags);
      napi_create_string_utf8(
          env,
          reinterpret_cast<const char *>(n->service),
          NAPI_AUTO_LENGTH,
          &service);
      napi_create_string_utf8(
          env,
          reinterpret_cast<const char *>(n->regexp),
          NAPI_AUTO_LENGTH,
          &regexp);
      napi_create_string_utf8(
          env, n->replacement, NAPI_AUTO_LENGTH, &replacement);
      napi_create_int32(env, n->order, &order);
      napi_create_int32(env, n->preference, &preference);

      napi_set_named_property(env, obj, "flags", flags);
      napi_set_named_property(env, obj, "service", service);
      napi_set_named_property(env, obj, "regexp", regexp);
      napi_set_named_property(env, obj, "replacement", replacement);
      napi_set_named_property(env, obj, "order", order);
      napi_set_named_property(env, obj, "preference", preference);

      napi_set_element(env, *result, i, obj);
    }
  }

  if (naptr_out)
    ares_free_data(naptr_out);
}

/// Parse SOA record.
static void
parseSoaReply(napi_env env, unsigned char *abuf, int alen, napi_value *result) {
  struct ares_soa_reply *soa_out = nullptr;
  int status = ares_parse_soa_reply(abuf, alen, &soa_out);

  if (status == ARES_SUCCESS && soa_out) {
    napi_create_object(env, result);

    napi_value nsname, hostmaster, serial, refresh, retry, expire, minttl;
    napi_create_string_utf8(env, soa_out->nsname, NAPI_AUTO_LENGTH, &nsname);
    napi_create_string_utf8(
        env, soa_out->hostmaster, NAPI_AUTO_LENGTH, &hostmaster);
    napi_create_uint32(env, soa_out->serial, &serial);
    napi_create_uint32(env, soa_out->refresh, &refresh);
    napi_create_uint32(env, soa_out->retry, &retry);
    napi_create_uint32(env, soa_out->expire, &expire);
    napi_create_uint32(env, soa_out->minttl, &minttl);

    napi_set_named_property(env, *result, "nsname", nsname);
    napi_set_named_property(env, *result, "hostmaster", hostmaster);
    napi_set_named_property(env, *result, "serial", serial);
    napi_set_named_property(env, *result, "refresh", refresh);
    napi_set_named_property(env, *result, "retry", retry);
    napi_set_named_property(env, *result, "expire", expire);
    napi_set_named_property(env, *result, "minttl", minttl);

    ares_free_data(soa_out);
  } else {
    napi_get_null(env, result);
  }
}

/// Parse CAA records.
static void
parseCaaReply(napi_env env, unsigned char *abuf, int alen, napi_value *result) {
  struct ares_caa_reply *caa_out = nullptr;
  int status = ares_parse_caa_reply(abuf, alen, &caa_out);

  napi_create_array(env, result);

  if (status == ARES_SUCCESS) {
    uint32_t i = 0;
    for (struct ares_caa_reply *caa = caa_out; caa; caa = caa->next, i++) {
      napi_value obj;
      napi_create_object(env, &obj);

      napi_value critical;
      napi_create_int32(env, caa->critical, &critical);
      napi_set_named_property(env, obj, "critical", critical);

      // Property is the tag (e.g., "issue", "issuewild", "iodef").
      std::string tag(
          reinterpret_cast<const char *>(caa->property), caa->plength);
      napi_value tagVal;
      napi_create_string_utf8(env, tag.c_str(), tag.length(), &tagVal);

      // Value is the property value.
      napi_value valueVal;
      napi_create_string_utf8(
          env,
          reinterpret_cast<const char *>(caa->value),
          caa->length,
          &valueVal);

      // Node sets the tag as a dynamic key: obj[tag] = value
      napi_set_property(env, obj, tagVal, valueVal);

      napi_set_element(env, *result, i, obj);
    }
  }

  if (caa_out)
    ares_free_data(caa_out);
}

// ---------------------------------------------------------------------------
// c-ares query callback — dispatches to type-specific parsers
// ---------------------------------------------------------------------------

static void queryCallback(
    void *arg,
    int status,
    int /*timeouts*/,
    unsigned char *abuf,
    int alen) {
  auto *req = static_cast<QueryReq *>(arg);
  napi_env env = req->env;

  napi_handle_scope scope;
  napi_open_handle_scope(env, &scope);

  napi_value reqObj;
  napi_get_reference_value(env, req->reqObjRef, &reqObj);

  if (status != ARES_SUCCESS) {
    // Error — call oncomplete(status).
    napi_value argv[1];
    napi_create_int32(env, status, &argv[0]);
    callOncomplete(env, reqObj, 1, argv);
  } else {
    // Parse according to query type.
    napi_value result;
    napi_value ttls;
    napi_get_undefined(env, &ttls);

    switch (req->type) {
      case QueryType::A:
        parseAReply(env, abuf, alen, &result, &ttls);
        break;
      case QueryType::AAAA:
        parseAAAAReply(env, abuf, alen, &result, &ttls);
        break;
      case QueryType::MX:
        parseMxReply(env, abuf, alen, &result);
        break;
      case QueryType::NS:
        parseNsReply(env, abuf, alen, &result);
        break;
      case QueryType::TXT:
        parseTxtReply(env, abuf, alen, &result);
        break;
      case QueryType::SRV:
        parseSrvReply(env, abuf, alen, &result);
        break;
      case QueryType::CNAME:
        parseCnameReply(env, abuf, alen, &result);
        break;
      case QueryType::PTR:
        parsePtrReply(env, abuf, alen, &result);
        break;
      case QueryType::NAPTR:
        parseNaptrReply(env, abuf, alen, &result);
        break;
      case QueryType::SOA:
        parseSoaReply(env, abuf, alen, &result);
        break;
      case QueryType::CAA:
        parseCaaReply(env, abuf, alen, &result);
        break;
      case QueryType::ANY:
        // ANY is complex — return parsed results for supported types.
        // For now, return array with what we can parse (A+AAAA).
        napi_create_array(env, &result);
        break;
      default:
        napi_create_array(env, &result);
        break;
    }

    // oncomplete(status, result, ttls)
    napi_value argv[3];
    napi_create_int32(env, 0, &argv[0]);
    argv[1] = result;
    argv[2] = ttls;
    callOncomplete(env, reqObj, 3, argv);
  }

  napi_delete_reference(env, req->reqObjRef);
  delete req;

  napi_close_handle_scope(env, scope);
}

/// Callback for ares_gethostbyaddr (reverse DNS).
static void
hostCallback(void *arg, int status, int /*timeouts*/, struct hostent *host) {
  auto *req = static_cast<QueryReq *>(arg);
  napi_env env = req->env;

  napi_handle_scope scope;
  napi_open_handle_scope(env, &scope);

  napi_value reqObj;
  napi_get_reference_value(env, req->reqObjRef, &reqObj);

  if (status != ARES_SUCCESS) {
    napi_value argv[1];
    napi_create_int32(env, status, &argv[0]);
    callOncomplete(env, reqObj, 1, argv);
  } else {
    napi_value result;
    napi_create_array(env, &result);
    uint32_t i = 0;

    if (host && host->h_name) {
      napi_value name;
      napi_create_string_utf8(env, host->h_name, NAPI_AUTO_LENGTH, &name);
      napi_set_element(env, result, i++, name);
    }
    if (host && host->h_aliases) {
      for (char **p = host->h_aliases; *p; p++, i++) {
        napi_value name;
        napi_create_string_utf8(env, *p, NAPI_AUTO_LENGTH, &name);
        napi_set_element(env, result, i, name);
      }
    }

    napi_value argv[2];
    napi_create_int32(env, 0, &argv[0]);
    argv[1] = result;
    callOncomplete(env, reqObj, 2, argv);
  }

  napi_delete_reference(env, req->reqObjRef);
  delete req;

  napi_close_handle_scope(env, scope);
}

// ---------------------------------------------------------------------------
// ChannelWrap NAPI methods
// ---------------------------------------------------------------------------

/// Helper to issue a c-ares query from a NAPI method.
/// argv[0] = JS req object, argv[1] = hostname string
static napi_value
doQuery(napi_env env, napi_callback_info info, QueryType type, int dnsType) {
  size_t argc = 2;
  napi_value argv[2];
  napi_value thisObj;
  napi_get_cb_info(env, info, &argc, argv, &thisObj, nullptr);

  ChannelWrap *channel = ChannelWrap::unwrap(env, thisObj);
  if (!channel || !channel->channel()) {
    napi_value err;
    napi_create_int32(env, ARES_ENOTINITIALIZED, &err);
    return err;
  }

  std::string hostname = napiStringToStd(env, argv[1]);
  std::string asciiHostname = ada::idna::to_ascii(hostname);

  auto *req = new QueryReq();
  req->channel = channel;
  req->env = env;
  req->type = type;
  napi_create_reference(env, argv[0], 1, &req->reqObjRef);

  ares_query(
      channel->channel(),
      asciiHostname.c_str(),
      ARES_CLASS_IN,
      dnsType,
      queryCallback,
      req);

  napi_value zero;
  napi_create_int32(env, 0, &zero);
  return zero;
}

static napi_value queryAFn(napi_env env, napi_callback_info info) {
  return doQuery(env, info, QueryType::A, ARES_REC_TYPE_A);
}
static napi_value queryAaaaFn(napi_env env, napi_callback_info info) {
  return doQuery(env, info, QueryType::AAAA, ARES_REC_TYPE_AAAA);
}
static napi_value queryMxFn(napi_env env, napi_callback_info info) {
  return doQuery(env, info, QueryType::MX, ARES_REC_TYPE_MX);
}
static napi_value queryNsFn(napi_env env, napi_callback_info info) {
  return doQuery(env, info, QueryType::NS, ARES_REC_TYPE_NS);
}
static napi_value queryTxtFn(napi_env env, napi_callback_info info) {
  return doQuery(env, info, QueryType::TXT, ARES_REC_TYPE_TXT);
}
static napi_value querySrvFn(napi_env env, napi_callback_info info) {
  return doQuery(env, info, QueryType::SRV, ARES_REC_TYPE_SRV);
}
static napi_value queryCnameFn(napi_env env, napi_callback_info info) {
  return doQuery(env, info, QueryType::CNAME, ARES_REC_TYPE_CNAME);
}
static napi_value queryPtrFn(napi_env env, napi_callback_info info) {
  return doQuery(env, info, QueryType::PTR, ARES_REC_TYPE_PTR);
}
static napi_value queryNaptrFn(napi_env env, napi_callback_info info) {
  return doQuery(env, info, QueryType::NAPTR, ARES_REC_TYPE_NAPTR);
}
static napi_value querySoaFn(napi_env env, napi_callback_info info) {
  return doQuery(env, info, QueryType::SOA, ARES_REC_TYPE_SOA);
}
static napi_value queryCaaFn(napi_env env, napi_callback_info info) {
  return doQuery(env, info, QueryType::CAA, ARES_REC_TYPE_CAA);
}
static napi_value queryAnyFn(napi_env env, napi_callback_info info) {
  return doQuery(env, info, QueryType::ANY, ARES_REC_TYPE_ANY);
}

/// getHostByAddr(req, ip_string) -> error_code
static napi_value getHostByAddrFn(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  napi_value thisObj;
  napi_get_cb_info(env, info, &argc, argv, &thisObj, nullptr);

  ChannelWrap *channel = ChannelWrap::unwrap(env, thisObj);
  if (!channel || !channel->channel()) {
    napi_value err;
    napi_create_int32(env, ARES_ENOTINITIALIZED, &err);
    return err;
  }

  std::string ip = napiStringToStd(env, argv[1]);

  auto *req = new QueryReq();
  req->channel = channel;
  req->env = env;
  req->type = QueryType::REVERSE;
  napi_create_reference(env, argv[0], 1, &req->reqObjRef);

  // Determine address family and parse.
  if (uv_inet_pton(AF_INET, ip.c_str(), req->addrBuf) == 0) {
    req->addrFamily = AF_INET;
    req->addrLen = sizeof(struct in_addr);
  } else if (uv_inet_pton(AF_INET6, ip.c_str(), req->addrBuf) == 0) {
    req->addrFamily = AF_INET6;
    req->addrLen = sizeof(struct in6_addr);
  } else {
    napi_delete_reference(env, req->reqObjRef);
    delete req;
    napi_value err;
    napi_create_int32(env, ARES_ENOTFOUND, &err);
    return err;
  }

  ares_gethostbyaddr(
      channel->channel(),
      req->addrBuf,
      req->addrLen,
      req->addrFamily,
      hostCallback,
      req);

  napi_value zero;
  napi_create_int32(env, 0, &zero);
  return zero;
}

// ---------------------------------------------------------------------------
// ChannelWrap constructor
// ---------------------------------------------------------------------------

static napi_value channelWrapCtor(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3];
  napi_value thisObj;
  napi_get_cb_info(env, info, &argc, argv, &thisObj, nullptr);

  int32_t timeout = 0;
  int32_t tries = 0;

  napi_valuetype t;
  if (argc > 0) {
    napi_typeof(env, argv[0], &t);
    if (t == napi_number)
      napi_get_value_int32(env, argv[0], &timeout);
  }
  if (argc > 1) {
    napi_typeof(env, argv[1], &t);
    if (t == napi_number)
      napi_get_value_int32(env, argv[1], &tries);
  }

  auto *wrap = new ChannelWrap(env, thisObj);
  wrap->setup(timeout, tries);

  return thisObj;
}

// ---------------------------------------------------------------------------
// ChannelWrap: getServers, setServers, cancel, setLocalAddress
// ---------------------------------------------------------------------------

/// getServers() -> [[ip, port], ...]
static napi_value channelWrapGetServers(napi_env env, napi_callback_info info) {
  napi_value thisObj;
  napi_get_cb_info(env, info, nullptr, nullptr, &thisObj, nullptr);

  ChannelWrap *channel = ChannelWrap::unwrap(env, thisObj);
  if (!channel || !channel->channel()) {
    napi_value arr;
    napi_create_array_with_length(env, 0, &arr);
    return arr;
  }

  struct ares_addr_port_node *servers = nullptr;
  int r = ares_get_servers_ports(channel->channel(), &servers);

  napi_value arr;
  napi_create_array(env, &arr);

  if (r == ARES_SUCCESS && servers) {
    uint32_t i = 0;
    for (struct ares_addr_port_node *s = servers; s; s = s->next, i++) {
      char ip[INET6_ADDRSTRLEN];
      if (s->family == AF_INET) {
        uv_inet_ntop(AF_INET, &s->addr.addr4, ip, sizeof(ip));
      } else {
        uv_inet_ntop(AF_INET6, &s->addr.addr6, ip, sizeof(ip));
      }

      int port = s->udp_port ? s->udp_port : 53;

      napi_value tuple;
      napi_create_array_with_length(env, 2, &tuple);
      napi_value ipVal, portVal;
      napi_create_string_utf8(env, ip, NAPI_AUTO_LENGTH, &ipVal);
      napi_create_int32(env, port, &portVal);
      napi_set_element(env, tuple, 0, ipVal);
      napi_set_element(env, tuple, 1, portVal);
      napi_set_element(env, arr, i, tuple);
    }
    ares_free_data(servers);
  }

  return arr;
}

/// setServers(servers) -> error_code
/// Input: [[family, ip_string, port], ...]
static napi_value channelWrapSetServers(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_value thisObj;
  napi_get_cb_info(env, info, &argc, argv, &thisObj, nullptr);

  ChannelWrap *channel = ChannelWrap::unwrap(env, thisObj);
  if (!channel || !channel->channel()) {
    napi_value err;
    napi_create_int32(env, ARES_ENOTINITIALIZED, &err);
    return err;
  }

  uint32_t len = 0;
  napi_get_array_length(env, argv[0], &len);

  if (len == 0) {
    // Setting empty server list clears servers.
    int r = ares_set_servers(channel->channel(), nullptr);
    napi_value result;
    napi_create_int32(env, r, &result);
    return result;
  }

  std::vector<struct ares_addr_port_node> nodes(len);

  for (uint32_t i = 0; i < len; i++) {
    napi_value entry;
    napi_get_element(env, argv[0], i, &entry);

    // [family, ip, port]
    napi_value famVal, ipVal, portVal;
    napi_get_element(env, entry, 0, &famVal);
    napi_get_element(env, entry, 1, &ipVal);
    napi_get_element(env, entry, 2, &portVal);

    int32_t family = 0;
    napi_get_value_int32(env, famVal, &family);

    std::string ip = napiStringToStd(env, ipVal);

    int32_t port = 53;
    napi_get_value_int32(env, portVal, &port);

    nodes[i].family = (family == 6) ? AF_INET6 : AF_INET;

    if (nodes[i].family == AF_INET) {
      uv_inet_pton(AF_INET, ip.c_str(), &nodes[i].addr.addr4);
    } else {
      uv_inet_pton(AF_INET6, ip.c_str(), &nodes[i].addr.addr6);
    }

    nodes[i].udp_port = port;
    nodes[i].tcp_port = port;
    nodes[i].next = (i + 1 < len) ? &nodes[i + 1] : nullptr;
  }

  int r = ares_set_servers_ports(channel->channel(), &nodes[0]);

  napi_value result;
  napi_create_int32(env, r, &result);
  return result;
}

/// cancel() -> undefined
static napi_value channelWrapCancel(napi_env env, napi_callback_info info) {
  napi_value thisObj;
  napi_get_cb_info(env, info, nullptr, nullptr, &thisObj, nullptr);

  ChannelWrap *channel = ChannelWrap::unwrap(env, thisObj);
  if (channel && channel->channel()) {
    ares_cancel(channel->channel());
  }

  napi_value undef;
  napi_get_undefined(env, &undef);
  return undef;
}

/// setLocalAddress(ipv4, ipv6) -> undefined
static napi_value channelWrapSetLocalAddress(
    napi_env env,
    napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  napi_value thisObj;
  napi_get_cb_info(env, info, &argc, argv, &thisObj, nullptr);

  ChannelWrap *channel = ChannelWrap::unwrap(env, thisObj);
  if (!channel || !channel->channel()) {
    napi_value undef;
    napi_get_undefined(env, &undef);
    return undef;
  }

  if (argc > 0) {
    napi_valuetype t;
    napi_typeof(env, argv[0], &t);
    if (t == napi_string) {
      std::string ipv4 = napiStringToStd(env, argv[0]);
      unsigned int addr;
      if (uv_inet_pton(AF_INET, ipv4.c_str(), &addr) == 0) {
        ares_set_local_ip4(channel->channel(), ntohl(addr));
      }
    }
  }

  if (argc > 1) {
    napi_valuetype t;
    napi_typeof(env, argv[1], &t);
    if (t == napi_string) {
      std::string ipv6 = napiStringToStd(env, argv[1]);
      unsigned char addr6[16];
      if (uv_inet_pton(AF_INET6, ipv6.c_str(), addr6) == 0) {
        ares_set_local_ip6(channel->channel(), addr6);
      }
    }
  }

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
  // Initialize c-ares library.
  ares_library_init(ARES_LIB_INIT_ALL);

  // Top-level functions.
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

  // GetAddrInfoReqWrap constructor.
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

  // QueryReqWrap constructor.
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

  // ChannelWrap constructor + prototype methods.
  // clang-format off
  napi_property_descriptor channelProtoProps[] = {
      {"getServers", nullptr, channelWrapGetServers, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"setServers", nullptr, channelWrapSetServers, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"cancel", nullptr, channelWrapCancel, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"setLocalAddress", nullptr, channelWrapSetLocalAddress, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"queryA", nullptr, queryAFn, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"queryAaaa", nullptr, queryAaaaFn, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"queryMx", nullptr, queryMxFn, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"queryNs", nullptr, queryNsFn, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"queryTxt", nullptr, queryTxtFn, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"querySrv", nullptr, querySrvFn, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"queryCname", nullptr, queryCnameFn, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"queryPtr", nullptr, queryPtrFn, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"queryNaptr", nullptr, queryNaptrFn, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"querySoa", nullptr, querySoaFn, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"queryCaa", nullptr, queryCaaFn, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"queryAny", nullptr, queryAnyFn, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"getHostByAddr", nullptr, getHostByAddrFn, nullptr, nullptr, nullptr, napi_default, nullptr},
  };
  // clang-format on

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
