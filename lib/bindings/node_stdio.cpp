/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <hermes/node-compat/bindings/node_stdio.h>
#include <node_api.h>

#include <uv.h>

#include <cstring>
#include <string>

namespace hermes {
namespace node_compat {

/// writeString(fd, string) — synchronous write of a UTF-8 string to fd.
/// Returns the number of bytes written.
static napi_value writeString(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  if (argc < 2) {
    napi_throw_type_error(env, nullptr, "writeString requires (fd, string)");
    return nullptr;
  }

  int32_t fd;
  napi_get_value_int32(env, argv[0], &fd);

  // Get string as UTF-8.
  size_t len = 0;
  napi_get_value_string_utf8(env, argv[1], nullptr, 0, &len);
  std::string buf(len, '\0');
  napi_get_value_string_utf8(env, argv[1], &buf[0], len + 1, &len);

  // Synchronous write via libuv.
  uv_buf_t uvBuf = uv_buf_init(&buf[0], static_cast<unsigned int>(len));
  uv_fs_t req;
  int result = uv_fs_write(nullptr, &req, fd, &uvBuf, 1, -1, nullptr);
  uv_fs_req_cleanup(&req);

  if (result < 0) {
    std::string msg = "write failed: ";
    msg += uv_strerror(result);
    napi_throw_error(env, uv_err_name(result), msg.c_str());
    return nullptr;
  }

  napi_value ret;
  napi_create_int32(env, result, &ret);
  return ret;
}

/// writeBuffer(fd, buffer) — synchronous write of a Uint8Array/Buffer to fd.
/// Returns the number of bytes written.
static napi_value writeBuffer(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  if (argc < 2) {
    napi_throw_type_error(env, nullptr, "writeBuffer requires (fd, buffer)");
    return nullptr;
  }

  int32_t fd;
  napi_get_value_int32(env, argv[0], &fd);

  // Get the typed array data.
  void *data = nullptr;
  size_t byteLength = 0;
  napi_typedarray_type type;
  size_t byteOffset = 0;
  napi_value arrayBuf;
  napi_status st =
      napi_get_typedarray_info(env, argv[1], &type, &byteLength, &data,
                               &arrayBuf, &byteOffset);
  if (st != napi_ok) {
    // Try as ArrayBuffer.
    st = napi_get_arraybuffer_info(env, argv[1], &data, &byteLength);
    if (st != napi_ok) {
      napi_throw_type_error(env, nullptr, "second argument must be a buffer");
      return nullptr;
    }
  }

  if (byteLength == 0) {
    napi_value ret;
    napi_create_int32(env, 0, &ret);
    return ret;
  }

  uv_buf_t uvBuf = uv_buf_init(
      static_cast<char *>(data), static_cast<unsigned int>(byteLength));
  uv_fs_t req;
  int result = uv_fs_write(nullptr, &req, fd, &uvBuf, 1, -1, nullptr);
  uv_fs_req_cleanup(&req);

  if (result < 0) {
    std::string msg = "write failed: ";
    msg += uv_strerror(result);
    napi_throw_error(env, uv_err_name(result), msg.c_str());
    return nullptr;
  }

  napi_value ret;
  napi_create_int32(env, result, &ret);
  return ret;
}

/// getHandleType(fd) — returns a string describing the handle type.
/// Possible values: "TCP", "TTY", "UDP", "FILE", "PIPE", "UNKNOWN".
static napi_value getHandleType(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  if (argc < 1) {
    napi_throw_type_error(env, nullptr, "getHandleType requires (fd)");
    return nullptr;
  }

  int32_t fd;
  napi_get_value_int32(env, argv[0], &fd);

  uv_handle_type type = uv_guess_handle(fd);
  const char *typeName;
  switch (type) {
    case UV_TCP:
      typeName = "TCP";
      break;
    case UV_TTY:
      typeName = "TTY";
      break;
    case UV_UDP:
      typeName = "UDP";
      break;
    case UV_FILE:
      typeName = "FILE";
      break;
    case UV_NAMED_PIPE:
      typeName = "PIPE";
      break;
    default:
      typeName = "UNKNOWN";
      break;
  }

  napi_value result;
  napi_create_string_utf8(env, typeName, NAPI_AUTO_LENGTH, &result);
  return result;
}

napi_value initStdioBinding(napi_env env, napi_value exports) {
  auto setFn = [&](const char *name, napi_callback cb) {
    napi_value fn;
    napi_create_function(env, name, NAPI_AUTO_LENGTH, cb, nullptr, &fn);
    napi_set_named_property(env, exports, name, fn);
  };

  setFn("writeString", writeString);
  setFn("writeBuffer", writeBuffer);
  setFn("getHandleType", getHandleType);

  return exports;
}

} // namespace node_compat
} // namespace hermes
