/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_NODE_COMPAT_BINDINGS_LIBUV_STREAM_BASE_H
#define HERMES_NODE_COMPAT_BINDINGS_LIBUV_STREAM_BASE_H

#include <hermes/node-compat/bindings/handle_wrap_base.h>

#include <cstdint>
#include <sys/types.h> // ssize_t

struct uv_stream_s;
typedef struct uv_stream_s uv_stream_t;
struct uv_write_s;
typedef struct uv_write_s uv_write_t;
struct uv_shutdown_s;
typedef struct uv_shutdown_s uv_shutdown_t;
struct uv_buf_t;

namespace hermes {
namespace node_compat {

/// Base class for libuv stream handle wraps (TCP, Pipe, TTY).
/// Extends HandleWrapBase with read/write/shutdown operations.
///
/// The JS side expects these methods on stream handles:
///   readStart(), readStop()
///   writeBuffer(req, buffer)
///   writeUtf8String(req, string)
///   writeLatin1String(req, string)
///   writeAsciiString(req, string)
///   writeUcs2String(req, string)
///   writev(req, chunks, allBuffers)
///   shutdown(req)
///   getWriteQueueSize() [getter]
///   setBlocking(enable)
///
/// The shared streamBaseState Int32Array is updated by these methods
/// to communicate results back to JS.
class LibuvStreamBase : public HandleWrapBase {
 public:
  LibuvStreamBase();

  uv_stream_t *stream() const {
    return stream_;
  }

  /// Initialize the stream wrap. Call after uv_*_init().
  void initStream(
      napi_env env,
      napi_value jsObj,
      uv_stream_t *stream);

  /// Add stream methods (readStart, readStop, write*, shutdown, etc.)
  /// to a JS prototype. Also adds HandleWrap methods.
  static void addStreamMethods(napi_env env, napi_value prototype);

  /// Set the shared streamBaseState pointer (from stream_wrap binding).
  static void setStreamBaseState(int32_t *state);

  // --- JS method callbacks ---
  static napi_value readStart(napi_env env, napi_callback_info info);
  static napi_value readStop(napi_env env, napi_callback_info info);
  static napi_value writeBuffer(napi_env env, napi_callback_info info);
  static napi_value writeUtf8String(napi_env env, napi_callback_info info);
  static napi_value writeLatin1String(napi_env env, napi_callback_info info);
  static napi_value writeAsciiString(napi_env env, napi_callback_info info);
  static napi_value writeUcs2String(napi_env env, napi_callback_info info);
  static napi_value writev(napi_env env, napi_callback_info info);
  static napi_value shutdown(napi_env env, napi_callback_info info);
  static napi_value getWriteQueueSize(napi_env env, napi_callback_info info);
  static napi_value setBlocking(napi_env env, napi_callback_info info);

 private:
  // libuv callbacks
  static void onAlloc(
      uv_handle_t *handle,
      size_t suggested_size,
      uv_buf_t *buf);
  static void onRead(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf);
  static void afterWrite(uv_write_t *req, int status);
  static void afterShutdown(uv_shutdown_t *req, int status);

  // Internal write helper
  int doWrite(
      napi_value reqObj,
      uv_buf_t *bufs,
      size_t count);

  // Fire the onread callback on the JS handle object.
  void emitRead(ssize_t nread, const uv_buf_t *buf);

  uv_stream_t *stream_ = nullptr;
};

} // namespace node_compat
} // namespace hermes

#endif // HERMES_NODE_COMPAT_BINDINGS_LIBUV_STREAM_BASE_H
