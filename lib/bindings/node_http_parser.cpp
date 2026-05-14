/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <hermes/node-compat/bindings/handle_wrap_base.h>
#include <hermes/node-compat/bindings/libuv_stream_base.h>
#include <hermes/node-compat/bindings/node_http_parser.h>
#include <node_api.h>
#include <uv.h>

#include <llhttp.h>

#include <algorithm>
#include <cassert>
#include <cstring>
#include <set>
#include <string>
#include <vector>

namespace hermes {
namespace node_compat {

// ============================================================================
// Constants
// ============================================================================

// Callback slot indices on the parser JS object.
enum ParserCallbackSlot {
  kOnMessageBegin = 0,
  kOnHeaders = 1,
  kOnHeadersComplete = 2,
  kOnBody = 3,
  kOnMessageComplete = 4,
  kOnExecute = 5,
  kOnTimeout = 6,
};

// Lenient flags (must match Node).
enum LenientFlags {
  kLenientNone = 0,
  kLenientHeaders = 1 << 0,
  kLenientChunkedLength = 1 << 1,
  kLenientKeepAlive = 1 << 2,
  kLenientTransferEncoding = 1 << 3,
  kLenientVersion = 1 << 4,
  kLenientDataAfterClose = 1 << 5,
  kLenientOptionalLFAfterCR = 1 << 6,
  kLenientOptionalCRLFAfterChunk = 1 << 7,
  kLenientOptionalCRBeforeLF = 1 << 8,
  kLenientSpacesAfterChunkSize = 1 << 9,
  kLenientAll = (1 << 10) - 1,
};

// Max header fields before flushing via kOnHeaders.
static constexpr int kMaxHeaderFieldsCount = 32;

// Default max HTTP header size (80KB, matching Node).
static constexpr uint32_t kDefaultMaxHttpHeaderSize = 80 * 1024;

// Max chunk extension size (16KB, matching Node).
static constexpr size_t kMaxChunkExtensionsSize = 16384;

// ============================================================================
// HTTP method name tables
// ============================================================================

// Standard methods (HTTP_METHOD_MAP from llhttp). Indexed by llhttp_method_t.
// Note: indices 34+ are RTSP methods included in allMethods but not methods.
static const char *const kMethodNames[] = {
    "DELETE",     "GET",        "HEAD",        "POST",      "PUT",
    "CONNECT",    "OPTIONS",    "TRACE",       "COPY",      "LOCK",
    "MKCOL",      "MOVE",       "PROPFIND",    "PROPPATCH", "SEARCH",
    "UNLOCK",     "BIND",       "REBIND",      "UNBIND",    "ACL",
    "REPORT",     "MKACTIVITY", "CHECKOUT",    "MERGE",     "M-SEARCH",
    "NOTIFY",     "SUBSCRIBE",  "UNSUBSCRIBE", "PATCH",     "PURGE",
    "MKCALENDAR", "LINK",       "UNLINK",      "SOURCE",    "QUERY",
};
static constexpr int kMethodCount =
    static_cast<int>(sizeof(kMethodNames) / sizeof(kMethodNames[0]));

// All methods including RTSP (HTTP_ALL_METHOD_MAP).
static const char *const kAllMethodNames[] = {
    "DELETE",     "GET",           "HEAD",          "POST",      "PUT",
    "CONNECT",    "OPTIONS",       "TRACE",         "COPY",      "LOCK",
    "MKCOL",      "MOVE",          "PROPFIND",      "PROPPATCH", "SEARCH",
    "UNLOCK",     "BIND",          "REBIND",        "UNBIND",    "ACL",
    "REPORT",     "MKACTIVITY",    "CHECKOUT",      "MERGE",     "M-SEARCH",
    "NOTIFY",     "SUBSCRIBE",     "UNSUBSCRIBE",   "PATCH",     "PURGE",
    "MKCALENDAR", "LINK",          "UNLINK",        "SOURCE",    "PRI",
    "DESCRIBE",   "ANNOUNCE",      "SETUP",         "PLAY",      "PAUSE",
    "TEARDOWN",   "GET_PARAMETER", "SET_PARAMETER", "REDIRECT",  "RECORD",
    "FLUSH",      "QUERY",
};
static constexpr int kAllMethodCount =
    static_cast<int>(sizeof(kAllMethodNames) / sizeof(kAllMethodNames[0]));

// ============================================================================
// Parser class — wraps llhttp_t
// ============================================================================

// Forward declaration of Parser; full definition appears below
// ConnectionsList. The dependency direction is:
//   ParserComparator -- needs Parser members (out-of-line, after Parser).
//   ConnectionsList  -- holds std::set<Parser*, ParserComparator>;
//                       its inline push/pop need ConnectionsList complete
//                       at Parser's inline method bodies, so it must be
//                       defined BEFORE Parser.
//   Parser           -- holds ConnectionsList* and its inline lifecycle
//                       methods call ConnectionsList::push/pop.
class Parser;

// Strict-weak ordering for the std::set<Parser*> instances inside
// ConnectionsList. Keyed on (lastMessageStart_, pointer), with the
// special case that idle parsers (lastMessageStart_ == 0) sort before
// active ones. Mirrors Node's ParserComparator in node_http_parser.cc.
// Body is defined out-of-line after Parser is complete.
struct ParserComparator {
  bool operator()(const Parser *lhs, const Parser *rhs) const;
};

// ============================================================================
// ConnectionsList -- tracks server-attached parsers for closeAllConnections,
// closeIdleConnections, and header/request timeout enforcement.
//
// Two ordered sets are maintained:
//   * all_    -- every parser the server has Initialized with this list.
//   * active_ -- subset that is currently mid-message (between
//                on_message_begin and on_message_complete).
//
// Both sets are keyed on Parser* with ParserComparator: the sort key is
// (lastMessageStart_, pointer), with the special case that idle parsers
// (lastMessageStart_ == 0) sort before active ones. The expired() method
// relies on this ordering, and any code that mutates lastMessageStart_
// MUST Pop the parser from both sets before mutating and Push afterwards
// -- otherwise std::set::erase silently fails to find the entry.
//
// We do NOT take napi_refs to the parser JS objects here. Each Parser
// already holds a strong selfRef_ that keeps its JS object alive, so we
// can recover the JS object from any Parser* via getJsObject(). This
// avoids ref bookkeeping when parsers are reordered (Pop/Push on
// timestamp change).
// ============================================================================

class ConnectionsList {
 public:
  ConnectionsList() = default;
  ~ConnectionsList() {
    // Do NOT call napi_delete_reference here -- see Parser destructor.
  }

  void init(napi_env env, napi_value jsObj) {
    env_ = env;
    napi_wrap(env, jsObj, this, pointerCb, nullptr, nullptr);
    napi_create_reference(env, jsObj, 1, &selfRef_);
  }

  void push(Parser *parser) {
    all_.insert(parser);
  }
  void pop(Parser *parser) {
    all_.erase(parser);
  }
  void pushActive(Parser *parser) {
    active_.insert(parser);
  }
  void popActive(Parser *parser) {
    active_.erase(parser);
  }

  napi_value all(napi_env env);
  napi_value idle(napi_env env);
  napi_value active(napi_env env);
  napi_value
  expired(napi_env env, uint32_t headersTimeoutMs, uint32_t requestTimeoutMs);

  static ConnectionsList *unwrap(napi_env env, napi_value obj) {
    void *data;
    napi_unwrap(env, obj, &data);
    return static_cast<ConnectionsList *>(data);
  }

 private:
  static void pointerCb(napi_env /*env*/, void *data, void * /*hint*/) {
    delete static_cast<ConnectionsList *>(data);
  }

  napi_env env_ = nullptr;
  napi_ref selfRef_ = nullptr;
  std::set<Parser *, ParserComparator> all_;
  std::set<Parser *, ParserComparator> active_;
};

// ============================================================================
// Parser class -- wraps llhttp_t
// ============================================================================

class Parser {
  friend class ConnectionsList;
  friend struct ParserComparator;

 public:
  Parser() {
    memset(&parser_, 0, sizeof(parser_));
    memset(&settings_, 0, sizeof(settings_));
    initSettings();
    parser_.data = this;
  }

  ~Parser() {
    // Defensive: mirror Node's invariant that a Parser is never destroyed
    // while still referenced from a ConnectionsList. Normally
    // freeParser() in JS calls parser.remove() before parser.close(), but
    // protect against a torn-down env or buggy caller leaving a dangling
    // pointer in the std::set.
    if (connectionsList_) {
      connectionsList_->pop(this);
      connectionsList_->popActive(this);
      connectionsList_ = nullptr;
    }
    // Do NOT call napi_delete_reference here -- this destructor is called
    // from the GC finalizer (pointerCb), and the env may already be
    // destroyed. NAPI refs are cleaned up by the runtime automatically.
  }

  void init(napi_env env, napi_value jsObj) {
    env_ = env;
    napi_wrap(env, jsObj, this, pointerCb, nullptr, nullptr);
    // Prevent-GC reference.
    napi_create_reference(env, jsObj, 1, &selfRef_);
  }

  napi_value getJsObject() const {
    if (!selfRef_)
      return nullptr;
    napi_value obj;
    napi_get_reference_value(env_, selfRef_, &obj);
    return obj;
  }

  // -------------------------------------------------------------------
  // initialize(type, asyncResource, maxHeaderSize?, lenientFlags?,
  //            connectionsList?)
  // -------------------------------------------------------------------
  void initialize(int type, uint32_t maxHeaderSize, int lenientFlags) {
    llhttp_init(&parser_, static_cast<llhttp_type_t>(type), &settings_);
    parser_.data = this;

    maxHttpHeaderSize_ = maxHeaderSize;
    headerNread_ = 0;
    url_.clear();
    statusMessage_.clear();
    numFields_ = 0;
    numValues_ = 0;
    haveFlushed_ = false;
    gotException_ = false;
    headersCompleted_ = false;
    chunkExtensionNread_ = 0;
    pendingPause_ = false;

    applyLenientFlags(lenientFlags);
  }

  // -------------------------------------------------------------------
  // execute(buffer) -> nread | Error
  // -------------------------------------------------------------------
  napi_value execute(napi_env env, const char *data, size_t len) {
    currentBufferData_ = data;
    currentBufferLen_ = len;
    gotException_ = false;

    llhttp_errno_t err;
    if (data == nullptr) {
      err = llhttp_finish(&parser_);
    } else {
      err = llhttp_execute(&parser_, data, len);
    }

    // Save accumulated strings (they may point into data buffer).
    save();

    size_t nread;
    if (err != HPE_OK && err != HPE_PAUSED && err != HPE_PAUSED_UPGRADE) {
      nread = llhttp_get_error_pos(&parser_) - data;
    } else {
      nread = len;
    }

    if (err == HPE_PAUSED_UPGRADE) {
      err = HPE_OK;
      llhttp_resume_after_upgrade(&parser_);
    }

    if (pendingPause_) {
      pendingPause_ = false;
      llhttp_pause(&parser_);
    }

    currentBufferData_ = nullptr;
    currentBufferLen_ = 0;

    if (gotException_) {
      // A JS exception was thrown inside a callback. Return undefined
      // and let NAPI propagate the exception.
      return nullptr;
    }

    if (err != HPE_OK && err != HPE_PAUSED) {
      // Build an Error object with bytesParsed, code, reason.
      napi_value errObj;
      napi_create_error(
          env, nullptr, createString(env, "Parse Error"), &errObj);
      setProperty(
          env,
          errObj,
          "bytesParsed",
          createUint32(env, static_cast<uint32_t>(nread)));
      setProperty(
          env, errObj, "code", createString(env, llhttp_errno_name(err)));
      setProperty(
          env,
          errObj,
          "reason",
          createString(env, llhttp_get_error_reason(&parser_)));
      return errObj;
    }

    return createUint32(env, static_cast<uint32_t>(nread));
  }

  // -------------------------------------------------------------------
  // finish() -> undefined | Error
  // -------------------------------------------------------------------
  napi_value finish(napi_env env) {
    return execute(env, nullptr, 0);
  }

  // -------------------------------------------------------------------
  // pause() / resume()
  // -------------------------------------------------------------------
  void pause() {
    llhttp_pause(&parser_);
  }

  void resume() {
    llhttp_resume(&parser_);
    llhttp_resume_after_upgrade(&parser_);
  }

  // -------------------------------------------------------------------
  // consume(handle) / unconsume()
  //
  // In Node, consume attaches the parser as a StreamListener on the
  // native stream, so data bypasses JS. We implement this by saving
  // the stream reference and replacing the handle's onread callback.
  // When data arrives, it is fed directly to execute().
  // -------------------------------------------------------------------
  void consume(napi_env env, napi_value streamHandle) {
    if (consumedStreamRef_) {
      napi_delete_reference(env, consumedStreamRef_);
      consumedStreamRef_ = nullptr;
    }
    napi_create_reference(env, streamHandle, 1, &consumedStreamRef_);
    consumed_ = true;

    // Install our onStreamRead callback on the handle.
    // Save the original onread and replace with our consumer function.
    //
    // Actually, for our implementation we rely on the JS-level
    // consume pattern: _http_server.js sets kOnExecute callback and
    // the stream_base_commons.js onStreamRead() will call
    // parser.execute() via the kOnExecute path. The consume/unconsume
    // here just tracks state so JS knows whether data should flow
    // through the parser.
    //
    // The actual data routing happens via LibuvStreamBase's onRead
    // calling handle.onread(buf) -> stream_base_commons onStreamRead()
    // which checks if there's a parser consuming the stream.
  }

  void unconsume(napi_env env) {
    if (consumedStreamRef_) {
      napi_delete_reference(env, consumedStreamRef_);
      consumedStreamRef_ = nullptr;
    }
    consumed_ = false;
  }

  // -------------------------------------------------------------------
  // getCurrentBuffer() -> Buffer
  // -------------------------------------------------------------------
  napi_value getCurrentBuffer(napi_env env) {
    if (!currentBufferData_ || currentBufferLen_ == 0) {
      napi_value undef;
      napi_get_undefined(env, &undef);
      return undef;
    }
    napi_value buf;
    void *data;
    napi_create_buffer_copy(
        env, currentBufferLen_, currentBufferData_, &data, &buf);
    return buf;
  }

  static Parser *unwrap(napi_env env, napi_value obj) {
    void *data;
    napi_unwrap(env, obj, &data);
    return static_cast<Parser *>(data);
  }

  // Optional ConnectionsList this parser is registered with. Used by
  // http.Server.closeAllConnections() / closeIdleConnections() /
  // checkConnections() (header/request timeouts) to enumerate parsers
  // attached to the server. Borrowed pointer -- the JS side keeps the
  // list alive via the server reference.
  ConnectionsList *connectionsList_ = nullptr;

  // uv_hrtime() at which the parser started reading the current message.
  // Reset to 0 in on_message_complete (parser becomes idle). Used as the
  // sort key for ConnectionsList's std::set, so it MUST NOT be mutated
  // while the parser is in either set -- always Pop before mutating.
  uint64_t lastMessageStart_ = 0;

 private:
  // -------------------------------------------------------------------
  // llhttp settings callbacks (static trampolines)
  // -------------------------------------------------------------------

  void initSettings() {
    llhttp_settings_init(&settings_);
    settings_.on_message_begin = sOnMessageBegin;
    settings_.on_url = sOnUrl;
    settings_.on_url_complete = sOnUrlComplete;
    settings_.on_status = sOnStatus;
    settings_.on_status_complete = sOnStatusComplete;
    settings_.on_header_field = sOnHeaderField;
    settings_.on_header_field_complete = sOnHeaderFieldComplete;
    settings_.on_header_value = sOnHeaderValue;
    settings_.on_header_value_complete = sOnHeaderValueComplete;
    settings_.on_headers_complete = sOnHeadersComplete;
    settings_.on_body = sOnBody;
    settings_.on_message_complete = sOnMessageComplete;
    settings_.on_chunk_header = sOnChunkHeader;
    settings_.on_chunk_complete = sOnChunkComplete;
    settings_.on_chunk_extension_name = sOnChunkExtension;
    settings_.on_chunk_extension_value = sOnChunkExtension;
  }

  static Parser *fromParser(llhttp_t *p) {
    return static_cast<Parser *>(p->data);
  }

  // Track header bytes for overflow detection.
  int trackHeader(size_t len) {
    headerNread_ += len;
    if (headerNread_ >= maxHttpHeaderSize_) {
      llhttp_set_error_reason(&parser_, "HPE_HEADER_OVERFLOW");
      return HPE_USER;
    }
    return 0;
  }

  // --- on_message_begin ---
  static int sOnMessageBegin(llhttp_t *p) {
    Parser *self = fromParser(p);

    // Important: Pop from the lists BEFORE resetting lastMessageStart_,
    // otherwise std::set::erase will fail (the comparator keys on it).
    if (self->connectionsList_) {
      self->connectionsList_->pop(self);
      self->connectionsList_->popActive(self);
    }

    self->headerNread_ = 0;
    self->url_.clear();
    self->statusMessage_.clear();
    self->numFields_ = 0;
    self->numValues_ = 0;
    self->haveFlushed_ = false;
    self->headersCompleted_ = false;
    self->chunkExtensionNread_ = 0;
    self->lastMessageStart_ = uv_hrtime();

    if (self->connectionsList_) {
      self->connectionsList_->push(self);
      self->connectionsList_->pushActive(self);
    }

    napi_value jsObj = self->getJsObject();
    if (!jsObj)
      return 0;

    napi_value cb;
    napi_get_element(self->env_, jsObj, kOnMessageBegin, &cb);
    napi_valuetype type;
    napi_typeof(self->env_, cb, &type);
    if (type == napi_function) {
      napi_value result;
      napi_status st =
          napi_call_function(self->env_, jsObj, cb, 0, nullptr, &result);
      if (st != napi_ok) {
        self->gotException_ = true;
        return HPE_USER;
      }
    }
    return 0;
  }

  // --- on_url ---
  static int sOnUrl(llhttp_t *p, const char *at, size_t length) {
    Parser *self = fromParser(p);
    int err = self->trackHeader(length);
    if (err)
      return err;
    self->url_.append(at, length);
    return 0;
  }

  static int sOnUrlComplete(llhttp_t *) {
    return 0;
  }

  // --- on_status ---
  static int sOnStatus(llhttp_t *p, const char *at, size_t length) {
    Parser *self = fromParser(p);
    int err = self->trackHeader(length);
    if (err)
      return err;
    self->statusMessage_.append(at, length);
    return 0;
  }

  static int sOnStatusComplete(llhttp_t *) {
    return 0;
  }

  // --- on_header_field ---
  static int sOnHeaderField(llhttp_t *p, const char *at, size_t length) {
    Parser *self = fromParser(p);
    int err = self->trackHeader(length);
    if (err)
      return err;

    if (self->numFields_ == self->numValues_) {
      // Starting a new header field.
      self->numFields_++;
      if (self->numFields_ > kMaxHeaderFieldsCount) {
        // Flush accumulated headers to JS.
        self->flush();
        self->numFields_ = 1;
        self->numValues_ = 0;
      }
      if (self->numFields_ <= kMaxHeaderFieldsCount) {
        self->fields_[self->numFields_ - 1].clear();
      }
    }
    if (self->numFields_ <= kMaxHeaderFieldsCount) {
      self->fields_[self->numFields_ - 1].append(at, length);
    }
    return 0;
  }

  static int sOnHeaderFieldComplete(llhttp_t *) {
    return 0;
  }

  // --- on_header_value ---
  static int sOnHeaderValue(llhttp_t *p, const char *at, size_t length) {
    Parser *self = fromParser(p);
    int err = self->trackHeader(length);
    if (err)
      return err;

    if (self->numValues_ != self->numFields_) {
      // Starting a new header value.
      self->numValues_++;
      if (self->numValues_ <= kMaxHeaderFieldsCount) {
        self->values_[self->numValues_ - 1].clear();
      }
    }
    if (self->numValues_ <= kMaxHeaderFieldsCount) {
      self->values_[self->numValues_ - 1].append(at, length);
    }
    return 0;
  }

  static int sOnHeaderValueComplete(llhttp_t *) {
    return 0;
  }

  // --- on_headers_complete ---
  static int sOnHeadersComplete(llhttp_t *p) {
    Parser *self = fromParser(p);
    self->headersCompleted_ = true;

    napi_env env = self->env_;
    napi_value jsObj = self->getJsObject();
    if (!jsObj)
      return 0;

    // Flush any remaining headers first if we have already flushed.
    if (self->haveFlushed_) {
      self->flush();
    }

    // Build headers array: [name1, value1, name2, value2, ...]
    int numPairs = self->numValues_ < self->numFields_ ? self->numValues_
                                                       : self->numFields_;
    napi_value headersArr;
    napi_create_array_with_length(env, numPairs * 2, &headersArr);
    for (int i = 0; i < numPairs; i++) {
      napi_set_element(
          env, headersArr, i * 2, createString(env, self->fields_[i]));
      napi_set_element(
          env, headersArr, i * 2 + 1, createString(env, self->values_[i]));
    }

    // Build callback args: (versionMajor, versionMinor, headers, method,
    //   url, statusCode, statusMessage, upgrade, shouldKeepAlive)
    napi_value args[9];
    napi_create_uint32(env, p->http_major, &args[0]);
    napi_create_uint32(env, p->http_minor, &args[1]);

    if (self->haveFlushed_) {
      // Headers already flushed, pass undefined.
      napi_get_undefined(env, &args[2]);
    } else {
      args[2] = headersArr;
    }

    if (p->type == HTTP_REQUEST) {
      napi_create_uint32(env, p->method, &args[3]);
      args[4] = createString(env, self->url_);
      napi_get_undefined(env, &args[5]);
      napi_get_undefined(env, &args[6]);
    } else {
      napi_get_undefined(env, &args[3]);
      napi_get_undefined(env, &args[4]);
      napi_create_uint32(env, p->status_code, &args[5]);
      args[6] = createString(env, self->statusMessage_);
    }

    napi_get_boolean(env, p->upgrade != 0, &args[7]);
    napi_get_boolean(env, llhttp_should_keep_alive(p) != 0, &args[8]);

    napi_value cb;
    napi_get_element(env, jsObj, kOnHeadersComplete, &cb);
    napi_valuetype cbType;
    napi_typeof(env, cb, &cbType);
    if (cbType != napi_function) {
      return 0;
    }

    napi_value result;
    napi_status st = napi_call_function(env, jsObj, cb, 9, args, &result);
    if (st != napi_ok) {
      self->gotException_ = true;
      return HPE_USER;
    }

    // The return value from kOnHeadersComplete:
    // 0 = continue, 1 = skip body, 2 = pause for upgrade
    int32_t headersAction = 0;
    napi_get_value_int32(env, result, &headersAction);

    if (headersAction == 1) {
      // Skip body.
      return 1;
    }
    if (headersAction == 2) {
      // Pause for upgrade. Set pending_pause so it takes effect
      // after this callback returns.
      self->pendingPause_ = true;
      return 0;
    }

    return 0;
  }

  // --- on_body ---
  static int sOnBody(llhttp_t *p, const char *at, size_t length) {
    Parser *self = fromParser(p);
    napi_env env = self->env_;
    napi_value jsObj = self->getJsObject();
    if (!jsObj)
      return 0;

    napi_value cb;
    napi_get_element(env, jsObj, kOnBody, &cb);
    napi_valuetype cbType;
    napi_typeof(env, cb, &cbType);
    if (cbType != napi_function) {
      return 0;
    }

    // Create a Buffer copy of the body chunk.
    napi_value buf;
    void *data;
    napi_create_buffer_copy(env, length, at, &data, &buf);

    napi_value args[1] = {buf};
    napi_value result;
    napi_status st = napi_call_function(env, jsObj, cb, 1, args, &result);
    if (st != napi_ok) {
      self->gotException_ = true;
      return HPE_USER;
    }
    return 0;
  }

  // --- on_message_complete ---
  static int sOnMessageComplete(llhttp_t *p) {
    Parser *self = fromParser(p);

    // Important: Pop BEFORE clearing lastMessageStart_ so std::set::erase
    // can find the parser. Then re-Push (only into the main list, not
    // active_) so closeIdleConnections() / Idle() can still find it.
    if (self->connectionsList_) {
      self->connectionsList_->pop(self);
      self->connectionsList_->popActive(self);
    }
    self->lastMessageStart_ = 0;
    if (self->connectionsList_) {
      self->connectionsList_->push(self);
    }

    // Flush any trailing headers.
    if (self->numFields_ > 0) {
      self->flush();
    }

    napi_env env = self->env_;
    napi_value jsObj = self->getJsObject();
    if (!jsObj)
      return 0;

    napi_value cb;
    napi_get_element(env, jsObj, kOnMessageComplete, &cb);
    napi_valuetype cbType;
    napi_typeof(env, cb, &cbType);
    if (cbType != napi_function) {
      return 0;
    }

    napi_value result;
    napi_status st = napi_call_function(env, jsObj, cb, 0, nullptr, &result);
    if (st != napi_ok) {
      self->gotException_ = true;
      return HPE_USER;
    }
    return 0;
  }

  // --- on_chunk_header / on_chunk_complete ---
  static int sOnChunkHeader(llhttp_t *) {
    return 0;
  }

  static int sOnChunkComplete(llhttp_t *) {
    return 0;
  }

  // --- on_chunk_extension (name and value) ---
  static int sOnChunkExtension(llhttp_t *p, const char *, size_t length) {
    Parser *self = fromParser(p);
    self->chunkExtensionNread_ += length;
    if (self->chunkExtensionNread_ > kMaxChunkExtensionsSize) {
      llhttp_set_error_reason(p, "HPE_CHUNK_EXTENSIONS_OVERFLOW");
      return HPE_USER;
    }
    return 0;
  }

  // -------------------------------------------------------------------
  // Internal helpers
  // -------------------------------------------------------------------

  // Flush accumulated headers to JS via kOnHeaders callback.
  void flush() {
    napi_env env = env_;
    napi_value jsObj = getJsObject();
    if (!jsObj)
      return;

    int numPairs = numValues_ < numFields_ ? numValues_ : numFields_;

    napi_value headersArr;
    napi_create_array_with_length(env, numPairs * 2, &headersArr);
    for (int i = 0; i < numPairs; i++) {
      napi_set_element(env, headersArr, i * 2, createString(env, fields_[i]));
      napi_set_element(
          env, headersArr, i * 2 + 1, createString(env, values_[i]));
    }

    napi_value urlVal = createString(env, url_);

    napi_value cb;
    napi_get_element(env, jsObj, kOnHeaders, &cb);
    napi_valuetype cbType;
    napi_typeof(env, cb, &cbType);
    if (cbType == napi_function) {
      napi_value args[2] = {headersArr, urlVal};
      napi_value result;
      napi_call_function(env, jsObj, cb, 2, args, &result);
    }

    url_.clear();
    numFields_ = 0;
    numValues_ = 0;
    haveFlushed_ = true;
  }

  // Save string data that might point into the parse buffer.
  // For our implementation using std::string, data is already
  // heap-allocated, so this is a no-op.
  void save() {}

  void applyLenientFlags(int flags) {
    if (flags & kLenientHeaders)
      llhttp_set_lenient_headers(&parser_, 1);
    if (flags & kLenientChunkedLength)
      llhttp_set_lenient_chunked_length(&parser_, 1);
    if (flags & kLenientKeepAlive)
      llhttp_set_lenient_keep_alive(&parser_, 1);
    if (flags & kLenientTransferEncoding)
      llhttp_set_lenient_transfer_encoding(&parser_, 1);
    if (flags & kLenientVersion)
      llhttp_set_lenient_version(&parser_, 1);
    if (flags & kLenientDataAfterClose)
      llhttp_set_lenient_data_after_close(&parser_, 1);
    if (flags & kLenientOptionalLFAfterCR)
      llhttp_set_lenient_optional_lf_after_cr(&parser_, 1);
    if (flags & kLenientOptionalCRLFAfterChunk)
      llhttp_set_lenient_optional_crlf_after_chunk(&parser_, 1);
    if (flags & kLenientOptionalCRBeforeLF)
      llhttp_set_lenient_optional_cr_before_lf(&parser_, 1);
    if (flags & kLenientSpacesAfterChunkSize)
      llhttp_set_lenient_spaces_after_chunk_size(&parser_, 1);
  }

  // NAPI helpers.
  static napi_value createString(napi_env env, const char *str) {
    napi_value val;
    napi_create_string_utf8(env, str, NAPI_AUTO_LENGTH, &val);
    return val;
  }

  static napi_value createString(napi_env env, const std::string &str) {
    napi_value val;
    napi_create_string_utf8(env, str.c_str(), str.size(), &val);
    return val;
  }

  static napi_value createUint32(napi_env env, uint32_t v) {
    napi_value val;
    napi_create_uint32(env, v, &val);
    return val;
  }

  static void setProperty(
      napi_env env,
      napi_value obj,
      const char *name,
      napi_value value) {
    napi_set_named_property(env, obj, name, value);
  }

  static void pointerCb(napi_env /*env*/, void *data, void * /*hint*/) {
    delete static_cast<Parser *>(data);
  }

  // -------------------------------------------------------------------
  // Instance data
  // -------------------------------------------------------------------
  llhttp_t parser_;
  llhttp_settings_t settings_;

  napi_env env_ = nullptr;
  napi_ref selfRef_ = nullptr;

  // Parser state.
  uint32_t maxHttpHeaderSize_ = kDefaultMaxHttpHeaderSize;
  size_t headerNread_ = 0;
  std::string url_;
  std::string statusMessage_;
  std::string fields_[kMaxHeaderFieldsCount];
  std::string values_[kMaxHeaderFieldsCount];
  int numFields_ = 0;
  int numValues_ = 0;
  bool haveFlushed_ = false;
  bool gotException_ = false;
  bool headersCompleted_ = false;
  size_t chunkExtensionNread_ = 0;
  bool pendingPause_ = false;

  // Consume state.
  bool consumed_ = false;
  napi_ref consumedStreamRef_ = nullptr;

  // Current buffer being parsed (valid only during execute).
  const char *currentBufferData_ = nullptr;
  size_t currentBufferLen_ = 0;
};

// Strict-weak ordering: idle parsers (timestamp 0) sort before active
// ones; among active parsers, oldest timestamp first; ties broken by
// pointer. Mirrors Node's ParserComparator at node_http_parser.cc:1050.
inline bool ParserComparator::operator()(const Parser *lhs, const Parser *rhs)
    const {
  if (lhs->lastMessageStart_ == 0 && rhs->lastMessageStart_ == 0) {
    return lhs < rhs;
  } else if (lhs->lastMessageStart_ == 0) {
    return true;
  } else if (rhs->lastMessageStart_ == 0) {
    return false;
  }
  return lhs->lastMessageStart_ < rhs->lastMessageStart_;
}

inline napi_value ConnectionsList::all(napi_env env) {
  napi_value arr;
  napi_create_array(env, &arr);
  uint32_t i = 0;
  for (Parser *p : all_) {
    napi_value obj = p->getJsObject();
    if (obj) {
      napi_set_element(env, arr, i++, obj);
    }
  }
  return arr;
}

// idle = parsers in all_ whose lastMessageStart_ == 0 (i.e. between
// requests on a keep-alive connection). Walks all_ rather than
// computing all_ \ active_; the two are equivalent given the lifecycle
// invariants but iterating one set is simpler. Matches Node's Idle().
inline napi_value ConnectionsList::idle(napi_env env) {
  napi_value arr;
  napi_create_array(env, &arr);
  uint32_t i = 0;
  for (Parser *p : all_) {
    if (p->lastMessageStart_ == 0) {
      napi_value obj = p->getJsObject();
      if (obj) {
        napi_set_element(env, arr, i++, obj);
      }
    }
  }
  return arr;
}

inline napi_value ConnectionsList::active(napi_env env) {
  napi_value arr;
  napi_create_array(env, &arr);
  uint32_t i = 0;
  for (Parser *p : active_) {
    napi_value obj = p->getJsObject();
    if (obj) {
      napi_set_element(env, arr, i++, obj);
    }
  }
  return arr;
}

// Returns parsers in active_ whose lastMessageStart_ is older than the
// applicable deadline (headersTimeout while still reading headers,
// requestTimeout once headersCompleted_ is set). Expired parsers are
// removed from active_ as a side effect; the JS caller is expected to
// destroy their sockets, which eventually triggers parser.remove() to
// pop them from all_. Mirrors Node's Expired() at
// node_http_parser.cc:1124, including the headers-vs-request swap and
// the now>timeout underflow guards.
inline napi_value ConnectionsList::expired(
    napi_env env,
    uint32_t headersTimeoutMs,
    uint32_t requestTimeoutMs) {
  napi_value arr;
  napi_create_array(env, &arr);

  uint64_t headersTimeoutNs =
      static_cast<uint64_t>(headersTimeoutMs) * 1000000ULL;
  uint64_t requestTimeoutNs =
      static_cast<uint64_t>(requestTimeoutMs) * 1000000ULL;

  if (headersTimeoutNs == 0 && requestTimeoutNs == 0) {
    return arr;
  }
  if (requestTimeoutNs > 0 && headersTimeoutNs > requestTimeoutNs) {
    std::swap(headersTimeoutNs, requestTimeoutNs);
  }

  uint64_t now = uv_hrtime();
  uint64_t headersDeadline = (headersTimeoutNs > 0 && now > headersTimeoutNs)
      ? now - headersTimeoutNs
      : 0;
  uint64_t requestDeadline = (requestTimeoutNs > 0 && now > requestTimeoutNs)
      ? now - requestTimeoutNs
      : 0;

  if (headersDeadline == 0 && requestDeadline == 0) {
    return arr;
  }

  uint32_t i = 0;
  for (auto it = active_.begin(); it != active_.end();) {
    Parser *p = *it;
    bool isExpired = (!p->headersCompleted_ && headersDeadline > 0 &&
                      p->lastMessageStart_ < headersDeadline) ||
        (requestDeadline > 0 && p->lastMessageStart_ < requestDeadline);
    if (isExpired) {
      napi_value obj = p->getJsObject();
      if (obj) {
        napi_set_element(env, arr, i++, obj);
      }
      it = active_.erase(it);
    } else {
      ++it;
    }
  }
  return arr;
}

// ============================================================================
// NAPI method implementations
// ============================================================================

// ---------- HTTPParser constructor ----------
static napi_value ParserNew(napi_env env, napi_callback_info info) {
  napi_value jsThis;
  napi_get_cb_info(env, info, nullptr, nullptr, &jsThis, nullptr);

  auto *parser = new Parser();
  parser->init(env, jsThis);
  return jsThis;
}

// ---------- HTTPParser.prototype.initialize ----------
static napi_value ParserInitialize(napi_env env, napi_callback_info info) {
  size_t argc = 5;
  napi_value args[5];
  napi_value jsThis;
  napi_get_cb_info(env, info, &argc, args, &jsThis, nullptr);

  Parser *parser = Parser::unwrap(env, jsThis);
  if (!parser)
    return nullptr;

  // args[0] = type (REQUEST=1 or RESPONSE=2)
  int32_t type = 0;
  napi_get_value_int32(env, args[0], &type);

  // args[1] = asyncResource (unused — no async_hooks support)

  // args[2] = maxHttpHeaderSize (optional, 0 means use default)
  uint32_t maxHeaderSize = kDefaultMaxHttpHeaderSize;
  if (argc > 2) {
    napi_valuetype vt;
    napi_typeof(env, args[2], &vt);
    if (vt == napi_number) {
      uint32_t val = 0;
      napi_get_value_uint32(env, args[2], &val);
      if (val > 0)
        maxHeaderSize = val;
    }
  }

  // args[3] = lenientFlags (optional)
  int32_t lenientFlags = kLenientNone;
  if (argc > 3) {
    napi_valuetype vt;
    napi_typeof(env, args[3], &vt);
    if (vt == napi_number) {
      napi_get_value_int32(env, args[3], &lenientFlags);
    }
  }

  parser->initialize(type, maxHeaderSize, lenientFlags);

  // args[4] = optional ConnectionsList. If provided, register this parser
  // with the list so that closeAllConnections / closeIdleConnections /
  // checkConnections (header/request timeouts) can find it. The server
  // keeps the list alive, so we store a borrowed pointer.
  //
  // Set lastMessageStart_ before Push so the comparator orders it as
  // active (not idle). This also serves as a DoS guard: a connection
  // that never sends any bytes still has a deadline, matching Node's
  // behavior at node_http_parser.cc:687.
  parser->connectionsList_ = nullptr;
  parser->lastMessageStart_ = 0;
  if (argc > 4) {
    napi_valuetype vt;
    napi_typeof(env, args[4], &vt);
    if (vt == napi_object) {
      auto *list = ConnectionsList::unwrap(env, args[4]);
      if (list) {
        parser->connectionsList_ = list;
        parser->lastMessageStart_ = uv_hrtime();
        list->push(parser);
        list->pushActive(parser);
      }
    }
  }

  napi_value undef;
  napi_get_undefined(env, &undef);
  return undef;
}

// ---------- HTTPParser.prototype.execute ----------
static napi_value ParserExecute(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value args[1];
  napi_value jsThis;
  napi_get_cb_info(env, info, &argc, args, &jsThis, nullptr);

  Parser *parser = Parser::unwrap(env, jsThis);
  if (!parser)
    return nullptr;

  // args[0] = Buffer (ArrayBufferView)
  void *data;
  size_t len;
  napi_value arrBuf;
  size_t offset;
  napi_get_typedarray_info(
      env, args[0], nullptr, &len, &data, &arrBuf, &offset);

  return parser->execute(env, static_cast<const char *>(data), len);
}

// ---------- HTTPParser.prototype.finish ----------
static napi_value ParserFinish(napi_env env, napi_callback_info info) {
  napi_value jsThis;
  napi_get_cb_info(env, info, nullptr, nullptr, &jsThis, nullptr);

  Parser *parser = Parser::unwrap(env, jsThis);
  if (!parser)
    return nullptr;

  return parser->finish(env);
}

// ---------- HTTPParser.prototype.close ----------
static napi_value ParserClose(napi_env env, napi_callback_info info) {
  napi_value jsThis;
  napi_get_cb_info(env, info, nullptr, nullptr, &jsThis, nullptr);

  // Remove the wrap so the parser is deleted.
  void *data;
  napi_remove_wrap(env, jsThis, &data);
  delete static_cast<Parser *>(data);

  napi_value undef;
  napi_get_undefined(env, &undef);
  return undef;
}

// ---------- HTTPParser.prototype.free ----------
static napi_value ParserFree(napi_env env, napi_callback_info info) {
  // In Node, this triggers async_hooks destroy events.
  // We have no async_hooks, so this is a no-op.
  napi_value undef;
  napi_get_undefined(env, &undef);
  return undef;
}

// ---------- HTTPParser.prototype.remove ----------
static napi_value ParserRemove(napi_env env, napi_callback_info info) {
  napi_value jsThis;
  napi_get_cb_info(env, info, nullptr, nullptr, &jsThis, nullptr);

  Parser *parser = Parser::unwrap(env, jsThis);
  if (parser && parser->connectionsList_) {
    parser->connectionsList_->pop(parser);
    parser->connectionsList_->popActive(parser);
    parser->connectionsList_ = nullptr;
  }

  napi_value undef;
  napi_get_undefined(env, &undef);
  return undef;
}

// ---------- HTTPParser.prototype.pause ----------
static napi_value ParserPause(napi_env env, napi_callback_info info) {
  napi_value jsThis;
  napi_get_cb_info(env, info, nullptr, nullptr, &jsThis, nullptr);

  Parser *parser = Parser::unwrap(env, jsThis);
  if (parser)
    parser->pause();

  napi_value undef;
  napi_get_undefined(env, &undef);
  return undef;
}

// ---------- HTTPParser.prototype.resume ----------
static napi_value ParserResume(napi_env env, napi_callback_info info) {
  napi_value jsThis;
  napi_get_cb_info(env, info, nullptr, nullptr, &jsThis, nullptr);

  Parser *parser = Parser::unwrap(env, jsThis);
  if (parser)
    parser->resume();

  napi_value undef;
  napi_get_undefined(env, &undef);
  return undef;
}

// ---------- HTTPParser.prototype.consume ----------
static napi_value ParserConsume(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value args[1];
  napi_value jsThis;
  napi_get_cb_info(env, info, &argc, args, &jsThis, nullptr);

  Parser *parser = Parser::unwrap(env, jsThis);
  if (parser && argc >= 1)
    parser->consume(env, args[0]);

  napi_value undef;
  napi_get_undefined(env, &undef);
  return undef;
}

// ---------- HTTPParser.prototype.unconsume ----------
static napi_value ParserUnconsume(napi_env env, napi_callback_info info) {
  napi_value jsThis;
  napi_get_cb_info(env, info, nullptr, nullptr, &jsThis, nullptr);

  Parser *parser = Parser::unwrap(env, jsThis);
  if (parser)
    parser->unconsume(env);

  napi_value undef;
  napi_get_undefined(env, &undef);
  return undef;
}

// ---------- HTTPParser.prototype.getCurrentBuffer ----------
static napi_value ParserGetCurrentBuffer(
    napi_env env,
    napi_callback_info info) {
  napi_value jsThis;
  napi_get_cb_info(env, info, nullptr, nullptr, &jsThis, nullptr);

  Parser *parser = Parser::unwrap(env, jsThis);
  if (!parser)
    return nullptr;

  return parser->getCurrentBuffer(env);
}

// ---------- ConnectionsList constructor ----------
static napi_value ConnectionsListNew(napi_env env, napi_callback_info info) {
  napi_value jsThis;
  napi_get_cb_info(env, info, nullptr, nullptr, &jsThis, nullptr);

  auto *list = new ConnectionsList();
  list->init(env, jsThis);
  return jsThis;
}

// ---------- ConnectionsList.prototype.all ----------
static napi_value ConnectionsListAll(napi_env env, napi_callback_info info) {
  napi_value jsThis;
  napi_get_cb_info(env, info, nullptr, nullptr, &jsThis, nullptr);
  auto *list = ConnectionsList::unwrap(env, jsThis);
  return list ? list->all(env) : nullptr;
}

// ---------- ConnectionsList.prototype.idle ----------
static napi_value ConnectionsListIdle(napi_env env, napi_callback_info info) {
  napi_value jsThis;
  napi_get_cb_info(env, info, nullptr, nullptr, &jsThis, nullptr);
  auto *list = ConnectionsList::unwrap(env, jsThis);
  return list ? list->idle(env) : nullptr;
}

// ---------- ConnectionsList.prototype.active ----------
static napi_value ConnectionsListActive(napi_env env, napi_callback_info info) {
  napi_value jsThis;
  napi_get_cb_info(env, info, nullptr, nullptr, &jsThis, nullptr);
  auto *list = ConnectionsList::unwrap(env, jsThis);
  return list ? list->active(env) : nullptr;
}

// ---------- ConnectionsList.prototype.expired ----------
static napi_value ConnectionsListExpired(
    napi_env env,
    napi_callback_info info) {
  size_t argc = 2;
  napi_value args[2];
  napi_value jsThis;
  napi_get_cb_info(env, info, &argc, args, &jsThis, nullptr);

  auto *list = ConnectionsList::unwrap(env, jsThis);
  if (!list)
    return nullptr;

  // JS-side timeouts are non-negative integer milliseconds (Node uses
  // Uint32::Value()). Convert to nanoseconds inside expired().
  uint32_t headersTimeout = 0, requestTimeout = 0;
  if (argc > 0)
    napi_get_value_uint32(env, args[0], &headersTimeout);
  if (argc > 1)
    napi_get_value_uint32(env, args[1], &requestTimeout);

  return list->expired(env, headersTimeout, requestTimeout);
}

// ============================================================================
// Binding init
// ============================================================================

napi_value initHttpParserBinding(napi_env env, napi_value exports) {
  napi_value undefined;
  napi_get_undefined(env, &undefined);

  // --- HTTPParser constructor ---
  napi_property_descriptor parserMethods[] = {
      {"initialize",
       nullptr,
       ParserInitialize,
       nullptr,
       nullptr,
       nullptr,
       napi_default,
       nullptr},
      {"execute",
       nullptr,
       ParserExecute,
       nullptr,
       nullptr,
       nullptr,
       napi_default,
       nullptr},
      {"finish",
       nullptr,
       ParserFinish,
       nullptr,
       nullptr,
       nullptr,
       napi_default,
       nullptr},
      {"close",
       nullptr,
       ParserClose,
       nullptr,
       nullptr,
       nullptr,
       napi_default,
       nullptr},
      {"free",
       nullptr,
       ParserFree,
       nullptr,
       nullptr,
       nullptr,
       napi_default,
       nullptr},
      {"remove",
       nullptr,
       ParserRemove,
       nullptr,
       nullptr,
       nullptr,
       napi_default,
       nullptr},
      {"pause",
       nullptr,
       ParserPause,
       nullptr,
       nullptr,
       nullptr,
       napi_default,
       nullptr},
      {"resume",
       nullptr,
       ParserResume,
       nullptr,
       nullptr,
       nullptr,
       napi_default,
       nullptr},
      {"consume",
       nullptr,
       ParserConsume,
       nullptr,
       nullptr,
       nullptr,
       napi_default,
       nullptr},
      {"unconsume",
       nullptr,
       ParserUnconsume,
       nullptr,
       nullptr,
       nullptr,
       napi_default,
       nullptr},
      {"getCurrentBuffer",
       nullptr,
       ParserGetCurrentBuffer,
       nullptr,
       nullptr,
       nullptr,
       napi_default,
       nullptr},
  };

  napi_value parserCtor;
  napi_define_class(
      env,
      "HTTPParser",
      NAPI_AUTO_LENGTH,
      ParserNew,
      nullptr,
      sizeof(parserMethods) / sizeof(parserMethods[0]),
      parserMethods,
      &parserCtor);

  // Set constants on HTTPParser constructor.
  auto setConst = [&](const char *name, int32_t val) {
    napi_value v;
    napi_create_int32(env, val, &v);
    napi_set_named_property(env, parserCtor, name, v);
  };

  setConst("REQUEST", HTTP_REQUEST);
  setConst("RESPONSE", HTTP_RESPONSE);
  setConst("kOnMessageBegin", kOnMessageBegin);
  setConst("kOnHeaders", kOnHeaders);
  setConst("kOnHeadersComplete", kOnHeadersComplete);
  setConst("kOnBody", kOnBody);
  setConst("kOnMessageComplete", kOnMessageComplete);
  setConst("kOnExecute", kOnExecute);
  setConst("kOnTimeout", kOnTimeout);
  setConst("kLenientNone", kLenientNone);
  setConst("kLenientHeaders", kLenientHeaders);
  setConst("kLenientChunkedLength", kLenientChunkedLength);
  setConst("kLenientKeepAlive", kLenientKeepAlive);
  setConst("kLenientTransferEncoding", kLenientTransferEncoding);
  setConst("kLenientVersion", kLenientVersion);
  setConst("kLenientDataAfterClose", kLenientDataAfterClose);
  setConst("kLenientOptionalLFAfterCR", kLenientOptionalLFAfterCR);
  setConst("kLenientOptionalCRLFAfterChunk", kLenientOptionalCRLFAfterChunk);
  setConst("kLenientOptionalCRBeforeLF", kLenientOptionalCRBeforeLF);
  setConst("kLenientSpacesAfterChunkSize", kLenientSpacesAfterChunkSize);
  setConst("kLenientAll", kLenientAll);

  napi_set_named_property(env, exports, "HTTPParser", parserCtor);

  // --- methods array ---
  napi_value methodsArr;
  napi_create_array_with_length(env, kMethodCount, &methodsArr);
  for (int i = 0; i < kMethodCount; i++) {
    napi_value s;
    napi_create_string_utf8(env, kMethodNames[i], NAPI_AUTO_LENGTH, &s);
    napi_set_element(env, methodsArr, i, s);
  }
  napi_set_named_property(env, exports, "methods", methodsArr);

  // --- allMethods array ---
  napi_value allMethodsArr;
  napi_create_array_with_length(env, kAllMethodCount, &allMethodsArr);
  for (int i = 0; i < kAllMethodCount; i++) {
    napi_value s;
    napi_create_string_utf8(env, kAllMethodNames[i], NAPI_AUTO_LENGTH, &s);
    napi_set_element(env, allMethodsArr, i, s);
  }
  napi_set_named_property(env, exports, "allMethods", allMethodsArr);

  // --- ConnectionsList constructor ---
  napi_property_descriptor connMethods[] = {
      {"all",
       nullptr,
       ConnectionsListAll,
       nullptr,
       nullptr,
       nullptr,
       napi_default,
       nullptr},
      {"idle",
       nullptr,
       ConnectionsListIdle,
       nullptr,
       nullptr,
       nullptr,
       napi_default,
       nullptr},
      {"active",
       nullptr,
       ConnectionsListActive,
       nullptr,
       nullptr,
       nullptr,
       napi_default,
       nullptr},
      {"expired",
       nullptr,
       ConnectionsListExpired,
       nullptr,
       nullptr,
       nullptr,
       napi_default,
       nullptr},
  };

  napi_value connListCtor;
  napi_define_class(
      env,
      "ConnectionsList",
      NAPI_AUTO_LENGTH,
      ConnectionsListNew,
      nullptr,
      sizeof(connMethods) / sizeof(connMethods[0]),
      connMethods,
      &connListCtor);
  napi_set_named_property(env, exports, "ConnectionsList", connListCtor);

  return exports;
}

} // namespace node_compat
} // namespace hermes
