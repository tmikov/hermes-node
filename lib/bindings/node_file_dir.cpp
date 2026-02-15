/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <hermes/node-compat/bindings/node_file_dir.h>
#include <node_api.h>
#include <uv.h>

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace hermes {
namespace node_compat {

// ---------------------------------------------------------------------------
// Module-level state set by the host before binding init.
// ---------------------------------------------------------------------------

static uv_loop_t *s_fsDirLoop = nullptr;

void setFsDirEventLoop(uv_loop_t *loop) {
  s_fsDirLoop = loop;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static std::string getStringArg(napi_env env, napi_value val) {
  size_t len = 0;
  napi_get_value_string_utf8(env, val, nullptr, 0, &len);
  std::string result(len, '\0');
  napi_get_value_string_utf8(env, val, &result[0], len + 1, &len);
  return result;
}

static int32_t getInt32(napi_env env, napi_value val, int32_t dflt = 0) {
  int32_t result = dflt;
  napi_get_value_int32(env, val, &result);
  return result;
}

static bool isNullOrUndefined(napi_env env, napi_value val) {
  napi_valuetype type;
  napi_typeof(env, val, &type);
  return type == napi_undefined || type == napi_null;
}

/// Create a UVException Error object (non-throwing).
static napi_value createUVException(
    napi_env env,
    int errorno,
    const char *syscall,
    const char *path = nullptr) {
  const char *code = uv_err_name(errorno);
  const char *msg = uv_strerror(errorno);

  std::string fullMsg = std::string(code) + ": " + msg + ", " + syscall;
  if (path) {
    fullMsg += " '";
    fullMsg += path;
    fullMsg += "'";
  }

  napi_value errObj;
  napi_value msgVal;
  napi_create_string_utf8(env, fullMsg.c_str(), fullMsg.size(), &msgVal);
  napi_create_error(env, nullptr, msgVal, &errObj);

  napi_value val;
  napi_create_int32(env, errorno, &val);
  napi_set_named_property(env, errObj, "errno", val);

  napi_create_string_utf8(env, code, NAPI_AUTO_LENGTH, &val);
  napi_set_named_property(env, errObj, "code", val);

  napi_create_string_utf8(env, syscall, NAPI_AUTO_LENGTH, &val);
  napi_set_named_property(env, errObj, "syscall", val);

  if (path) {
    napi_create_string_utf8(env, path, NAPI_AUTO_LENGTH, &val);
    napi_set_named_property(env, errObj, "path", val);
  }

  return errObj;
}

static napi_value throwUVException(
    napi_env env,
    int errorno,
    const char *syscall,
    const char *path = nullptr) {
  napi_value errObj = createUVException(env, errorno, syscall, path);
  napi_throw(env, errObj);
  return nullptr;
}

/// Check if a value is an FSReqCallback (has oncomplete property).
static bool isFSReqCallback(napi_env env, napi_value val) {
  napi_valuetype type;
  napi_typeof(env, val, &type);
  if (type != napi_object)
    return false;
  bool hasOncomplete = false;
  napi_has_named_property(env, val, "oncomplete", &hasOncomplete);
  return hasOncomplete;
}

// ---------------------------------------------------------------------------
// DirHandle — wraps a uv_dir_t for opendir/readdir/closedir
// ---------------------------------------------------------------------------

/// DirHandle stored as external data in a JS object via napi_wrap.
struct DirHandle {
  uv_dir_t *dir = nullptr;
  std::vector<uv_dirent_t> dirents; // Buffer for readdir results.
  bool closed = false;
};

/// Invoke the GC destructor for the DirHandle — if we reach here, the handle
/// was garbage collected without explicit close. We silently close it.
static void dirHandleGCdestructor(napi_env env, void *data, void * /*hint*/) {
  auto *handle = static_cast<DirHandle *>(data);
  if (!handle->closed && handle->dir) {
    uv_fs_t req;
    uv_fs_closedir(nullptr, &req, handle->dir, nullptr);
    uv_fs_req_cleanup(&req);
  }
  delete handle;
}

/// Build the read-result array from dirents: [name1, type1, name2, type2, ...]
/// Returns null if numEntries == 0.
static napi_value buildReadResult(
    napi_env env,
    uv_dirent_t *dirents,
    int numEntries) {
  if (numEntries == 0) {
    napi_value nullVal;
    napi_get_null(env, &nullVal);
    return nullVal;
  }

  napi_value arr;
  napi_create_array_with_length(
      env, static_cast<size_t>(numEntries) * 2, &arr);

  for (int i = 0; i < numEntries; i++) {
    napi_value name;
    napi_create_string_utf8(
        env, dirents[i].name, NAPI_AUTO_LENGTH, &name);
    napi_set_element(env, arr, static_cast<uint32_t>(i * 2), name);

    napi_value type;
    napi_create_int32(env, static_cast<int32_t>(dirents[i].type), &type);
    napi_set_element(env, arr, static_cast<uint32_t>(i * 2 + 1), type);
  }

  return arr;
}

// ---------------------------------------------------------------------------
// DirHandle methods (read / close) — called as methods on the DirHandle object
// ---------------------------------------------------------------------------

/// Async request wrapper for dir operations.
struct DirReqWrap {
  uv_fs_t req{};
  napi_env env = nullptr;
  napi_ref callbackRef = nullptr;   // Ref to FSReqCallback object
  napi_ref dirHandleRef = nullptr;  // Keep dir handle object alive
  DirHandle *dirHandle = nullptr;   // Pointer to native DirHandle
  enum class Op { Read, Close } op = Op::Read;
};

/// Common async completion callback for dir operations.
static void dirAfterAsync(uv_fs_t *req) {
  auto *wrap = static_cast<DirReqWrap *>(req->data);
  napi_env env = wrap->env;

  napi_handle_scope scope;
  napi_open_handle_scope(env, &scope);

  int result = static_cast<int>(req->result);

  // Get the FSReqCallback object and its oncomplete function.
  napi_value reqObj;
  napi_get_reference_value(env, wrap->callbackRef, &reqObj);
  napi_value oncomplete;
  napi_get_named_property(env, reqObj, "oncomplete", &oncomplete);

  if (result < 0) {
    // Error: call oncomplete(error).
    const char *syscall =
        (wrap->op == DirReqWrap::Op::Read) ? "readdir" : "closedir";
    napi_value errObj = createUVException(env, result, syscall);

    napi_value cbResult;
    napi_call_function(env, reqObj, oncomplete, 1, &errObj, &cbResult);
  } else {
    if (wrap->op == DirReqWrap::Op::Read) {
      // Build result array from dirents.
      napi_value jsResult =
          buildReadResult(env, wrap->dirHandle->dirents.data(), result);
      napi_value args[2];
      napi_get_null(env, &args[0]); // null error
      args[1] = jsResult;

      napi_value cbResult;
      napi_call_function(env, reqObj, oncomplete, 2, args, &cbResult);
    } else {
      // Close: mark as closed, call oncomplete(null, undefined).
      wrap->dirHandle->closed = true;
      napi_value args[2];
      napi_get_null(env, &args[0]);
      napi_get_undefined(env, &args[1]);

      napi_value cbResult;
      napi_call_function(env, reqObj, oncomplete, 2, args, &cbResult);
    }
  }

  // Cleanup.
  uv_fs_req_cleanup(&wrap->req);
  napi_delete_reference(env, wrap->callbackRef);
  napi_delete_reference(env, wrap->dirHandleRef);
  napi_close_handle_scope(env, scope);
  delete wrap;
}

/// dirHandle.read(encoding, bufferSize, req?)
static napi_value dirHandleRead(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3];
  napi_value thisArg;
  napi_get_cb_info(env, info, &argc, argv, &thisArg, nullptr);

  // Unwrap the DirHandle from 'this'.
  DirHandle *dirHandle = nullptr;
  napi_unwrap(env, thisArg, reinterpret_cast<void **>(&dirHandle));
  if (!dirHandle || dirHandle->closed) {
    napi_throw_error(env, nullptr, "Directory handle is closed");
    return nullptr;
  }

  // encoding is argv[0] (unused — we always return UTF-8 strings).
  int32_t bufferSize = getInt32(env, argv[1], 32);
  if (bufferSize <= 0)
    bufferSize = 32;

  // Resize the dirents buffer if needed.
  if (static_cast<size_t>(bufferSize) != dirHandle->dirents.size()) {
    dirHandle->dirents.resize(static_cast<size_t>(bufferSize));
  }
  dirHandle->dir->nentries = static_cast<size_t>(bufferSize);
  dirHandle->dir->dirents = dirHandle->dirents.data();

  // Check for async req argument.
  napi_value asyncReq = nullptr;
  if (argc > 2 && !isNullOrUndefined(env, argv[2]) &&
      isFSReqCallback(env, argv[2])) {
    asyncReq = argv[2];
  }

  if (asyncReq) {
    // Async read.
    if (!s_fsDirLoop) {
      napi_throw_error(
          env, nullptr, "Event loop not set for async fs_dir operations");
      return nullptr;
    }

    auto *wrap = new DirReqWrap();
    wrap->env = env;
    wrap->dirHandle = dirHandle;
    wrap->op = DirReqWrap::Op::Read;
    wrap->req.data = wrap;

    napi_create_reference(env, asyncReq, 1, &wrap->callbackRef);
    napi_create_reference(env, thisArg, 1, &wrap->dirHandleRef);

    int err = uv_fs_readdir(
        s_fsDirLoop, &wrap->req, dirHandle->dir, dirAfterAsync);
    if (err < 0) {
      napi_delete_reference(env, wrap->callbackRef);
      napi_delete_reference(env, wrap->dirHandleRef);
      delete wrap;
      return throwUVException(env, err, "readdir");
    }

    napi_value undef;
    napi_get_undefined(env, &undef);
    return undef;
  }

  // Sync read.
  uv_fs_t req;
  int result = uv_fs_readdir(nullptr, &req, dirHandle->dir, nullptr);

  if (result < 0) {
    uv_fs_req_cleanup(&req);
    return throwUVException(env, result, "readdir");
  }

  // Build result BEFORE cleanup — dirent names point to req-owned memory.
  napi_value jsResult = buildReadResult(env, dirHandle->dirents.data(), result);
  uv_fs_req_cleanup(&req);
  return jsResult;
}

/// dirHandle.close(req?)
static napi_value dirHandleClose(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_value thisArg;
  napi_get_cb_info(env, info, &argc, argv, &thisArg, nullptr);

  DirHandle *dirHandle = nullptr;
  napi_unwrap(env, thisArg, reinterpret_cast<void **>(&dirHandle));
  if (!dirHandle || dirHandle->closed) {
    napi_value undef;
    napi_get_undefined(env, &undef);
    return undef;
  }

  // Check for async req argument.
  napi_value asyncReq = nullptr;
  if (argc > 0 && !isNullOrUndefined(env, argv[0]) &&
      isFSReqCallback(env, argv[0])) {
    asyncReq = argv[0];
  }

  if (asyncReq) {
    // Async close.
    if (!s_fsDirLoop) {
      napi_throw_error(
          env, nullptr, "Event loop not set for async fs_dir operations");
      return nullptr;
    }

    auto *wrap = new DirReqWrap();
    wrap->env = env;
    wrap->dirHandle = dirHandle;
    wrap->op = DirReqWrap::Op::Close;
    wrap->req.data = wrap;

    napi_create_reference(env, asyncReq, 1, &wrap->callbackRef);
    napi_create_reference(env, thisArg, 1, &wrap->dirHandleRef);

    int err = uv_fs_closedir(
        s_fsDirLoop, &wrap->req, dirHandle->dir, dirAfterAsync);
    if (err < 0) {
      napi_delete_reference(env, wrap->callbackRef);
      napi_delete_reference(env, wrap->dirHandleRef);
      delete wrap;
      return throwUVException(env, err, "closedir");
    }

    napi_value undef;
    napi_get_undefined(env, &undef);
    return undef;
  }

  // Sync close.
  uv_fs_t req;
  int result = uv_fs_closedir(nullptr, &req, dirHandle->dir, nullptr);
  uv_fs_req_cleanup(&req);

  dirHandle->closed = true;

  if (result < 0)
    return throwUVException(env, result, "closedir");

  napi_value undef;
  napi_get_undefined(env, &undef);
  return undef;
}

// ---------------------------------------------------------------------------
// Create a DirHandle JS object from a uv_dir_t.
// ---------------------------------------------------------------------------

static napi_value createDirHandleObject(napi_env env, uv_dir_t *dir) {
  auto *handle = new DirHandle();
  handle->dir = dir;

  napi_value obj;
  napi_create_object(env, &obj);

  // Wrap the native DirHandle into the JS object.
  napi_wrap(env, obj, handle, dirHandleGCdestructor, nullptr, nullptr);

  // Add read() and close() methods.
  napi_value readFn;
  napi_create_function(
      env, "read", NAPI_AUTO_LENGTH, dirHandleRead, nullptr, &readFn);
  napi_set_named_property(env, obj, "read", readFn);

  napi_value closeFn;
  napi_create_function(
      env, "close", NAPI_AUTO_LENGTH, dirHandleClose, nullptr, &closeFn);
  napi_set_named_property(env, obj, "close", closeFn);

  return obj;
}

// ---------------------------------------------------------------------------
// Async opendir completion
// ---------------------------------------------------------------------------

struct OpendirReqWrap {
  uv_fs_t req{};
  napi_env env = nullptr;
  napi_ref callbackRef = nullptr;
  std::string path;
};

static void opendirAfterAsync(uv_fs_t *req) {
  auto *wrap = static_cast<OpendirReqWrap *>(req->data);
  napi_env env = wrap->env;

  napi_handle_scope scope;
  napi_open_handle_scope(env, &scope);

  napi_value reqObj;
  napi_get_reference_value(env, wrap->callbackRef, &reqObj);
  napi_value oncomplete;
  napi_get_named_property(env, reqObj, "oncomplete", &oncomplete);

  int result = static_cast<int>(req->result);

  if (result < 0) {
    napi_value errObj =
        createUVException(env, result, "opendir", wrap->path.c_str());
    napi_value cbResult;
    napi_call_function(env, reqObj, oncomplete, 1, &errObj, &cbResult);
  } else {
    auto *dir = static_cast<uv_dir_t *>(req->ptr);
    napi_value handleObj = createDirHandleObject(env, dir);

    napi_value args[2];
    napi_get_null(env, &args[0]);
    args[1] = handleObj;

    napi_value cbResult;
    napi_call_function(env, reqObj, oncomplete, 2, args, &cbResult);
  }

  uv_fs_req_cleanup(&wrap->req);
  napi_delete_reference(env, wrap->callbackRef);
  napi_close_handle_scope(env, scope);
  delete wrap;
}

// ---------------------------------------------------------------------------
// Binding functions: opendir, opendirSync
// ---------------------------------------------------------------------------

/// opendir(path, encoding, req?)
static napi_value fsOpendir(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  std::string path = getStringArg(env, argv[0]);
  // encoding is argv[1] (unused — we always return UTF-8 strings).

  // Check for async req argument at position 2.
  napi_value asyncReq = nullptr;
  if (argc > 2 && !isNullOrUndefined(env, argv[2]) &&
      isFSReqCallback(env, argv[2])) {
    asyncReq = argv[2];
  }

  if (asyncReq) {
    // Async opendir.
    if (!s_fsDirLoop) {
      napi_throw_error(
          env, nullptr, "Event loop not set for async fs_dir operations");
      return nullptr;
    }

    auto *wrap = new OpendirReqWrap();
    wrap->env = env;
    wrap->path = path;
    wrap->req.data = wrap;

    napi_create_reference(env, asyncReq, 1, &wrap->callbackRef);

    int err = uv_fs_opendir(
        s_fsDirLoop, &wrap->req, path.c_str(), opendirAfterAsync);
    if (err < 0) {
      napi_delete_reference(env, wrap->callbackRef);
      delete wrap;
      return throwUVException(env, err, "opendir", path.c_str());
    }

    napi_value undef;
    napi_get_undefined(env, &undef);
    return undef;
  }

  // Sync opendir (used by readSyncRecursive in dir.js).
  uv_fs_t req;
  int result = uv_fs_opendir(nullptr, &req, path.c_str(), nullptr);
  if (result < 0) {
    uv_fs_req_cleanup(&req);
    return throwUVException(env, result, "opendir", path.c_str());
  }

  auto *dir = static_cast<uv_dir_t *>(req.ptr);
  uv_fs_req_cleanup(&req);

  return createDirHandleObject(env, dir);
}

/// opendirSync(path)
static napi_value fsOpendirSync(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  std::string path = getStringArg(env, argv[0]);

  uv_fs_t req;
  int result = uv_fs_opendir(nullptr, &req, path.c_str(), nullptr);
  if (result < 0) {
    uv_fs_req_cleanup(&req);
    return throwUVException(env, result, "opendir", path.c_str());
  }

  auto *dir = static_cast<uv_dir_t *>(req.ptr);
  uv_fs_req_cleanup(&req);

  return createDirHandleObject(env, dir);
}

// ---------------------------------------------------------------------------
// Binding init
// ---------------------------------------------------------------------------

napi_value initFsDirBinding(napi_env env, napi_value exports) {
  napi_value fnVal;

  napi_create_function(
      env, "opendir", NAPI_AUTO_LENGTH, fsOpendir, nullptr, &fnVal);
  napi_set_named_property(env, exports, "opendir", fnVal);

  napi_create_function(
      env, "opendirSync", NAPI_AUTO_LENGTH, fsOpendirSync, nullptr, &fnVal);
  napi_set_named_property(env, exports, "opendirSync", fnVal);

  return exports;
}

} // namespace node_compat
} // namespace hermes
