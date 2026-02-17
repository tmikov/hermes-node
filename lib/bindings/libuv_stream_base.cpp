/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <hermes/node-compat/bindings/libuv_stream_base.h>
#include <node_api.h>
#include <uv.h>

#include <cassert>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace hermes {
namespace node_compat {

// StreamBaseStateFields from Node's stream_base.h — must match
// the constants exported by initStreamWrapBinding.
enum StreamBaseStateFields {
  kReadBytesOrError,
  kArrayBufferOffset,
  kBytesWritten,
  kLastWriteWasAsync,
};

// Shared state array pointer (set by stream_wrap binding init).
static int32_t *s_streamBaseState = nullptr;

void LibuvStreamBase::setStreamBaseState(int32_t *state) {
  s_streamBaseState = state;
}

// ---------------------------------------------------------------------------
// WriteReqData — native data attached to write request objects
// ---------------------------------------------------------------------------

struct WriteReqData {
  uv_write_t req;
  LibuvStreamBase *wrap;
  napi_env env;
  napi_ref reqRef; // prevent-GC ref to JS request object
  // Storage for string write data (freed after write completes).
  char *storage = nullptr;
  size_t storageLen = 0;
  // For writev: multiple buffers + storage
  std::vector<uv_buf_t> bufs;
  std::vector<char *> storages;

  WriteReqData() : wrap(nullptr), env(nullptr), reqRef(nullptr) {
    memset(&req, 0, sizeof(req));
  }

  ~WriteReqData() {
    free(storage);
    for (char *s : storages)
      free(s);
  }
};

// ---------------------------------------------------------------------------
// ShutdownReqData — native data attached to shutdown request objects
// ---------------------------------------------------------------------------

struct ShutdownReqData {
  uv_shutdown_t req;
  LibuvStreamBase *wrap;
  napi_env env;
  napi_ref reqRef;

  ShutdownReqData() : wrap(nullptr), env(nullptr), reqRef(nullptr) {
    memset(&req, 0, sizeof(req));
  }
};

// ---------------------------------------------------------------------------
// LibuvStreamBase
// ---------------------------------------------------------------------------

LibuvStreamBase::LibuvStreamBase() = default;

void LibuvStreamBase::initStream(
    napi_env env,
    napi_value jsObj,
    uv_stream_t *stream) {
  stream_ = stream;
  HandleWrapBase::init(env, jsObj, reinterpret_cast<uv_handle_t *>(stream));
}

// ---------------------------------------------------------------------------
// readStart / readStop
// ---------------------------------------------------------------------------

napi_value LibuvStreamBase::readStart(napi_env env, napi_callback_info info) {
  napi_value thisObj;
  napi_get_cb_info(env, info, nullptr, nullptr, &thisObj, nullptr);

  auto *wrap =
      static_cast<LibuvStreamBase *>(HandleWrapBase::unwrap(env, thisObj));
  if (!wrap || wrap->state() != kInitialized) {
    napi_value result;
    napi_create_int32(env, UV_EINVAL, &result);
    return result;
  }

  int err = uv_read_start(
      wrap->stream_, LibuvStreamBase::onAlloc, LibuvStreamBase::onRead);

  napi_value result;
  napi_create_int32(env, err, &result);
  return result;
}

napi_value LibuvStreamBase::readStop(napi_env env, napi_callback_info info) {
  napi_value thisObj;
  napi_get_cb_info(env, info, nullptr, nullptr, &thisObj, nullptr);

  auto *wrap =
      static_cast<LibuvStreamBase *>(HandleWrapBase::unwrap(env, thisObj));
  if (!wrap || wrap->state() != kInitialized) {
    napi_value result;
    napi_create_int32(env, UV_EINVAL, &result);
    return result;
  }

  int err = uv_read_stop(wrap->stream_);

  napi_value result;
  napi_create_int32(env, err, &result);
  return result;
}

// ---------------------------------------------------------------------------
// Alloc / Read callbacks
// ---------------------------------------------------------------------------

void LibuvStreamBase::onAlloc(
    uv_handle_t *handle,
    size_t suggested_size,
    uv_buf_t *buf) {
  // Allocate a buffer. We use a simple malloc here.
  // The JS side will create a FastBuffer view into the ArrayBuffer
  // we provide in the onread callback.
  size_t size = suggested_size;
  if (size > 65536)
    size = 65536;
  buf->base = static_cast<char *>(malloc(size));
  buf->len = buf->base ? size : 0;
}

void LibuvStreamBase::onRead(
    uv_stream_t *stream,
    ssize_t nread,
    const uv_buf_t *buf) {
  auto *wrap = static_cast<LibuvStreamBase *>(stream->data);
  if (!wrap || !wrap->env())
    goto cleanup;

  wrap->emitRead(nread, buf);

cleanup:
  // Free the buffer allocated in onAlloc.
  if (buf && buf->base)
    free(buf->base);
}

void LibuvStreamBase::emitRead(ssize_t nread, const uv_buf_t *buf) {
  napi_env env = this->env();

  napi_handle_scope scope;
  napi_open_handle_scope(env, &scope);

  // Get the JS handle object via getJsObject() (from prevent-GC ref).
  napi_value thisObj = getJsObject();
  if (!thisObj) {
    napi_close_handle_scope(env, scope);
    return;
  }

  // Track total bytes read.
  if (nread > 0) {
    bytesRead_ += static_cast<uint64_t>(nread);
  }

  // Set streamBaseState fields for the JS callback.
  if (s_streamBaseState) {
    s_streamBaseState[kReadBytesOrError] = static_cast<int32_t>(nread);
    s_streamBaseState[kArrayBufferOffset] = 0;
  }

  // Get the onread callback from the handle object.
  napi_value onread;
  napi_get_named_property(env, thisObj, "onread", &onread);

  napi_valuetype onreadType;
  napi_typeof(env, onread, &onreadType);
  if (onreadType != napi_function) {
    napi_close_handle_scope(env, scope);
    return;
  }

  // Build the argument: an ArrayBuffer wrapping the read data (or undefined).
  napi_value args[1];
  if (nread > 0 && buf && buf->base) {
    // Create an ArrayBuffer with a copy of the data.
    void *abData = nullptr;
    napi_value ab;
    napi_create_arraybuffer(env, nread, &abData, &ab);
    memcpy(abData, buf->base, nread);
    args[0] = ab;
  } else {
    napi_get_undefined(env, &args[0]);
  }

  // Call onread with the handle as `this`.
  napi_value retval;
  napi_call_function(env, thisObj, onread, 1, args, &retval);

  // Clear any pending exception.
  bool hasPending = false;
  napi_is_exception_pending(env, &hasPending);
  if (hasPending) {
    napi_value exc;
    napi_get_and_clear_last_exception(env, &exc);
  }

  napi_close_handle_scope(env, scope);
}

// ---------------------------------------------------------------------------
// Write operations
// ---------------------------------------------------------------------------

int LibuvStreamBase::doWrite(napi_value reqObj, uv_buf_t *bufs, size_t count) {
  napi_env env = this->env();

  auto *reqData = new WriteReqData();
  reqData->wrap = this;
  reqData->env = env;
  reqData->req.data = reqData;

  // Create a prevent-GC reference for the request object.
  napi_create_reference(env, reqObj, 1, &reqData->reqRef);

  int err = uv_write(&reqData->req, stream_, bufs, count, afterWrite);
  if (err != 0) {
    // Synchronous failure — clean up.
    napi_delete_reference(env, reqData->reqRef);
    delete reqData;
    if (s_streamBaseState) {
      s_streamBaseState[kLastWriteWasAsync] = 0;
      s_streamBaseState[kBytesWritten] = 0;
    }
    return err;
  }

  // Calculate total bytes.
  size_t totalBytes = 0;
  for (size_t i = 0; i < count; ++i)
    totalBytes += bufs[i].len;

  bytesWritten_ += totalBytes;

  if (s_streamBaseState) {
    s_streamBaseState[kLastWriteWasAsync] = 1;
    s_streamBaseState[kBytesWritten] = static_cast<int32_t>(totalBytes);
  }

  return 0;
}

void LibuvStreamBase::afterWrite(uv_write_t *req, int status) {
  auto *reqData = static_cast<WriteReqData *>(req->data);
  if (!reqData)
    return;

  napi_env env = reqData->env;

  napi_handle_scope scope;
  napi_open_handle_scope(env, &scope);

  // Get the JS request object.
  napi_value reqObj;
  napi_get_reference_value(env, reqData->reqRef, &reqObj);

  // Call oncomplete(status) on the request object.
  napi_value oncomplete;
  napi_get_named_property(env, reqObj, "oncomplete", &oncomplete);

  napi_valuetype oncompType;
  napi_typeof(env, oncomplete, &oncompType);
  if (oncompType == napi_function) {
    napi_value args[1];
    napi_create_int32(env, status, &args[0]);
    napi_value retval;
    napi_call_function(env, reqObj, oncomplete, 1, args, &retval);

    bool hasPending = false;
    napi_is_exception_pending(env, &hasPending);
    if (hasPending) {
      napi_value exc;
      napi_get_and_clear_last_exception(env, &exc);
    }
  }

  // Cleanup.
  napi_delete_reference(env, reqData->reqRef);
  delete reqData;

  napi_close_handle_scope(env, scope);
}

// ---------------------------------------------------------------------------
// writeBuffer(req, buffer)
// ---------------------------------------------------------------------------

napi_value LibuvStreamBase::writeBuffer(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2], thisObj;
  napi_get_cb_info(env, info, &argc, argv, &thisObj, nullptr);

  auto *wrap =
      static_cast<LibuvStreamBase *>(HandleWrapBase::unwrap(env, thisObj));
  if (!wrap || wrap->state() != kInitialized) {
    napi_value result;
    napi_create_int32(env, UV_EINVAL, &result);
    return result;
  }

  napi_value reqObj = argv[0];
  napi_value bufArg = argv[1];

  // Get buffer data.
  void *bufData = nullptr;
  size_t bufLen = 0;
  napi_get_buffer_info(env, bufArg, &bufData, &bufLen);

  uv_buf_t buf = uv_buf_init(static_cast<char *>(bufData), bufLen);
  int err = wrap->doWrite(reqObj, &buf, 1);

  napi_value result;
  napi_create_int32(env, err, &result);
  return result;
}

// ---------------------------------------------------------------------------
// writeUtf8String(req, string)
// ---------------------------------------------------------------------------

napi_value LibuvStreamBase::writeUtf8String(
    napi_env env,
    napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2], thisObj;
  napi_get_cb_info(env, info, &argc, argv, &thisObj, nullptr);

  auto *wrap =
      static_cast<LibuvStreamBase *>(HandleWrapBase::unwrap(env, thisObj));
  if (!wrap || wrap->state() != kInitialized) {
    napi_value result;
    napi_create_int32(env, UV_EINVAL, &result);
    return result;
  }

  // Get string length.
  size_t strLen = 0;
  napi_get_value_string_utf8(env, argv[1], nullptr, 0, &strLen);

  // Allocate storage and copy string.
  auto *reqData = new WriteReqData();
  reqData->storage = static_cast<char *>(malloc(strLen + 1));
  reqData->storageLen = strLen;
  napi_get_value_string_utf8(
      env, argv[1], reqData->storage, strLen + 1, &strLen);

  reqData->wrap = wrap;
  reqData->env = env;
  reqData->req.data = reqData;

  napi_create_reference(env, argv[0], 1, &reqData->reqRef);

  uv_buf_t buf = uv_buf_init(reqData->storage, strLen);
  int err = uv_write(&reqData->req, wrap->stream_, &buf, 1, afterWrite);
  if (err != 0) {
    napi_delete_reference(env, reqData->reqRef);
    delete reqData;
    if (s_streamBaseState) {
      s_streamBaseState[kLastWriteWasAsync] = 0;
      s_streamBaseState[kBytesWritten] = 0;
    }
  } else {
    wrap->bytesWritten_ += strLen;
    if (s_streamBaseState) {
      s_streamBaseState[kLastWriteWasAsync] = 1;
      s_streamBaseState[kBytesWritten] = static_cast<int32_t>(strLen);
    }
  }

  napi_value result;
  napi_create_int32(env, err, &result);
  return result;
}

// ---------------------------------------------------------------------------
// writeLatin1String(req, string) — treat as binary/latin1
// ---------------------------------------------------------------------------

napi_value LibuvStreamBase::writeLatin1String(
    napi_env env,
    napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2], thisObj;
  napi_get_cb_info(env, info, &argc, argv, &thisObj, nullptr);

  auto *wrap =
      static_cast<LibuvStreamBase *>(HandleWrapBase::unwrap(env, thisObj));
  if (!wrap || wrap->state() != kInitialized) {
    napi_value result;
    napi_create_int32(env, UV_EINVAL, &result);
    return result;
  }

  // For latin1/binary, we get the string as UTF-8 and it should be fine
  // for ASCII/latin1 strings (values 0-255 in JS become their UTF-8 encoding).
  // Node actually uses a special latin1 extraction. For now, use UTF-8.
  size_t strLen = 0;
  napi_get_value_string_utf8(env, argv[1], nullptr, 0, &strLen);

  auto *reqData = new WriteReqData();
  reqData->storage = static_cast<char *>(malloc(strLen + 1));
  reqData->storageLen = strLen;
  napi_get_value_string_utf8(
      env, argv[1], reqData->storage, strLen + 1, &strLen);

  reqData->wrap = wrap;
  reqData->env = env;
  reqData->req.data = reqData;

  napi_create_reference(env, argv[0], 1, &reqData->reqRef);

  uv_buf_t buf = uv_buf_init(reqData->storage, strLen);
  int err = uv_write(&reqData->req, wrap->stream_, &buf, 1, afterWrite);
  if (err != 0) {
    napi_delete_reference(env, reqData->reqRef);
    delete reqData;
    if (s_streamBaseState) {
      s_streamBaseState[kLastWriteWasAsync] = 0;
      s_streamBaseState[kBytesWritten] = 0;
    }
  } else {
    wrap->bytesWritten_ += strLen;
    if (s_streamBaseState) {
      s_streamBaseState[kLastWriteWasAsync] = 1;
      s_streamBaseState[kBytesWritten] = static_cast<int32_t>(strLen);
    }
  }

  napi_value result;
  napi_create_int32(env, err, &result);
  return result;
}

// ---------------------------------------------------------------------------
// writeAsciiString(req, string) — same as latin1 for our purposes
// ---------------------------------------------------------------------------

napi_value LibuvStreamBase::writeAsciiString(
    napi_env env,
    napi_callback_info info) {
  // ASCII is a subset of UTF-8, so we can use the same implementation.
  return writeLatin1String(env, info);
}

// ---------------------------------------------------------------------------
// writeUcs2String(req, string) — UCS-2/UTF-16LE
// ---------------------------------------------------------------------------

napi_value LibuvStreamBase::writeUcs2String(
    napi_env env,
    napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2], thisObj;
  napi_get_cb_info(env, info, &argc, argv, &thisObj, nullptr);

  auto *wrap =
      static_cast<LibuvStreamBase *>(HandleWrapBase::unwrap(env, thisObj));
  if (!wrap || wrap->state() != kInitialized) {
    napi_value result;
    napi_create_int32(env, UV_EINVAL, &result);
    return result;
  }

  // Get string as UTF-16.
  size_t strLen = 0;
  napi_get_value_string_utf16(env, argv[1], nullptr, 0, &strLen);

  size_t byteLen = strLen * 2; // UTF-16 = 2 bytes per code unit
  auto *reqData = new WriteReqData();
  reqData->storage = static_cast<char *>(malloc(byteLen));
  reqData->storageLen = byteLen;

  napi_get_value_string_utf16(
      env,
      argv[1],
      reinterpret_cast<char16_t *>(reqData->storage),
      strLen + 1,
      &strLen);

  reqData->wrap = wrap;
  reqData->env = env;
  reqData->req.data = reqData;

  napi_create_reference(env, argv[0], 1, &reqData->reqRef);

  uv_buf_t buf = uv_buf_init(reqData->storage, strLen * 2);
  int err = uv_write(&reqData->req, wrap->stream_, &buf, 1, afterWrite);
  if (err != 0) {
    napi_delete_reference(env, reqData->reqRef);
    delete reqData;
    if (s_streamBaseState) {
      s_streamBaseState[kLastWriteWasAsync] = 0;
      s_streamBaseState[kBytesWritten] = 0;
    }
  } else {
    wrap->bytesWritten_ += strLen * 2;
    if (s_streamBaseState) {
      s_streamBaseState[kLastWriteWasAsync] = 1;
      s_streamBaseState[kBytesWritten] = static_cast<int32_t>(strLen * 2);
    }
  }

  napi_value result;
  napi_create_int32(env, err, &result);
  return result;
}

// ---------------------------------------------------------------------------
// writev(req, chunks, allBuffers)
// ---------------------------------------------------------------------------

napi_value LibuvStreamBase::writev(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3], thisObj;
  napi_get_cb_info(env, info, &argc, argv, &thisObj, nullptr);

  auto *wrap =
      static_cast<LibuvStreamBase *>(HandleWrapBase::unwrap(env, thisObj));
  if (!wrap || wrap->state() != kInitialized) {
    napi_value result;
    napi_create_int32(env, UV_EINVAL, &result);
    return result;
  }

  napi_value reqObj = argv[0];
  napi_value chunks = argv[1];
  bool allBuffers = false;
  if (argc > 2) {
    napi_get_value_bool(env, argv[2], &allBuffers);
  }

  uint32_t chunksLen = 0;
  napi_get_array_length(env, chunks, &chunksLen);

  size_t count;
  if (allBuffers) {
    count = chunksLen;
  } else {
    count = chunksLen >> 1;
  }

  auto *reqData = new WriteReqData();
  reqData->wrap = wrap;
  reqData->env = env;
  reqData->req.data = reqData;
  reqData->bufs.resize(count);

  napi_create_reference(env, reqObj, 1, &reqData->reqRef);

  size_t totalBytes = 0;

  if (allBuffers) {
    for (size_t i = 0; i < count; ++i) {
      napi_value chunk;
      napi_get_element(env, chunks, i, &chunk);
      void *data = nullptr;
      size_t len = 0;
      napi_get_buffer_info(env, chunk, &data, &len);
      reqData->bufs[i] = uv_buf_init(static_cast<char *>(data), len);
      totalBytes += len;
    }
  } else {
    for (size_t i = 0; i < count; ++i) {
      napi_value chunk;
      napi_get_element(env, chunks, i * 2, &chunk);

      // Check if it's a buffer.
      bool isBuf = false;
      napi_is_buffer(env, chunk, &isBuf);

      if (isBuf) {
        void *data = nullptr;
        size_t len = 0;
        napi_get_buffer_info(env, chunk, &data, &len);
        reqData->bufs[i] = uv_buf_init(static_cast<char *>(data), len);
        totalBytes += len;
      } else {
        // String chunk — get as UTF-8.
        size_t strLen = 0;
        napi_get_value_string_utf8(env, chunk, nullptr, 0, &strLen);
        char *storage = static_cast<char *>(malloc(strLen + 1));
        napi_get_value_string_utf8(env, chunk, storage, strLen + 1, &strLen);
        reqData->storages.push_back(storage);
        reqData->bufs[i] = uv_buf_init(storage, strLen);
        totalBytes += strLen;
      }
    }
  }

  int err = uv_write(
      &reqData->req, wrap->stream_, reqData->bufs.data(), count, afterWrite);

  if (err != 0) {
    napi_delete_reference(env, reqData->reqRef);
    delete reqData;
    if (s_streamBaseState) {
      s_streamBaseState[kLastWriteWasAsync] = 0;
      s_streamBaseState[kBytesWritten] = 0;
    }
  } else {
    wrap->bytesWritten_ += totalBytes;
    if (s_streamBaseState) {
      s_streamBaseState[kLastWriteWasAsync] = 1;
      s_streamBaseState[kBytesWritten] = static_cast<int32_t>(totalBytes);
    }
  }

  napi_value result;
  napi_create_int32(env, err, &result);
  return result;
}

// ---------------------------------------------------------------------------
// shutdown(req)
// ---------------------------------------------------------------------------

void LibuvStreamBase::afterShutdown(uv_shutdown_t *req, int status) {
  auto *reqData = static_cast<ShutdownReqData *>(req->data);
  if (!reqData)
    return;

  napi_env env = reqData->env;

  napi_handle_scope scope;
  napi_open_handle_scope(env, &scope);

  napi_value reqObj;
  napi_get_reference_value(env, reqData->reqRef, &reqObj);

  // Call oncomplete(status) on the request object.
  napi_value oncomplete;
  napi_get_named_property(env, reqObj, "oncomplete", &oncomplete);

  napi_valuetype oncompType;
  napi_typeof(env, oncomplete, &oncompType);
  if (oncompType == napi_function) {
    napi_value args[1];
    napi_create_int32(env, status, &args[0]);
    napi_value retval;
    napi_call_function(env, reqObj, oncomplete, 1, args, &retval);

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

napi_value LibuvStreamBase::shutdown(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1], thisObj;
  napi_get_cb_info(env, info, &argc, argv, &thisObj, nullptr);

  auto *wrap =
      static_cast<LibuvStreamBase *>(HandleWrapBase::unwrap(env, thisObj));
  if (!wrap || wrap->state() != kInitialized) {
    napi_value result;
    napi_create_int32(env, UV_EINVAL, &result);
    return result;
  }

  auto *reqData = new ShutdownReqData();
  reqData->wrap = wrap;
  reqData->env = env;
  reqData->req.data = reqData;

  napi_create_reference(env, argv[0], 1, &reqData->reqRef);

  int err = uv_shutdown(&reqData->req, wrap->stream_, afterShutdown);
  if (err != 0) {
    napi_delete_reference(env, reqData->reqRef);
    delete reqData;
  }

  napi_value result;
  napi_create_int32(env, err, &result);
  return result;
}

// ---------------------------------------------------------------------------
// getWriteQueueSize / setBlocking
// ---------------------------------------------------------------------------

napi_value LibuvStreamBase::getWriteQueueSize(
    napi_env env,
    napi_callback_info info) {
  napi_value thisObj;
  napi_get_cb_info(env, info, nullptr, nullptr, &thisObj, nullptr);

  auto *wrap =
      static_cast<LibuvStreamBase *>(HandleWrapBase::unwrap(env, thisObj));
  uint32_t size = 0;
  if (wrap && wrap->stream_) {
    size = wrap->stream_->write_queue_size;
  }

  napi_value result;
  napi_create_uint32(env, size, &result);
  return result;
}

napi_value LibuvStreamBase::setBlocking(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1], thisObj;
  napi_get_cb_info(env, info, &argc, argv, &thisObj, nullptr);

  auto *wrap =
      static_cast<LibuvStreamBase *>(HandleWrapBase::unwrap(env, thisObj));
  if (!wrap || wrap->state() != kInitialized) {
    napi_value result;
    napi_create_int32(env, UV_EINVAL, &result);
    return result;
  }

  bool enable = false;
  napi_get_value_bool(env, argv[0], &enable);

  int err = uv_stream_set_blocking(wrap->stream_, enable ? 1 : 0);

  napi_value result;
  napi_create_int32(env, err, &result);
  return result;
}

// ---------------------------------------------------------------------------
// getBytesRead / getBytesWritten
// ---------------------------------------------------------------------------

napi_value LibuvStreamBase::getBytesRead(
    napi_env env,
    napi_callback_info info) {
  napi_value thisObj;
  napi_get_cb_info(env, info, nullptr, nullptr, &thisObj, nullptr);

  auto *wrap =
      static_cast<LibuvStreamBase *>(HandleWrapBase::unwrap(env, thisObj));
  double value = 0;
  if (wrap) {
    value = static_cast<double>(wrap->bytesRead_);
  }

  napi_value result;
  napi_create_double(env, value, &result);
  return result;
}

napi_value LibuvStreamBase::getBytesWritten(
    napi_env env,
    napi_callback_info info) {
  napi_value thisObj;
  napi_get_cb_info(env, info, nullptr, nullptr, &thisObj, nullptr);

  auto *wrap =
      static_cast<LibuvStreamBase *>(HandleWrapBase::unwrap(env, thisObj));
  double value = 0;
  if (wrap) {
    value = static_cast<double>(wrap->bytesWritten_);
  }

  napi_value result;
  napi_create_double(env, value, &result);
  return result;
}

// ---------------------------------------------------------------------------
// addStreamMethods
// ---------------------------------------------------------------------------

void LibuvStreamBase::addStreamMethods(napi_env env, napi_value prototype) {
  // First add HandleWrap methods (ref/unref/hasRef/close/getAsyncId).
  HandleWrapBase::addHandleWrapMethods(env, prototype);

  napi_value fn;

  napi_create_function(
      env, "readStart", NAPI_AUTO_LENGTH, readStart, nullptr, &fn);
  napi_set_named_property(env, prototype, "readStart", fn);

  napi_create_function(
      env, "readStop", NAPI_AUTO_LENGTH, readStop, nullptr, &fn);
  napi_set_named_property(env, prototype, "readStop", fn);

  napi_create_function(
      env, "writeBuffer", NAPI_AUTO_LENGTH, writeBuffer, nullptr, &fn);
  napi_set_named_property(env, prototype, "writeBuffer", fn);

  napi_create_function(
      env, "writeUtf8String", NAPI_AUTO_LENGTH, writeUtf8String, nullptr, &fn);
  napi_set_named_property(env, prototype, "writeUtf8String", fn);

  napi_create_function(
      env,
      "writeLatin1String",
      NAPI_AUTO_LENGTH,
      writeLatin1String,
      nullptr,
      &fn);
  napi_set_named_property(env, prototype, "writeLatin1String", fn);

  napi_create_function(
      env,
      "writeAsciiString",
      NAPI_AUTO_LENGTH,
      writeAsciiString,
      nullptr,
      &fn);
  napi_set_named_property(env, prototype, "writeAsciiString", fn);

  napi_create_function(
      env, "writeUcs2String", NAPI_AUTO_LENGTH, writeUcs2String, nullptr, &fn);
  napi_set_named_property(env, prototype, "writeUcs2String", fn);

  napi_create_function(env, "writev", NAPI_AUTO_LENGTH, writev, nullptr, &fn);
  napi_set_named_property(env, prototype, "writev", fn);

  napi_create_function(
      env, "shutdown", NAPI_AUTO_LENGTH, shutdown, nullptr, &fn);
  napi_set_named_property(env, prototype, "shutdown", fn);

  napi_create_function(
      env,
      "getWriteQueueSize",
      NAPI_AUTO_LENGTH,
      getWriteQueueSize,
      nullptr,
      &fn);
  napi_set_named_property(env, prototype, "getWriteQueueSize", fn);

  napi_create_function(
      env, "setBlocking", NAPI_AUTO_LENGTH, setBlocking, nullptr, &fn);
  napi_set_named_property(env, prototype, "setBlocking", fn);

  // bytesRead getter (Node exposes this as a property on the handle).
  // napi_property_descriptor getter field must be napi_callback (fn ptr).
  napi_property_descriptor bytesReadDesc = {
      "bytesRead",
      nullptr,
      nullptr,
      getBytesRead,
      nullptr,
      nullptr,
      napi_enumerable,
      nullptr};
  napi_define_properties(env, prototype, 1, &bytesReadDesc);

  // bytesWritten getter.
  napi_property_descriptor bytesWrittenDesc = {
      "bytesWritten",
      nullptr,
      nullptr,
      getBytesWritten,
      nullptr,
      nullptr,
      napi_enumerable,
      nullptr};
  napi_define_properties(env, prototype, 1, &bytesWrittenDesc);

  // isStreamBase marker.
  napi_value trueVal;
  napi_get_boolean(env, true, &trueVal);
  napi_set_named_property(env, prototype, "isStreamBase", trueVal);
}

} // namespace node_compat
} // namespace hermes
