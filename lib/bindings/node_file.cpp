/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <hermes/node-compat/bindings/node_file.h>
#include <node_api.h>
#include <uv.h>

#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace hermes {
namespace node_compat {

// ---------------------------------------------------------------------------
// Module-level state set by the host before binding init.
// ---------------------------------------------------------------------------

static uv_loop_t *s_fsLoop = nullptr;

void setFsEventLoop(uv_loop_t *loop) {
  s_fsLoop = loop;
}

// ---------------------------------------------------------------------------
// Stat field layout -- must match Node's FsStatsOffset enum
// ---------------------------------------------------------------------------

enum FsStatsOffset {
  kDev = 0,
  kMode,
  kNlink,
  kUid,
  kGid,
  kRdev,
  kBlkSize,
  kIno,
  kSize,
  kBlocks,
  kATimeSec,
  kATimeNsec,
  kMTimeSec,
  kMTimeNsec,
  kCTimeSec,
  kCTimeNsec,
  kBirthTimeSec,
  kBirthTimeNsec,
  kFsStatsFieldsNumber // 18
};

static constexpr int kFsStatsBufferLength = kFsStatsFieldsNumber * 2; // 36

// ---------------------------------------------------------------------------
// StatFs field layout
// ---------------------------------------------------------------------------

enum FsStatFsOffset {
  kType = 0,
  kBSize,
  kStatFsBlocks,
  kBFree,
  kBAvail,
  kFiles,
  kFFree,
  kFsStatFsFieldsNumber // 7
};

static constexpr int kStatFsBufferLength = kFsStatFsFieldsNumber * 2; // 14

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/// Get a UTF-8 string from a napi_value. Returns empty string on non-string.
static std::string getStringArg(napi_env env, napi_value val) {
  size_t len = 0;
  napi_get_value_string_utf8(env, val, nullptr, 0, &len);
  std::string result(len, '\0');
  napi_get_value_string_utf8(env, val, &result[0], len + 1, &len);
  return result;
}

/// Get an int32 from a napi_value. Returns dflt if not a number.
static int32_t getInt32(napi_env env, napi_value val, int32_t dflt = 0) {
  int32_t result = dflt;
  napi_get_value_int32(env, val, &result);
  return result;
}

/// Get a double from a napi_value.
static double getDouble(napi_env env, napi_value val, double dflt = 0.0) {
  double result = dflt;
  napi_get_value_double(env, val, &result);
  return result;
}

/// Get an int64 from a napi_value (for file positions).
static int64_t getInt64(napi_env env, napi_value val, int64_t dflt = -1) {
  int64_t result = dflt;
  napi_get_value_int64(env, val, &result);
  return result;
}

/// Check if a value is null or undefined.
static bool isNullOrUndefined(napi_env env, napi_value val) {
  napi_valuetype type;
  napi_typeof(env, val, &type);
  return type == napi_undefined || type == napi_null;
}

/// Get a boolean from a napi_value.
static bool getBool(napi_env env, napi_value val, bool dflt = false) {
  bool result = dflt;
  napi_get_value_bool(env, val, &result);
  return result;
}

// ---------------------------------------------------------------------------
// UVException: throw a libuv error as a JS Error with the right properties
// ---------------------------------------------------------------------------

/// Create a UVException Error object (does NOT throw it).
static napi_value createUVException(
    napi_env env,
    int errorno,
    const char *syscall,
    const char *path = nullptr,
    const char *dest = nullptr) {
  const char *code = uv_err_name(errorno);
  const char *msg = uv_strerror(errorno);

  // Build message: "CODE: message, syscall 'path' -> 'dest'"
  std::string fullMsg = std::string(code) + ": " + msg + ", " + syscall;
  if (path) {
    fullMsg += " '";
    fullMsg += path;
    fullMsg += "'";
  }
  if (dest) {
    fullMsg += " -> '";
    fullMsg += dest;
    fullMsg += "'";
  }

  // Create an Error object.
  napi_value errObj;
  napi_value msgVal;
  napi_create_string_utf8(env, fullMsg.c_str(), fullMsg.size(), &msgVal);
  napi_create_error(env, nullptr, msgVal, &errObj);

  // Set properties: errno, code, syscall, path, dest.
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
  if (dest) {
    napi_create_string_utf8(env, dest, NAPI_AUTO_LENGTH, &val);
    napi_set_named_property(env, errObj, "dest", val);
  }

  return errObj;
}

/// Throw a UVException. Returns nullptr (for use as return value).
static napi_value throwUVException(
    napi_env env,
    int errorno,
    const char *syscall,
    const char *path = nullptr,
    const char *dest = nullptr) {
  napi_value errObj = createUVException(env, errorno, syscall, path, dest);
  napi_throw(env, errObj);
  return nullptr;
}

/// Throw a cp-style error with code, syscall, path properties.
/// Used by cpSyncCheckPaths, cpSyncOverrideFile, cpSyncCopyDir.
static napi_value throwCpError(
    napi_env env,
    const char *code,
    const std::string &message,
    const char *path = nullptr) {
  napi_value msgVal;
  napi_create_string_utf8(env, message.c_str(), message.size(), &msgVal);
  napi_value errObj;
  napi_create_error(env, nullptr, msgVal, &errObj);

  napi_value val;
  napi_create_string_utf8(env, code, NAPI_AUTO_LENGTH, &val);
  napi_set_named_property(env, errObj, "code", val);

  napi_create_string_utf8(env, "cp", NAPI_AUTO_LENGTH, &val);
  napi_set_named_property(env, errObj, "syscall", val);

  if (path) {
    napi_create_string_utf8(env, path, NAPI_AUTO_LENGTH, &val);
    napi_set_named_property(env, errObj, "path", val);
  }

  napi_throw(env, errObj);
  return nullptr;
}

/// Set error info on a ctx object (for writeBuffer/writeString pattern).
static void setCtxError(
    napi_env env,
    napi_value ctx,
    int errorno,
    const char *syscall,
    const char *path = nullptr) {
  napi_value val;

  napi_create_int32(env, errorno, &val);
  napi_set_named_property(env, ctx, "errno", val);

  napi_create_string_utf8(env, uv_strerror(errorno), NAPI_AUTO_LENGTH, &val);
  napi_set_named_property(env, ctx, "message", val);

  napi_create_string_utf8(env, syscall, NAPI_AUTO_LENGTH, &val);
  napi_set_named_property(env, ctx, "syscall", val);

  if (path) {
    napi_create_string_utf8(env, path, NAPI_AUTO_LENGTH, &val);
    napi_set_named_property(env, ctx, "path", val);
  }
}

// ---------------------------------------------------------------------------
// Fill stat buffer from uv_stat_t
// ---------------------------------------------------------------------------

static void fillStatValues(double *buf, const uv_stat_t *s) {
  buf[kDev] = static_cast<double>(s->st_dev);
  buf[kMode] = static_cast<double>(s->st_mode);
  buf[kNlink] = static_cast<double>(s->st_nlink);
  buf[kUid] = static_cast<double>(s->st_uid);
  buf[kGid] = static_cast<double>(s->st_gid);
  buf[kRdev] = static_cast<double>(s->st_rdev);
  buf[kBlkSize] = static_cast<double>(s->st_blksize);
  buf[kIno] = static_cast<double>(s->st_ino);
  buf[kSize] = static_cast<double>(s->st_size);
  buf[kBlocks] = static_cast<double>(s->st_blocks);
  buf[kATimeSec] = static_cast<double>(s->st_atim.tv_sec);
  buf[kATimeNsec] = static_cast<double>(s->st_atim.tv_nsec);
  buf[kMTimeSec] = static_cast<double>(s->st_mtim.tv_sec);
  buf[kMTimeNsec] = static_cast<double>(s->st_mtim.tv_nsec);
  buf[kCTimeSec] = static_cast<double>(s->st_ctim.tv_sec);
  buf[kCTimeNsec] = static_cast<double>(s->st_ctim.tv_nsec);
  buf[kBirthTimeSec] = static_cast<double>(s->st_birthtim.tv_sec);
  buf[kBirthTimeNsec] = static_cast<double>(s->st_birthtim.tv_nsec);
}

static void fillBigIntStatValues(int64_t *buf, const uv_stat_t *s) {
  buf[kDev] = static_cast<int64_t>(s->st_dev);
  buf[kMode] = static_cast<int64_t>(s->st_mode);
  buf[kNlink] = static_cast<int64_t>(s->st_nlink);
  buf[kUid] = static_cast<int64_t>(s->st_uid);
  buf[kGid] = static_cast<int64_t>(s->st_gid);
  buf[kRdev] = static_cast<int64_t>(s->st_rdev);
  buf[kBlkSize] = static_cast<int64_t>(s->st_blksize);
  buf[kIno] = static_cast<int64_t>(s->st_ino);
  buf[kSize] = static_cast<int64_t>(s->st_size);
  buf[kBlocks] = static_cast<int64_t>(s->st_blocks);
  buf[kATimeSec] = static_cast<int64_t>(s->st_atim.tv_sec);
  buf[kATimeNsec] = static_cast<int64_t>(s->st_atim.tv_nsec);
  buf[kMTimeSec] = static_cast<int64_t>(s->st_mtim.tv_sec);
  buf[kMTimeNsec] = static_cast<int64_t>(s->st_mtim.tv_nsec);
  buf[kCTimeSec] = static_cast<int64_t>(s->st_ctim.tv_sec);
  buf[kCTimeNsec] = static_cast<int64_t>(s->st_ctim.tv_nsec);
  buf[kBirthTimeSec] = static_cast<int64_t>(s->st_birthtim.tv_sec);
  buf[kBirthTimeNsec] = static_cast<int64_t>(s->st_birthtim.tv_nsec);
}

// ---------------------------------------------------------------------------
// Persistent state: shared typed arrays for stat results
// ---------------------------------------------------------------------------

struct FsBindingData {
  napi_ref statValuesRef; // Float64Array(36)
  napi_ref bigintStatValuesRef; // BigInt64Array(36)
  napi_ref statFsValuesRef; // Float64Array(14)
  napi_ref bigintStatFsValuesRef; // BigInt64Array(14)
  napi_ref kUsePromisesRef; // Sentinel object for promise mode
};

static FsBindingData *getFsData(napi_env env, napi_callback_info info) {
  void *data = nullptr;
  napi_get_cb_info(env, info, nullptr, nullptr, nullptr, &data);
  return static_cast<FsBindingData *>(data);
}

/// Fill stat result into the appropriate shared typed array and return it.
static napi_value fillAndReturnStats(
    napi_env env,
    FsBindingData *fsData,
    const uv_stat_t *s,
    bool useBigint,
    int offset = 0) {
  napi_value result;
  if (useBigint) {
    napi_get_reference_value(env, fsData->bigintStatValuesRef, &result);
    // Get the backing buffer.
    napi_typedarray_type type;
    size_t length;
    void *data;
    napi_get_typedarray_info(
        env, result, &type, &length, &data, nullptr, nullptr);
    auto *buf = static_cast<int64_t *>(data);
    fillBigIntStatValues(buf + offset, s);
  } else {
    napi_get_reference_value(env, fsData->statValuesRef, &result);
    napi_typedarray_type type;
    size_t length;
    void *data;
    napi_get_typedarray_info(
        env, result, &type, &length, &data, nullptr, nullptr);
    auto *buf = static_cast<double *>(data);
    fillStatValues(buf + offset, s);
  }
  return result;
}

// ---------------------------------------------------------------------------
// Async infrastructure: FSReqWrap
// ---------------------------------------------------------------------------

/// Result type determines how the completion callback processes the result.
enum class FSReqResultType {
  Void, // No result (unlink, rename, mkdir non-recursive, chmod, etc.)
  Integer, // Result is req.result (open -> fd, read -> bytes, write -> bytes)
  FileHandle, // Result is { fd: N, getAsyncId() } object (openFileHandle)
  Stat, // Result is stat buffer (stat, lstat, fstat)
  StatFs, // Result is statfs buffer
  StringPath, // Result is req.path (mkdtemp)
  StringPtr, // Result is req.ptr as string (readlink, realpath)
  Readdir, // Result is readdir entries
  MkdirResult, // Result is first created path string or undefined
};

/// Request wrapper for async fs operations.
struct FSReqWrap {
  uv_fs_t req{};
  napi_env env = nullptr;
  napi_ref callbackRef =
      nullptr; // Ref to FSReqCallback object (callback mode only)
  napi_deferred deferred = nullptr; // Promise deferred (promise mode only)
  napi_ref bufferRef = nullptr; // Ref to buffer to prevent GC during async op
  FsBindingData *fsData = nullptr;
  FSReqResultType resultType = FSReqResultType::Void;
  bool useBigint = false;
  bool isPromise = false; // true if using kUsePromises (deferred promise)
  bool withFileTypes = false; // For readdir
  std::string path; // For error messages
  std::string dest; // For error messages (rename dest)
  std::string writeData; // String data for async writeString
  std::string firstCreated; // For recursive mkdir
};

/// Check if the given argument is the kUsePromises sentinel.
static bool isUsePromises(napi_env env, napi_value val, FsBindingData *fsData) {
  if (!fsData || !fsData->kUsePromisesRef)
    return false;
  napi_value sentinel;
  napi_get_reference_value(env, fsData->kUsePromisesRef, &sentinel);
  bool result = false;
  napi_strict_equals(env, val, sentinel, &result);
  return result;
}

/// Check if the given argument is an FSReqCallback object (has oncomplete).
static bool isFSReqCallback(napi_env env, napi_value val) {
  napi_valuetype type;
  napi_typeof(env, val, &type);
  if (type != napi_object)
    return false;
  // Check that it has an oncomplete property.
  bool hasOncomplete = false;
  napi_has_named_property(env, val, "oncomplete", &hasOncomplete);
  return hasOncomplete;
}

/// Create a fresh (non-shared) stats typed array for promise mode.
/// Promise mode must not use the shared stats buffer because JS code
/// may not read the result before another stat operation overwrites it.
static napi_value
createFreshStats(napi_env env, const uv_stat_t *s, bool useBigint) {
  const size_t count = kFsStatsFieldsNumber;
  napi_value arrBuf;
  void *data;
  napi_value typedArr;
  if (useBigint) {
    napi_create_arraybuffer(env, count * sizeof(int64_t), &data, &arrBuf);
    napi_create_typedarray(
        env, napi_bigint64_array, count, arrBuf, 0, &typedArr);
    fillBigIntStatValues(static_cast<int64_t *>(data), s);
  } else {
    napi_create_arraybuffer(env, count * sizeof(double), &data, &arrBuf);
    napi_create_typedarray(
        env, napi_float64_array, count, arrBuf, 0, &typedArr);
    fillStatValues(static_cast<double *>(data), s);
  }
  return typedArr;
}

// Forward declaration (used by fileHandleClose before definition).
static void fsAfterAsync(uv_fs_t *req);

/// Stub getAsyncId for FileHandle objects (async hooks not implemented).
static napi_value fileHandleGetAsyncId(
    napi_env env,
    napi_callback_info /*info*/) {
  napi_value result;
  napi_create_int32(env, 0, &result);
  return result;
}

/// Close method for FileHandle objects. Returns a promise that resolves
/// after uv_fs_close completes.
static napi_value fileHandleClose(napi_env env, napi_callback_info info) {
  napi_value thisObj;
  napi_get_cb_info(env, info, nullptr, nullptr, &thisObj, nullptr);

  napi_value fdVal;
  napi_get_named_property(env, thisObj, "fd", &fdVal);
  int fd;
  napi_get_value_int32(env, fdVal, &fd);

  if (fd < 0) {
    // Already closed -- return resolved promise.
    napi_deferred deferred;
    napi_value promise;
    napi_create_promise(env, &deferred, &promise);
    napi_value undef;
    napi_get_undefined(env, &undef);
    napi_resolve_deferred(env, deferred, undef);
    return promise;
  }

  // Mark as closed.
  napi_value minusOne;
  napi_create_int32(env, -1, &minusOne);
  napi_set_named_property(env, thisObj, "fd", minusOne);

  auto *wrap = new FSReqWrap();
  wrap->env = env;
  wrap->req.data = wrap;
  wrap->resultType = FSReqResultType::Void;
  wrap->isPromise = true;
  napi_value promise;
  napi_create_promise(env, &wrap->deferred, &promise);

  uv_fs_close(s_fsLoop, &wrap->req, fd, fsAfterAsync);
  return promise;
}

/// Create a FileHandle-like object { fd, getAsyncId(), close() }
/// for promises API.
static napi_value createFileHandleObject(napi_env env, int fd) {
  napi_value obj;
  napi_create_object(env, &obj);

  napi_value fdVal;
  napi_create_int32(env, fd, &fdVal);
  napi_set_named_property(env, obj, "fd", fdVal);

  napi_value fn;
  napi_create_function(
      env, "getAsyncId", NAPI_AUTO_LENGTH, fileHandleGetAsyncId, nullptr, &fn);
  napi_set_named_property(env, obj, "getAsyncId", fn);

  napi_create_function(
      env, "close", NAPI_AUTO_LENGTH, fileHandleClose, nullptr, &fn);
  napi_set_named_property(env, obj, "close", fn);

  return obj;
}

/// Common libuv fs completion callback. Runs on the main thread after I/O.
static void fsAfterAsync(uv_fs_t *req) {
  auto *wrap = static_cast<FSReqWrap *>(req->data);
  napi_env env = wrap->env;

  napi_handle_scope scope;
  napi_open_handle_scope(env, &scope);

  napi_value global;
  napi_get_global(env, &global);

  int result = static_cast<int>(req->result);

  if (result < 0) {
    // Error case: call oncomplete(error) or reject promise.
    napi_value errObj = createUVException(
        env,
        result,
        uv_fs_get_type(req) == UV_FS_STAT            ? "stat"
            : uv_fs_get_type(req) == UV_FS_LSTAT     ? "lstat"
            : uv_fs_get_type(req) == UV_FS_FSTAT     ? "fstat"
            : uv_fs_get_type(req) == UV_FS_OPEN      ? "open"
            : uv_fs_get_type(req) == UV_FS_CLOSE     ? "close"
            : uv_fs_get_type(req) == UV_FS_READ      ? "read"
            : uv_fs_get_type(req) == UV_FS_WRITE     ? "write"
            : uv_fs_get_type(req) == UV_FS_RENAME    ? "rename"
            : uv_fs_get_type(req) == UV_FS_UNLINK    ? "unlink"
            : uv_fs_get_type(req) == UV_FS_MKDIR     ? "mkdir"
            : uv_fs_get_type(req) == UV_FS_RMDIR     ? "rmdir"
            : uv_fs_get_type(req) == UV_FS_SCANDIR   ? "scandir"
            : uv_fs_get_type(req) == UV_FS_CHMOD     ? "chmod"
            : uv_fs_get_type(req) == UV_FS_FCHMOD    ? "fchmod"
            : uv_fs_get_type(req) == UV_FS_CHOWN     ? "chown"
            : uv_fs_get_type(req) == UV_FS_FCHOWN    ? "fchown"
            : uv_fs_get_type(req) == UV_FS_LCHOWN    ? "lchown"
            : uv_fs_get_type(req) == UV_FS_LINK      ? "link"
            : uv_fs_get_type(req) == UV_FS_SYMLINK   ? "symlink"
            : uv_fs_get_type(req) == UV_FS_READLINK  ? "readlink"
            : uv_fs_get_type(req) == UV_FS_REALPATH  ? "realpath"
            : uv_fs_get_type(req) == UV_FS_FTRUNCATE ? "ftruncate"
            : uv_fs_get_type(req) == UV_FS_UTIME     ? "utime"
            : uv_fs_get_type(req) == UV_FS_FUTIME    ? "futime"
            : uv_fs_get_type(req) == UV_FS_LUTIME    ? "lutime"
            : uv_fs_get_type(req) == UV_FS_MKDTEMP   ? "mkdtemp"
            : uv_fs_get_type(req) == UV_FS_COPYFILE  ? "copyfile"
            : uv_fs_get_type(req) == UV_FS_ACCESS    ? "access"
            : uv_fs_get_type(req) == UV_FS_FSYNC     ? "fsync"
            : uv_fs_get_type(req) == UV_FS_FDATASYNC ? "fdatasync"
            : uv_fs_get_type(req) == UV_FS_STATFS    ? "statfs"
                                                     : "unknown",
        wrap->path.empty() ? nullptr : wrap->path.c_str(),
        wrap->dest.empty() ? nullptr : wrap->dest.c_str());

    if (wrap->isPromise) {
      // Reject the deferred promise.
      napi_reject_deferred(env, wrap->deferred, errObj);
    } else {
      napi_value reqObj;
      napi_get_reference_value(env, wrap->callbackRef, &reqObj);
      // Get oncomplete from the req object.
      napi_value oncomplete;
      napi_get_named_property(env, reqObj, "oncomplete", &oncomplete);
      // Call with FSReqCallback as 'this' (Node convention).
      napi_value cbResult;
      napi_call_function(env, reqObj, oncomplete, 1, &errObj, &cbResult);
    }
  } else {
    // Success case: construct result and call oncomplete(null, result) or
    // resolve promise.
    napi_value jsResult;

    switch (wrap->resultType) {
      case FSReqResultType::Void:
        napi_get_undefined(env, &jsResult);
        break;

      case FSReqResultType::Integer:
        napi_create_int32(env, result, &jsResult);
        break;

      case FSReqResultType::FileHandle:
        jsResult = createFileHandleObject(env, result);
        break;

      case FSReqResultType::Stat:
      case FSReqResultType::StatFs:
        if (wrap->resultType == FSReqResultType::Stat) {
          if (wrap->isPromise) {
            // Promise mode: create a fresh array so concurrent stats
            // don't clobber each other's results.
            jsResult = createFreshStats(env, &req->statbuf, wrap->useBigint);
          } else {
            jsResult = fillAndReturnStats(
                env, wrap->fsData, &req->statbuf, wrap->useBigint);
          }
        } else {
          // StatFs
          auto *sf = static_cast<uv_statfs_t *>(req->ptr);
          auto fillStatFs = [sf](auto *buf) {
            buf[kType] = decltype(buf[0])(sf->f_type);
            buf[kBSize] = decltype(buf[0])(sf->f_bsize);
            buf[kStatFsBlocks] = decltype(buf[0])(sf->f_blocks);
            buf[kBFree] = decltype(buf[0])(sf->f_bfree);
            buf[kBAvail] = decltype(buf[0])(sf->f_bavail);
            buf[kFiles] = decltype(buf[0])(sf->f_files);
            buf[kFFree] = decltype(buf[0])(sf->f_ffree);
          };
          if (wrap->isPromise) {
            // Promise mode: fresh array.
            napi_value arrBuf;
            void *data;
            if (wrap->useBigint) {
              napi_create_arraybuffer(
                  env, kStatFsBufferLength * sizeof(int64_t), &data, &arrBuf);
              napi_create_typedarray(
                  env,
                  napi_bigint64_array,
                  kStatFsBufferLength,
                  arrBuf,
                  0,
                  &jsResult);
              fillStatFs(static_cast<int64_t *>(data));
            } else {
              napi_create_arraybuffer(
                  env, kStatFsBufferLength * sizeof(double), &data, &arrBuf);
              napi_create_typedarray(
                  env,
                  napi_float64_array,
                  kStatFsBufferLength,
                  arrBuf,
                  0,
                  &jsResult);
              fillStatFs(static_cast<double *>(data));
            }
          } else if (wrap->useBigint) {
            napi_get_reference_value(
                env, wrap->fsData->bigintStatFsValuesRef, &jsResult);
            napi_typedarray_type arrType;
            size_t length;
            void *data;
            napi_get_typedarray_info(
                env, jsResult, &arrType, &length, &data, nullptr, nullptr);
            fillStatFs(static_cast<int64_t *>(data));
          } else {
            napi_get_reference_value(
                env, wrap->fsData->statFsValuesRef, &jsResult);
            napi_typedarray_type arrType;
            size_t length;
            void *data;
            napi_get_typedarray_info(
                env, jsResult, &arrType, &length, &data, nullptr, nullptr);
            fillStatFs(static_cast<double *>(data));
          }
        }
        break;

      case FSReqResultType::StringPath:
        napi_create_string_utf8(env, req->path, NAPI_AUTO_LENGTH, &jsResult);
        break;

      case FSReqResultType::StringPtr: {
        auto *ptr = static_cast<const char *>(req->ptr);
        napi_create_string_utf8(env, ptr, NAPI_AUTO_LENGTH, &jsResult);
        break;
      }

      case FSReqResultType::Readdir: {
        // Collect entries from scandir.
        std::vector<std::string> names;
        std::vector<int> types;
        uv_dirent_t ent;
        while (uv_fs_scandir_next(req, &ent) == 0) {
          names.push_back(ent.name);
          if (wrap->withFileTypes)
            types.push_back(static_cast<int>(ent.type));
        }

        if (wrap->withFileTypes) {
          napi_value namesArr;
          napi_create_array_with_length(env, names.size(), &namesArr);
          for (size_t i = 0; i < names.size(); i++) {
            napi_value str;
            napi_create_string_utf8(
                env, names[i].c_str(), names[i].size(), &str);
            napi_set_element(env, namesArr, static_cast<uint32_t>(i), str);
          }
          napi_value typesArr;
          napi_create_array_with_length(env, types.size(), &typesArr);
          for (size_t i = 0; i < types.size(); i++) {
            napi_value num;
            napi_create_int32(env, types[i], &num);
            napi_set_element(env, typesArr, static_cast<uint32_t>(i), num);
          }
          napi_value resultArr;
          napi_create_array_with_length(env, 2, &resultArr);
          napi_set_element(env, resultArr, 0, namesArr);
          napi_set_element(env, resultArr, 1, typesArr);
          jsResult = resultArr;
        } else {
          napi_value namesArr;
          napi_create_array_with_length(env, names.size(), &namesArr);
          for (size_t i = 0; i < names.size(); i++) {
            napi_value str;
            napi_create_string_utf8(
                env, names[i].c_str(), names[i].size(), &str);
            napi_set_element(env, namesArr, static_cast<uint32_t>(i), str);
          }
          jsResult = namesArr;
        }
        break;
      }

      case FSReqResultType::MkdirResult:
        if (!wrap->firstCreated.empty()) {
          napi_create_string_utf8(
              env,
              wrap->firstCreated.c_str(),
              wrap->firstCreated.size(),
              &jsResult);
        } else {
          napi_get_undefined(env, &jsResult);
        }
        break;
    }

    if (wrap->isPromise) {
      napi_resolve_deferred(env, wrap->deferred, jsResult);
    } else {
      napi_value reqObj;
      napi_get_reference_value(env, wrap->callbackRef, &reqObj);
      napi_value oncomplete;
      napi_get_named_property(env, reqObj, "oncomplete", &oncomplete);
      napi_value nullVal;
      napi_get_null(env, &nullVal);
      napi_value args[2] = {nullVal, jsResult};
      int argCount = (wrap->resultType == FSReqResultType::Void) ? 1 : 2;
      // Call with FSReqCallback as 'this' (Node convention).
      napi_value cbResult;
      napi_call_function(env, reqObj, oncomplete, argCount, args, &cbResult);
    }
  }

  // Clear any pending exception (shouldn't propagate from callbacks).
  bool pending = false;
  napi_is_exception_pending(env, &pending);
  if (pending) {
    napi_value exc;
    napi_get_and_clear_last_exception(env, &exc);
    // Print to stderr for debugging.
    napi_value stack;
    napi_status st = napi_get_named_property(env, exc, "stack", &stack);
    napi_valuetype stackType = napi_undefined;
    if (st == napi_ok)
      napi_typeof(env, stack, &stackType);
    napi_value msg;
    if (stackType == napi_string)
      msg = stack;
    else
      napi_coerce_to_string(env, exc, &msg);
    char buf[4096];
    size_t len = 0;
    napi_get_value_string_utf8(env, msg, buf, sizeof(buf), &len);
    std::fprintf(stderr, "%.*s\n", static_cast<int>(len), buf);
  }

  napi_close_handle_scope(env, scope);

  // Cleanup.
  if (!wrap->isPromise && wrap->callbackRef)
    napi_delete_reference(env, wrap->callbackRef);
  if (wrap->bufferRef)
    napi_delete_reference(env, wrap->bufferRef);
  // uv_fs_req_cleanup frees req->bufs (if != bufsml), path, ptr, etc.
  uv_fs_req_cleanup(req);
  delete wrap;
}

/// Start an async fs operation. Returns the promise (for kUsePromises) or
/// undefined (for FSReqCallback). The wrap takes ownership and will be freed
/// in fsAfterAsync.
static napi_value startAsyncFsOp(
    napi_env env,
    FSReqWrap *wrap,
    napi_value reqOrSentinel,
    FsBindingData *fsData) {
  wrap->env = env;
  wrap->fsData = fsData;
  wrap->req.data = wrap;

  if (isUsePromises(env, reqOrSentinel, fsData)) {
    // Promise mode: create a deferred.
    wrap->isPromise = true;
    napi_value promise;
    napi_create_promise(env, &wrap->deferred, &promise);
    return promise;
  } else {
    // FSReqCallback mode: store reference to the request object.
    wrap->isPromise = false;
    napi_create_reference(env, reqOrSentinel, 1, &wrap->callbackRef);

    napi_value undef;
    napi_get_undefined(env, &undef);
    return undef;
  }
}

/// Detect if the argument at position `pos` is an async request.
/// Returns the arg value if it's a req/kUsePromises, or nullptr if sync.
static napi_value getAsyncReq(
    napi_env env,
    size_t argc,
    napi_value *argv,
    size_t pos,
    FsBindingData *fsData) {
  if (pos >= argc)
    return nullptr;
  napi_value val = argv[pos];
  if (isNullOrUndefined(env, val))
    return nullptr;
  if (isUsePromises(env, val, fsData))
    return val;
  if (isFSReqCallback(env, val))
    return val;
  return nullptr;
}

// ---------------------------------------------------------------------------
// Sync FS operations (with async dispatch when req is provided)
// ---------------------------------------------------------------------------

// binding.access(path, mode, req?)
static napi_value fsAccess(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3];
  FsBindingData *fsData = getFsData(env, info);
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  std::string path = getStringArg(env, argv[0]);
  int mode = getInt32(env, argv[1], 0);

  napi_value asyncReq = getAsyncReq(env, argc, argv, 2, fsData);
  if (asyncReq) {
    auto *wrap = new FSReqWrap();
    wrap->resultType = FSReqResultType::Void;
    wrap->path = path;
    napi_value result = startAsyncFsOp(env, wrap, asyncReq, fsData);
    uv_fs_access(s_fsLoop, &wrap->req, path.c_str(), mode, fsAfterAsync);
    return result;
  }

  uv_fs_t req;
  int result = uv_fs_access(nullptr, &req, path.c_str(), mode, nullptr);
  uv_fs_req_cleanup(&req);
  if (result < 0) {
    return throwUVException(env, result, "access", path.c_str());
  }
  return nullptr;
}

// binding.open(path, flags, mode, req?)
static napi_value fsOpen(napi_env env, napi_callback_info info) {
  size_t argc = 4;
  napi_value argv[4];
  FsBindingData *fsData = getFsData(env, info);
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  std::string path = getStringArg(env, argv[0]);
  int flags = getInt32(env, argv[1], 0);
  int mode = getInt32(env, argv[2], 0666);

  napi_value asyncReq = getAsyncReq(env, argc, argv, 3, fsData);
  if (asyncReq) {
    auto *wrap = new FSReqWrap();
    wrap->resultType = FSReqResultType::Integer;
    wrap->path = path;
    napi_value result = startAsyncFsOp(env, wrap, asyncReq, fsData);
    uv_fs_open(s_fsLoop, &wrap->req, path.c_str(), flags, mode, fsAfterAsync);
    return result;
  }

  uv_fs_t req;
  int result = uv_fs_open(nullptr, &req, path.c_str(), flags, mode, nullptr);
  uv_fs_req_cleanup(&req);
  if (result < 0) {
    return throwUVException(env, result, "open", path.c_str());
  }

  napi_value jsResult;
  napi_create_int32(env, result, &jsResult);
  return jsResult;
}

// binding.openFileHandle(path, flags, mode, req?)
// Like open() but resolves to a FileHandle object { fd, getAsyncId() }
// instead of a raw fd. Used by fs.promises.
static napi_value fsOpenFileHandle(napi_env env, napi_callback_info info) {
  size_t argc = 4;
  napi_value argv[4];
  FsBindingData *fsData = getFsData(env, info);
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  std::string path = getStringArg(env, argv[0]);
  int flags = getInt32(env, argv[1], 0);
  int mode = getInt32(env, argv[2], 0666);

  napi_value asyncReq = getAsyncReq(env, argc, argv, 3, fsData);
  if (asyncReq) {
    auto *wrap = new FSReqWrap();
    wrap->resultType = FSReqResultType::FileHandle;
    wrap->path = path;
    napi_value result = startAsyncFsOp(env, wrap, asyncReq, fsData);
    uv_fs_open(s_fsLoop, &wrap->req, path.c_str(), flags, mode, fsAfterAsync);
    return result;
  }

  uv_fs_t req;
  int result = uv_fs_open(nullptr, &req, path.c_str(), flags, mode, nullptr);
  uv_fs_req_cleanup(&req);
  if (result < 0) {
    return throwUVException(env, result, "open", path.c_str());
  }

  return createFileHandleObject(env, result);
}

// binding.close(fd, req?)
static napi_value fsClose(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  FsBindingData *fsData = getFsData(env, info);
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  int fd = getInt32(env, argv[0]);

  napi_value asyncReq = getAsyncReq(env, argc, argv, 1, fsData);
  if (asyncReq) {
    auto *wrap = new FSReqWrap();
    wrap->resultType = FSReqResultType::Void;
    napi_value result = startAsyncFsOp(env, wrap, asyncReq, fsData);
    uv_fs_close(s_fsLoop, &wrap->req, fd, fsAfterAsync);
    return result;
  }

  uv_fs_t req;
  int result = uv_fs_close(nullptr, &req, fd, nullptr);
  uv_fs_req_cleanup(&req);
  if (result < 0) {
    return throwUVException(env, result, "close");
  }
  return nullptr;
}

// binding.read(fd, buffer, offset, length, position, req?)
static napi_value fsRead(napi_env env, napi_callback_info info) {
  size_t argc = 6;
  napi_value argv[6];
  FsBindingData *fsData = getFsData(env, info);
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  int fd = getInt32(env, argv[0]);

  // Get the buffer data.
  napi_typedarray_type arrType;
  size_t arrLength;
  void *data;
  napi_get_typedarray_info(
      env, argv[1], &arrType, &arrLength, &data, nullptr, nullptr);
  auto *bufData = static_cast<uint8_t *>(data);

  int32_t offset = getInt32(env, argv[2], 0);
  int32_t length = getInt32(env, argv[3], 0);
  int64_t position = getInt64(env, argv[4], -1);

  napi_value asyncReq = getAsyncReq(env, argc, argv, 5, fsData);
  if (asyncReq) {
    auto *wrap = new FSReqWrap();
    wrap->resultType = FSReqResultType::Integer;
    // Keep buffer alive during async op.
    napi_create_reference(env, argv[1], 1, &wrap->bufferRef);
    napi_value result = startAsyncFsOp(env, wrap, asyncReq, fsData);
    uv_buf_t buf = uv_buf_init(
        reinterpret_cast<char *>(bufData + offset),
        static_cast<unsigned int>(length));
    // libuv copies bufs internally for async ops, so local var is fine.
    uv_fs_read(s_fsLoop, &wrap->req, fd, &buf, 1, position, fsAfterAsync);
    return result;
  }

  uv_buf_t buf = uv_buf_init(
      reinterpret_cast<char *>(bufData + offset),
      static_cast<unsigned int>(length));

  uv_fs_t req;
  int result = uv_fs_read(nullptr, &req, fd, &buf, 1, position, nullptr);
  uv_fs_req_cleanup(&req);
  if (result < 0) {
    return throwUVException(env, result, "read");
  }

  napi_value jsResult;
  napi_create_int32(env, result, &jsResult);
  return jsResult;
}

// binding.writeBuffer(fd, buffer, offset, length, position, req?, ctx?)
static napi_value fsWriteBuffer(napi_env env, napi_callback_info info) {
  size_t argc = 7;
  napi_value argv[7];
  FsBindingData *fsData = getFsData(env, info);
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  int fd = getInt32(env, argv[0]);

  // Get the buffer data.
  napi_typedarray_type arrType;
  size_t arrLength;
  void *data;
  napi_get_typedarray_info(
      env, argv[1], &arrType, &arrLength, &data, nullptr, nullptr);
  auto *bufData = static_cast<uint8_t *>(data);

  int32_t offset = getInt32(env, argv[2], 0);
  int32_t length = getInt32(env, argv[3], 0);

  // position can be null (current position).
  int64_t position = -1;
  if (argc > 4 && !isNullOrUndefined(env, argv[4])) {
    position = getInt64(env, argv[4], -1);
  }

  napi_value asyncReq = getAsyncReq(env, argc, argv, 5, fsData);
  if (asyncReq) {
    auto *wrap = new FSReqWrap();
    wrap->resultType = FSReqResultType::Integer;
    napi_create_reference(env, argv[1], 1, &wrap->bufferRef);
    napi_value result = startAsyncFsOp(env, wrap, asyncReq, fsData);
    uv_buf_t buf = uv_buf_init(
        reinterpret_cast<char *>(bufData + offset),
        static_cast<unsigned int>(length));
    uv_fs_write(s_fsLoop, &wrap->req, fd, &buf, 1, position, fsAfterAsync);
    return result;
  }

  uv_buf_t buf = uv_buf_init(
      reinterpret_cast<char *>(bufData + offset),
      static_cast<unsigned int>(length));

  uv_fs_t req;
  int result = uv_fs_write(nullptr, &req, fd, &buf, 1, position, nullptr);
  uv_fs_req_cleanup(&req);

  // writeBuffer uses ctx-based error reporting.
  if (result < 0 && argc > 6 && !isNullOrUndefined(env, argv[6])) {
    setCtxError(env, argv[6], result, "write");
    napi_value jsResult;
    napi_create_int32(env, 0, &jsResult);
    return jsResult;
  }
  if (result < 0) {
    return throwUVException(env, result, "write");
  }

  napi_value jsResult;
  napi_create_int32(env, result, &jsResult);
  return jsResult;
}

// binding.writeString(fd, string, position, encoding, req?, ctx?)
static napi_value fsWriteString(napi_env env, napi_callback_info info) {
  size_t argc = 6;
  napi_value argv[6];
  FsBindingData *fsData = getFsData(env, info);
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  int fd = getInt32(env, argv[0]);
  std::string str = getStringArg(env, argv[1]);

  // position can be null.
  int64_t position = -1;
  if (argc > 2 && !isNullOrUndefined(env, argv[2])) {
    position = getInt64(env, argv[2], -1);
  }

  // encoding is argv[3] but we always write as UTF-8.

  napi_value asyncReq = getAsyncReq(env, argc, argv, 4, fsData);
  if (asyncReq) {
    auto *wrap = new FSReqWrap();
    wrap->resultType = FSReqResultType::Integer;
    // Copy string data into the wrap so it outlives the JS call.
    wrap->writeData = str;
    napi_value result = startAsyncFsOp(env, wrap, asyncReq, fsData);
    uv_buf_t buf = uv_buf_init(
        const_cast<char *>(wrap->writeData.data()),
        static_cast<unsigned int>(wrap->writeData.size()));
    uv_fs_write(s_fsLoop, &wrap->req, fd, &buf, 1, position, fsAfterAsync);
    return result;
  }

  uv_buf_t buf = uv_buf_init(
      const_cast<char *>(str.data()), static_cast<unsigned int>(str.size()));

  uv_fs_t req;
  int result = uv_fs_write(nullptr, &req, fd, &buf, 1, position, nullptr);
  uv_fs_req_cleanup(&req);

  // writeString uses ctx-based error reporting.
  if (result < 0 && argc > 5 && !isNullOrUndefined(env, argv[5])) {
    setCtxError(env, argv[5], result, "write");
    napi_value jsResult;
    napi_create_int32(env, 0, &jsResult);
    return jsResult;
  }
  if (result < 0) {
    return throwUVException(env, result, "write");
  }

  napi_value jsResult;
  napi_create_int32(env, result, &jsResult);
  return jsResult;
}

// binding.stat(path, useBigint, req?, throwIfNoEntry?)
static napi_value fsStat(napi_env env, napi_callback_info info) {
  size_t argc = 4;
  napi_value argv[4];
  FsBindingData *fsData = getFsData(env, info);
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  std::string path = getStringArg(env, argv[0]);
  bool useBigint = argc > 1 && getBool(env, argv[1], false);

  napi_value asyncReq = getAsyncReq(env, argc, argv, 2, fsData);
  if (asyncReq) {
    auto *wrap = new FSReqWrap();
    wrap->resultType = FSReqResultType::Stat;
    wrap->path = path;
    wrap->useBigint = useBigint;
    napi_value result = startAsyncFsOp(env, wrap, asyncReq, fsData);
    uv_fs_stat(s_fsLoop, &wrap->req, path.c_str(), fsAfterAsync);
    return result;
  }

  // throwIfNoEntry: argv[3] (default true for statSync)
  bool throwIfNoEntry = true;
  if (argc > 3 && !isNullOrUndefined(env, argv[3])) {
    throwIfNoEntry = getBool(env, argv[3], true);
  }

  uv_fs_t req;
  int result = uv_fs_stat(nullptr, &req, path.c_str(), nullptr);
  if (result < 0) {
    uv_fs_req_cleanup(&req);
    if (!throwIfNoEntry &&
        (result == UV_ENOENT || result == UV_ENOTDIR || result == UV_ELOOP)) {
      return nullptr; // return undefined
    }
    return throwUVException(env, result, "stat", path.c_str());
  }

  napi_value jsResult =
      fillAndReturnStats(env, fsData, &req.statbuf, useBigint);
  uv_fs_req_cleanup(&req);
  return jsResult;
}

// binding.lstat(path, useBigint, req?, throwIfNoEntry?)
static napi_value fsLstat(napi_env env, napi_callback_info info) {
  size_t argc = 4;
  napi_value argv[4];
  FsBindingData *fsData = getFsData(env, info);
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  std::string path = getStringArg(env, argv[0]);
  bool useBigint = argc > 1 && getBool(env, argv[1], false);

  napi_value asyncReq = getAsyncReq(env, argc, argv, 2, fsData);
  if (asyncReq) {
    auto *wrap = new FSReqWrap();
    wrap->resultType = FSReqResultType::Stat;
    wrap->path = path;
    wrap->useBigint = useBigint;
    napi_value result = startAsyncFsOp(env, wrap, asyncReq, fsData);
    uv_fs_lstat(s_fsLoop, &wrap->req, path.c_str(), fsAfterAsync);
    return result;
  }

  bool throwIfNoEntry = true;
  if (argc > 3 && !isNullOrUndefined(env, argv[3])) {
    throwIfNoEntry = getBool(env, argv[3], true);
  }

  uv_fs_t req;
  int result = uv_fs_lstat(nullptr, &req, path.c_str(), nullptr);
  if (result < 0) {
    uv_fs_req_cleanup(&req);
    if (!throwIfNoEntry &&
        (result == UV_ENOENT || result == UV_ENOTDIR || result == UV_ELOOP)) {
      return nullptr;
    }
    return throwUVException(env, result, "lstat", path.c_str());
  }

  napi_value jsResult =
      fillAndReturnStats(env, fsData, &req.statbuf, useBigint);
  uv_fs_req_cleanup(&req);
  return jsResult;
}

// binding.fstat(fd, useBigint, req?, shouldNotThrow?)
static napi_value fsFstat(napi_env env, napi_callback_info info) {
  size_t argc = 4;
  napi_value argv[4];
  FsBindingData *fsData = getFsData(env, info);
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  int fd = getInt32(env, argv[0]);
  bool useBigint = argc > 1 && getBool(env, argv[1], false);

  napi_value asyncReq = getAsyncReq(env, argc, argv, 2, fsData);
  if (asyncReq) {
    auto *wrap = new FSReqWrap();
    wrap->resultType = FSReqResultType::Stat;
    wrap->useBigint = useBigint;
    napi_value result = startAsyncFsOp(env, wrap, asyncReq, fsData);
    uv_fs_fstat(s_fsLoop, &wrap->req, fd, fsAfterAsync);
    return result;
  }

  bool shouldNotThrow = false;
  if (argc > 3 && !isNullOrUndefined(env, argv[3])) {
    shouldNotThrow = getBool(env, argv[3], false);
  }

  uv_fs_t req;
  int result = uv_fs_fstat(nullptr, &req, fd, nullptr);
  if (result < 0) {
    uv_fs_req_cleanup(&req);
    if (shouldNotThrow) {
      return nullptr;
    }
    return throwUVException(env, result, "fstat");
  }

  napi_value jsResult =
      fillAndReturnStats(env, fsData, &req.statbuf, useBigint);
  uv_fs_req_cleanup(&req);
  return jsResult;
}

// binding.statfs(path, useBigint, req?)
static napi_value fsStatFs(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3];
  FsBindingData *fsData = getFsData(env, info);
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  std::string path = getStringArg(env, argv[0]);
  bool useBigint = argc > 1 && getBool(env, argv[1], false);

  napi_value asyncReq = getAsyncReq(env, argc, argv, 2, fsData);
  if (asyncReq) {
    auto *wrap = new FSReqWrap();
    wrap->resultType = FSReqResultType::StatFs;
    wrap->path = path;
    wrap->useBigint = useBigint;
    napi_value result = startAsyncFsOp(env, wrap, asyncReq, fsData);
    uv_fs_statfs(s_fsLoop, &wrap->req, path.c_str(), fsAfterAsync);
    return result;
  }

  uv_fs_t req;
  int result = uv_fs_statfs(nullptr, &req, path.c_str(), nullptr);
  if (result < 0) {
    uv_fs_req_cleanup(&req);
    return throwUVException(env, result, "statfs", path.c_str());
  }

  auto *sf = static_cast<uv_statfs_t *>(req.ptr);
  napi_value jsResult;

  if (useBigint) {
    napi_get_reference_value(env, fsData->bigintStatFsValuesRef, &jsResult);
    napi_typedarray_type type;
    size_t length;
    void *data;
    napi_get_typedarray_info(
        env, jsResult, &type, &length, &data, nullptr, nullptr);
    auto *buf = static_cast<int64_t *>(data);
    buf[kType] = static_cast<int64_t>(sf->f_type);
    buf[kBSize] = static_cast<int64_t>(sf->f_bsize);
    buf[kStatFsBlocks] = static_cast<int64_t>(sf->f_blocks);
    buf[kBFree] = static_cast<int64_t>(sf->f_bfree);
    buf[kBAvail] = static_cast<int64_t>(sf->f_bavail);
    buf[kFiles] = static_cast<int64_t>(sf->f_files);
    buf[kFFree] = static_cast<int64_t>(sf->f_ffree);
  } else {
    napi_get_reference_value(env, fsData->statFsValuesRef, &jsResult);
    napi_typedarray_type type;
    size_t length;
    void *data;
    napi_get_typedarray_info(
        env, jsResult, &type, &length, &data, nullptr, nullptr);
    auto *buf = static_cast<double *>(data);
    buf[kType] = static_cast<double>(sf->f_type);
    buf[kBSize] = static_cast<double>(sf->f_bsize);
    buf[kStatFsBlocks] = static_cast<double>(sf->f_blocks);
    buf[kBFree] = static_cast<double>(sf->f_bfree);
    buf[kBAvail] = static_cast<double>(sf->f_bavail);
    buf[kFiles] = static_cast<double>(sf->f_files);
    buf[kFFree] = static_cast<double>(sf->f_ffree);
  }

  uv_fs_req_cleanup(&req);
  return jsResult;
}

// binding.rename(oldPath, newPath, req?)
static napi_value fsRename(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3];
  FsBindingData *fsData = getFsData(env, info);
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  std::string oldPath = getStringArg(env, argv[0]);
  std::string newPath = getStringArg(env, argv[1]);

  napi_value asyncReq = getAsyncReq(env, argc, argv, 2, fsData);
  if (asyncReq) {
    auto *wrap = new FSReqWrap();
    wrap->resultType = FSReqResultType::Void;
    wrap->path = oldPath;
    wrap->dest = newPath;
    napi_value result = startAsyncFsOp(env, wrap, asyncReq, fsData);
    uv_fs_rename(
        s_fsLoop, &wrap->req, oldPath.c_str(), newPath.c_str(), fsAfterAsync);
    return result;
  }

  uv_fs_t req;
  int result =
      uv_fs_rename(nullptr, &req, oldPath.c_str(), newPath.c_str(), nullptr);
  uv_fs_req_cleanup(&req);
  if (result < 0) {
    return throwUVException(
        env, result, "rename", oldPath.c_str(), newPath.c_str());
  }
  return nullptr;
}

// binding.unlink(path, req?)
static napi_value fsUnlink(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  FsBindingData *fsData = getFsData(env, info);
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  std::string path = getStringArg(env, argv[0]);

  napi_value asyncReq = getAsyncReq(env, argc, argv, 1, fsData);
  if (asyncReq) {
    auto *wrap = new FSReqWrap();
    wrap->resultType = FSReqResultType::Void;
    wrap->path = path;
    napi_value result = startAsyncFsOp(env, wrap, asyncReq, fsData);
    uv_fs_unlink(s_fsLoop, &wrap->req, path.c_str(), fsAfterAsync);
    return result;
  }

  uv_fs_t req;
  int result = uv_fs_unlink(nullptr, &req, path.c_str(), nullptr);
  uv_fs_req_cleanup(&req);
  if (result < 0) {
    return throwUVException(env, result, "unlink", path.c_str());
  }
  return nullptr;
}

// binding.mkdir(path, mode, recursive, req?)
static napi_value fsMkdir(napi_env env, napi_callback_info info) {
  size_t argc = 4;
  napi_value argv[4];
  FsBindingData *fsData = getFsData(env, info);
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  std::string path = getStringArg(env, argv[0]);
  int mode = getInt32(env, argv[1], 0777);
  bool recursive = argc > 2 && getBool(env, argv[2], false);

  napi_value asyncReq = getAsyncReq(env, argc, argv, 3, fsData);
  if (asyncReq) {
    if (recursive) {
      // For async recursive mkdir, do it synchronously on the main thread
      // (it's a series of mkdir calls) and then call the callback.
      // This matches what most implementations do for simplicity.
      auto *wrap = new FSReqWrap();
      wrap->resultType = FSReqResultType::MkdirResult;
      wrap->path = path;

      // Do recursive mkdir synchronously.
      std::string firstCreated;
      std::string current;
      size_t pos = 0;
      if (!path.empty() && path[0] == '/') {
        current = "/";
        pos = 1;
      }
      bool hadError = false;
      int errResult = 0;
      while (pos <= path.size()) {
        size_t slash = path.find('/', pos);
        if (slash == std::string::npos)
          slash = path.size();
        std::string component = path.substr(pos, slash - pos);
        pos = slash + 1;
        if (component.empty() || component == ".")
          continue;
        if (!current.empty() && current.back() != '/')
          current += '/';
        current += component;
        uv_fs_t mkReq;
        int mkResult =
            uv_fs_mkdir(nullptr, &mkReq, current.c_str(), mode, nullptr);
        uv_fs_req_cleanup(&mkReq);
        if (mkResult == 0) {
          if (firstCreated.empty())
            firstCreated = current;
        } else if (mkResult != UV_EEXIST) {
          hadError = true;
          errResult = mkResult;
          wrap->path = current;
          break;
        }
      }

      wrap->firstCreated = firstCreated;
      napi_value result = startAsyncFsOp(env, wrap, asyncReq, fsData);

      if (hadError) {
        // Simulate error by setting req.result and calling callback.
        wrap->req.result = errResult;
        fsAfterAsync(&wrap->req);
      } else {
        // Simulate success.
        wrap->req.result = 0;
        fsAfterAsync(&wrap->req);
      }
      return result;
    }

    auto *wrap = new FSReqWrap();
    wrap->resultType = FSReqResultType::Void;
    wrap->path = path;
    napi_value result = startAsyncFsOp(env, wrap, asyncReq, fsData);
    uv_fs_mkdir(s_fsLoop, &wrap->req, path.c_str(), mode, fsAfterAsync);
    return result;
  }

  if (!recursive) {
    uv_fs_t req;
    int result = uv_fs_mkdir(nullptr, &req, path.c_str(), mode, nullptr);
    uv_fs_req_cleanup(&req);
    if (result < 0) {
      return throwUVException(env, result, "mkdir", path.c_str());
    }
    return nullptr;
  }

  // Recursive mkdir: create each component.
  std::string firstCreated;
  std::string current;
  size_t pos = 0;

  if (!path.empty() && path[0] == '/') {
    current = "/";
    pos = 1;
  }

  while (pos <= path.size()) {
    size_t slash = path.find('/', pos);
    if (slash == std::string::npos)
      slash = path.size();

    std::string component = path.substr(pos, slash - pos);
    pos = slash + 1;

    if (component.empty() || component == ".")
      continue;

    if (!current.empty() && current.back() != '/')
      current += '/';
    current += component;

    uv_fs_t req;
    int result = uv_fs_mkdir(nullptr, &req, current.c_str(), mode, nullptr);
    uv_fs_req_cleanup(&req);

    if (result == 0) {
      if (firstCreated.empty())
        firstCreated = current;
    } else if (result != UV_EEXIST) {
      return throwUVException(env, result, "mkdir", path.c_str());
    }
    // UV_EEXIST: path exists — continue (may be a dir or a file).
  }

  // After iterating all components, verify the full path is a directory.
  // If it's a file (or doesn't exist), throw the appropriate error.
  {
    uv_fs_t statReq;
    int statRes = uv_fs_stat(nullptr, &statReq, path.c_str(), nullptr);
    if (statRes == 0) {
      bool isDir = (uv_fs_get_statbuf(&statReq)->st_mode & S_IFMT) == S_IFDIR;
      uv_fs_req_cleanup(&statReq);
      if (!isDir) {
        return throwUVException(env, UV_EEXIST, "mkdir", path.c_str());
      }
    } else {
      uv_fs_req_cleanup(&statReq);
      // The path doesn't exist — an intermediate component was a file.
      return throwUVException(env, UV_ENOTDIR, "mkdir", path.c_str());
    }
  }

  if (!firstCreated.empty()) {
    napi_value jsResult;
    napi_create_string_utf8(
        env, firstCreated.c_str(), firstCreated.size(), &jsResult);
    return jsResult;
  }
  return nullptr;
}

// binding.rmdir(path, req?)
static napi_value fsRmdir(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  FsBindingData *fsData = getFsData(env, info);
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  std::string path = getStringArg(env, argv[0]);

  napi_value asyncReq = getAsyncReq(env, argc, argv, 1, fsData);
  if (asyncReq) {
    auto *wrap = new FSReqWrap();
    wrap->resultType = FSReqResultType::Void;
    wrap->path = path;
    napi_value result = startAsyncFsOp(env, wrap, asyncReq, fsData);
    uv_fs_rmdir(s_fsLoop, &wrap->req, path.c_str(), fsAfterAsync);
    return result;
  }

  uv_fs_t req;
  int result = uv_fs_rmdir(nullptr, &req, path.c_str(), nullptr);
  uv_fs_req_cleanup(&req);
  if (result < 0) {
    return throwUVException(env, result, "rmdir", path.c_str());
  }
  return nullptr;
}

// binding.readdir(path, encoding, withFileTypes, req?)
static napi_value fsReaddir(napi_env env, napi_callback_info info) {
  size_t argc = 4;
  napi_value argv[4];
  FsBindingData *fsData = getFsData(env, info);
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  std::string path = getStringArg(env, argv[0]);
  // encoding is argv[1] - we always return UTF-8 strings.
  bool withFileTypes = argc > 2 && getBool(env, argv[2], false);

  napi_value asyncReq = getAsyncReq(env, argc, argv, 3, fsData);
  if (asyncReq) {
    auto *wrap = new FSReqWrap();
    wrap->resultType = FSReqResultType::Readdir;
    wrap->path = path;
    wrap->withFileTypes = withFileTypes;
    napi_value result = startAsyncFsOp(env, wrap, asyncReq, fsData);
    uv_fs_scandir(s_fsLoop, &wrap->req, path.c_str(), 0, fsAfterAsync);
    return result;
  }

  uv_fs_t req;
  int result = uv_fs_scandir(nullptr, &req, path.c_str(), 0, nullptr);
  if (result < 0) {
    uv_fs_req_cleanup(&req);
    return throwUVException(env, result, "scandir", path.c_str());
  }

  // Collect entries.
  std::vector<std::string> names;
  std::vector<int> types;
  uv_dirent_t ent;
  while (uv_fs_scandir_next(&req, &ent) == 0) {
    names.push_back(ent.name);
    if (withFileTypes)
      types.push_back(static_cast<int>(ent.type));
  }
  uv_fs_req_cleanup(&req);

  if (withFileTypes) {
    // Return [namesArray, typesArray].
    napi_value namesArr;
    napi_create_array_with_length(env, names.size(), &namesArr);
    for (size_t i = 0; i < names.size(); i++) {
      napi_value str;
      napi_create_string_utf8(env, names[i].c_str(), names[i].size(), &str);
      napi_set_element(env, namesArr, static_cast<uint32_t>(i), str);
    }

    napi_value typesArr;
    napi_create_array_with_length(env, types.size(), &typesArr);
    for (size_t i = 0; i < types.size(); i++) {
      napi_value num;
      napi_create_int32(env, types[i], &num);
      napi_set_element(env, typesArr, static_cast<uint32_t>(i), num);
    }

    napi_value resultArr;
    napi_create_array_with_length(env, 2, &resultArr);
    napi_set_element(env, resultArr, 0, namesArr);
    napi_set_element(env, resultArr, 1, typesArr);
    return resultArr;
  } else {
    napi_value namesArr;
    napi_create_array_with_length(env, names.size(), &namesArr);
    for (size_t i = 0; i < names.size(); i++) {
      napi_value str;
      napi_create_string_utf8(env, names[i].c_str(), names[i].size(), &str);
      napi_set_element(env, namesArr, static_cast<uint32_t>(i), str);
    }
    return namesArr;
  }
}

// binding.chmod(path, mode, req?)
static napi_value fsChmod(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3];
  FsBindingData *fsData = getFsData(env, info);
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  std::string path = getStringArg(env, argv[0]);
  int mode = getInt32(env, argv[1]);

  napi_value asyncReq = getAsyncReq(env, argc, argv, 2, fsData);
  if (asyncReq) {
    auto *wrap = new FSReqWrap();
    wrap->resultType = FSReqResultType::Void;
    wrap->path = path;
    napi_value result = startAsyncFsOp(env, wrap, asyncReq, fsData);
    uv_fs_chmod(s_fsLoop, &wrap->req, path.c_str(), mode, fsAfterAsync);
    return result;
  }

  uv_fs_t req;
  int result = uv_fs_chmod(nullptr, &req, path.c_str(), mode, nullptr);
  uv_fs_req_cleanup(&req);
  if (result < 0) {
    return throwUVException(env, result, "chmod", path.c_str());
  }
  return nullptr;
}

// binding.fchmod(fd, mode, req?)
static napi_value fsFchmod(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3];
  FsBindingData *fsData = getFsData(env, info);
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  int fd = getInt32(env, argv[0]);
  int mode = getInt32(env, argv[1]);

  napi_value asyncReq = getAsyncReq(env, argc, argv, 2, fsData);
  if (asyncReq) {
    auto *wrap = new FSReqWrap();
    wrap->resultType = FSReqResultType::Void;
    napi_value result = startAsyncFsOp(env, wrap, asyncReq, fsData);
    uv_fs_fchmod(s_fsLoop, &wrap->req, fd, mode, fsAfterAsync);
    return result;
  }

  uv_fs_t req;
  int result = uv_fs_fchmod(nullptr, &req, fd, mode, nullptr);
  uv_fs_req_cleanup(&req);
  if (result < 0) {
    return throwUVException(env, result, "fchmod");
  }
  return nullptr;
}

// binding.chown(path, uid, gid, req?)
static napi_value fsChown(napi_env env, napi_callback_info info) {
  size_t argc = 4;
  napi_value argv[4];
  FsBindingData *fsData = getFsData(env, info);
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  std::string path = getStringArg(env, argv[0]);
  int uid = getInt32(env, argv[1]);
  int gid = getInt32(env, argv[2]);

  napi_value asyncReq = getAsyncReq(env, argc, argv, 3, fsData);
  if (asyncReq) {
    auto *wrap = new FSReqWrap();
    wrap->resultType = FSReqResultType::Void;
    wrap->path = path;
    napi_value result = startAsyncFsOp(env, wrap, asyncReq, fsData);
    uv_fs_chown(s_fsLoop, &wrap->req, path.c_str(), uid, gid, fsAfterAsync);
    return result;
  }

  uv_fs_t req;
  int result = uv_fs_chown(nullptr, &req, path.c_str(), uid, gid, nullptr);
  uv_fs_req_cleanup(&req);
  if (result < 0) {
    return throwUVException(env, result, "chown", path.c_str());
  }
  return nullptr;
}

// binding.fchown(fd, uid, gid, req?)
static napi_value fsFchown(napi_env env, napi_callback_info info) {
  size_t argc = 4;
  napi_value argv[4];
  FsBindingData *fsData = getFsData(env, info);
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  int fd = getInt32(env, argv[0]);
  int uid = getInt32(env, argv[1]);
  int gid = getInt32(env, argv[2]);

  napi_value asyncReq = getAsyncReq(env, argc, argv, 3, fsData);
  if (asyncReq) {
    auto *wrap = new FSReqWrap();
    wrap->resultType = FSReqResultType::Void;
    napi_value result = startAsyncFsOp(env, wrap, asyncReq, fsData);
    uv_fs_fchown(s_fsLoop, &wrap->req, fd, uid, gid, fsAfterAsync);
    return result;
  }

  uv_fs_t req;
  int result = uv_fs_fchown(nullptr, &req, fd, uid, gid, nullptr);
  uv_fs_req_cleanup(&req);
  if (result < 0) {
    return throwUVException(env, result, "fchown");
  }
  return nullptr;
}

// binding.lchown(path, uid, gid, req?)
static napi_value fsLchown(napi_env env, napi_callback_info info) {
  size_t argc = 4;
  napi_value argv[4];
  FsBindingData *fsData = getFsData(env, info);
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  std::string path = getStringArg(env, argv[0]);
  int uid = getInt32(env, argv[1]);
  int gid = getInt32(env, argv[2]);

  napi_value asyncReq = getAsyncReq(env, argc, argv, 3, fsData);
  if (asyncReq) {
    auto *wrap = new FSReqWrap();
    wrap->resultType = FSReqResultType::Void;
    wrap->path = path;
    napi_value result = startAsyncFsOp(env, wrap, asyncReq, fsData);
    uv_fs_lchown(s_fsLoop, &wrap->req, path.c_str(), uid, gid, fsAfterAsync);
    return result;
  }

  uv_fs_t req;
  int result = uv_fs_lchown(nullptr, &req, path.c_str(), uid, gid, nullptr);
  uv_fs_req_cleanup(&req);
  if (result < 0) {
    return throwUVException(env, result, "lchown", path.c_str());
  }
  return nullptr;
}

// binding.link(existingPath, newPath, req?)
static napi_value fsLink(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3];
  FsBindingData *fsData = getFsData(env, info);
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  std::string existingPath = getStringArg(env, argv[0]);
  std::string newPath = getStringArg(env, argv[1]);

  napi_value asyncReq = getAsyncReq(env, argc, argv, 2, fsData);
  if (asyncReq) {
    auto *wrap = new FSReqWrap();
    wrap->resultType = FSReqResultType::Void;
    wrap->path = existingPath;
    wrap->dest = newPath;
    napi_value result = startAsyncFsOp(env, wrap, asyncReq, fsData);
    uv_fs_link(
        s_fsLoop,
        &wrap->req,
        existingPath.c_str(),
        newPath.c_str(),
        fsAfterAsync);
    return result;
  }

  uv_fs_t req;
  int result =
      uv_fs_link(nullptr, &req, existingPath.c_str(), newPath.c_str(), nullptr);
  uv_fs_req_cleanup(&req);
  if (result < 0) {
    return throwUVException(
        env, result, "link", existingPath.c_str(), newPath.c_str());
  }
  return nullptr;
}

// binding.symlink(target, path, flags, req?)
static napi_value fsSymlink(napi_env env, napi_callback_info info) {
  size_t argc = 4;
  napi_value argv[4];
  FsBindingData *fsData = getFsData(env, info);
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  std::string target = getStringArg(env, argv[0]);
  std::string path = getStringArg(env, argv[1]);
  int flags = argc > 2 ? getInt32(env, argv[2], 0) : 0;

  napi_value asyncReq = getAsyncReq(env, argc, argv, 3, fsData);
  if (asyncReq) {
    auto *wrap = new FSReqWrap();
    wrap->resultType = FSReqResultType::Void;
    wrap->path = target;
    wrap->dest = path;
    napi_value result = startAsyncFsOp(env, wrap, asyncReq, fsData);
    uv_fs_symlink(
        s_fsLoop,
        &wrap->req,
        target.c_str(),
        path.c_str(),
        flags,
        fsAfterAsync);
    return result;
  }

  uv_fs_t req;
  int result = uv_fs_symlink(
      nullptr, &req, target.c_str(), path.c_str(), flags, nullptr);
  uv_fs_req_cleanup(&req);
  if (result < 0) {
    return throwUVException(
        env, result, "symlink", target.c_str(), path.c_str());
  }
  return nullptr;
}

// binding.readlink(path, encoding, req?)
static napi_value fsReadlink(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3];
  FsBindingData *fsData = getFsData(env, info);
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  std::string path = getStringArg(env, argv[0]);

  napi_value asyncReq = getAsyncReq(env, argc, argv, 2, fsData);
  if (asyncReq) {
    auto *wrap = new FSReqWrap();
    wrap->resultType = FSReqResultType::StringPtr;
    wrap->path = path;
    napi_value result = startAsyncFsOp(env, wrap, asyncReq, fsData);
    uv_fs_readlink(s_fsLoop, &wrap->req, path.c_str(), fsAfterAsync);
    return result;
  }

  uv_fs_t req;
  int result = uv_fs_readlink(nullptr, &req, path.c_str(), nullptr);
  if (result < 0) {
    uv_fs_req_cleanup(&req);
    return throwUVException(env, result, "readlink", path.c_str());
  }

  auto *linkPath = static_cast<const char *>(req.ptr);
  napi_value jsResult;
  napi_create_string_utf8(env, linkPath, NAPI_AUTO_LENGTH, &jsResult);
  uv_fs_req_cleanup(&req);
  return jsResult;
}

// binding.realpath(path, encoding, req?)
static napi_value fsRealpath(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3];
  FsBindingData *fsData = getFsData(env, info);
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  std::string path = getStringArg(env, argv[0]);

  napi_value asyncReq = getAsyncReq(env, argc, argv, 2, fsData);
  if (asyncReq) {
    auto *wrap = new FSReqWrap();
    wrap->resultType = FSReqResultType::StringPtr;
    wrap->path = path;
    napi_value result = startAsyncFsOp(env, wrap, asyncReq, fsData);
    uv_fs_realpath(s_fsLoop, &wrap->req, path.c_str(), fsAfterAsync);
    return result;
  }

  uv_fs_t req;
  int result = uv_fs_realpath(nullptr, &req, path.c_str(), nullptr);
  if (result < 0) {
    uv_fs_req_cleanup(&req);
    return throwUVException(env, result, "realpath", path.c_str());
  }

  auto *resolved = static_cast<const char *>(req.ptr);
  napi_value jsResult;
  napi_create_string_utf8(env, resolved, NAPI_AUTO_LENGTH, &jsResult);
  uv_fs_req_cleanup(&req);
  return jsResult;
}

// binding.ftruncate(fd, len, req?)
static napi_value fsFtruncate(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3];
  FsBindingData *fsData = getFsData(env, info);
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  int fd = getInt32(env, argv[0]);
  int64_t len = getInt64(env, argv[1], 0);

  napi_value asyncReq = getAsyncReq(env, argc, argv, 2, fsData);
  if (asyncReq) {
    auto *wrap = new FSReqWrap();
    wrap->resultType = FSReqResultType::Void;
    napi_value result = startAsyncFsOp(env, wrap, asyncReq, fsData);
    uv_fs_ftruncate(s_fsLoop, &wrap->req, fd, len, fsAfterAsync);
    return result;
  }

  uv_fs_t req;
  int result = uv_fs_ftruncate(nullptr, &req, fd, len, nullptr);
  uv_fs_req_cleanup(&req);
  if (result < 0) {
    return throwUVException(env, result, "ftruncate");
  }
  return nullptr;
}

// binding.utimes(path, atime, mtime, req?)
static napi_value fsUtimes(napi_env env, napi_callback_info info) {
  size_t argc = 4;
  napi_value argv[4];
  FsBindingData *fsData = getFsData(env, info);
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  std::string path = getStringArg(env, argv[0]);
  double atime = getDouble(env, argv[1]);
  double mtime = getDouble(env, argv[2]);

  napi_value asyncReq = getAsyncReq(env, argc, argv, 3, fsData);
  if (asyncReq) {
    auto *wrap = new FSReqWrap();
    wrap->resultType = FSReqResultType::Void;
    wrap->path = path;
    napi_value result = startAsyncFsOp(env, wrap, asyncReq, fsData);
    uv_fs_utime(s_fsLoop, &wrap->req, path.c_str(), atime, mtime, fsAfterAsync);
    return result;
  }

  uv_fs_t req;
  int result = uv_fs_utime(nullptr, &req, path.c_str(), atime, mtime, nullptr);
  uv_fs_req_cleanup(&req);
  if (result < 0) {
    return throwUVException(env, result, "utime", path.c_str());
  }
  return nullptr;
}

// binding.futimes(fd, atime, mtime, req?)
static napi_value fsFutimes(napi_env env, napi_callback_info info) {
  size_t argc = 4;
  napi_value argv[4];
  FsBindingData *fsData = getFsData(env, info);
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  int fd = getInt32(env, argv[0]);
  double atime = getDouble(env, argv[1]);
  double mtime = getDouble(env, argv[2]);

  napi_value asyncReq = getAsyncReq(env, argc, argv, 3, fsData);
  if (asyncReq) {
    auto *wrap = new FSReqWrap();
    wrap->resultType = FSReqResultType::Void;
    napi_value result = startAsyncFsOp(env, wrap, asyncReq, fsData);
    uv_fs_futime(s_fsLoop, &wrap->req, fd, atime, mtime, fsAfterAsync);
    return result;
  }

  uv_fs_t req;
  int result = uv_fs_futime(nullptr, &req, fd, atime, mtime, nullptr);
  uv_fs_req_cleanup(&req);
  if (result < 0) {
    return throwUVException(env, result, "futime");
  }
  return nullptr;
}

// binding.lutimes(path, atime, mtime, req?)
static napi_value fsLutimes(napi_env env, napi_callback_info info) {
  size_t argc = 4;
  napi_value argv[4];
  FsBindingData *fsData = getFsData(env, info);
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  std::string path = getStringArg(env, argv[0]);
  double atime = getDouble(env, argv[1]);
  double mtime = getDouble(env, argv[2]);

  napi_value asyncReq = getAsyncReq(env, argc, argv, 3, fsData);
  if (asyncReq) {
    auto *wrap = new FSReqWrap();
    wrap->resultType = FSReqResultType::Void;
    wrap->path = path;
    napi_value result = startAsyncFsOp(env, wrap, asyncReq, fsData);
    uv_fs_lutime(
        s_fsLoop, &wrap->req, path.c_str(), atime, mtime, fsAfterAsync);
    return result;
  }

  uv_fs_t req;
  int result = uv_fs_lutime(nullptr, &req, path.c_str(), atime, mtime, nullptr);
  uv_fs_req_cleanup(&req);
  if (result < 0) {
    return throwUVException(env, result, "lutime", path.c_str());
  }
  return nullptr;
}

// binding.mkdtemp(prefix, encoding, req?)
static napi_value fsMkdtemp(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3];
  FsBindingData *fsData = getFsData(env, info);
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  std::string prefix = getStringArg(env, argv[0]);
  std::string tmpl = prefix + "XXXXXX";

  napi_value asyncReq = getAsyncReq(env, argc, argv, 2, fsData);
  if (asyncReq) {
    auto *wrap = new FSReqWrap();
    wrap->resultType = FSReqResultType::StringPath;
    wrap->path = prefix;
    napi_value result = startAsyncFsOp(env, wrap, asyncReq, fsData);
    uv_fs_mkdtemp(s_fsLoop, &wrap->req, tmpl.c_str(), fsAfterAsync);
    return result;
  }

  uv_fs_t req;
  int result = uv_fs_mkdtemp(nullptr, &req, tmpl.c_str(), nullptr);
  if (result < 0) {
    uv_fs_req_cleanup(&req);
    return throwUVException(env, result, "mkdtemp", prefix.c_str());
  }

  napi_value jsResult;
  napi_create_string_utf8(env, req.path, NAPI_AUTO_LENGTH, &jsResult);
  uv_fs_req_cleanup(&req);
  return jsResult;
}

// binding.copyFile(src, dest, mode, req?)
static napi_value fsCopyFile(napi_env env, napi_callback_info info) {
  size_t argc = 4;
  napi_value argv[4];
  FsBindingData *fsData = getFsData(env, info);
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  std::string src = getStringArg(env, argv[0]);
  std::string dest = getStringArg(env, argv[1]);
  int flags = argc > 2 ? getInt32(env, argv[2], 0) : 0;

  napi_value asyncReq = getAsyncReq(env, argc, argv, 3, fsData);
  if (asyncReq) {
    auto *wrap = new FSReqWrap();
    wrap->resultType = FSReqResultType::Void;
    wrap->path = src;
    wrap->dest = dest;
    napi_value result = startAsyncFsOp(env, wrap, asyncReq, fsData);
    uv_fs_copyfile(
        s_fsLoop, &wrap->req, src.c_str(), dest.c_str(), flags, fsAfterAsync);
    return result;
  }

  uv_fs_t req;
  int result =
      uv_fs_copyfile(nullptr, &req, src.c_str(), dest.c_str(), flags, nullptr);
  uv_fs_req_cleanup(&req);
  if (result < 0) {
    return throwUVException(env, result, "copyfile", src.c_str(), dest.c_str());
  }
  return nullptr;
}

// binding.existsSync(path)
static napi_value fsExistsSync(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  std::string path = getStringArg(env, argv[0]);

  uv_fs_t req;
  int result = uv_fs_access(nullptr, &req, path.c_str(), 0 /* F_OK */, nullptr);
  uv_fs_req_cleanup(&req);

  napi_value jsResult;
  napi_get_boolean(env, result == 0, &jsResult);
  return jsResult;
}

// binding.fsync(fd, req?)
static napi_value fsFsync(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  FsBindingData *fsData = getFsData(env, info);
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  int fd = getInt32(env, argv[0]);

  napi_value asyncReq = getAsyncReq(env, argc, argv, 1, fsData);
  if (asyncReq) {
    auto *wrap = new FSReqWrap();
    wrap->resultType = FSReqResultType::Void;
    napi_value result = startAsyncFsOp(env, wrap, asyncReq, fsData);
    uv_fs_fsync(s_fsLoop, &wrap->req, fd, fsAfterAsync);
    return result;
  }

  uv_fs_t req;
  int result = uv_fs_fsync(nullptr, &req, fd, nullptr);
  uv_fs_req_cleanup(&req);
  if (result < 0) {
    return throwUVException(env, result, "fsync");
  }
  return nullptr;
}

// binding.fdatasync(fd, req?)
static napi_value fsFdatasync(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  FsBindingData *fsData = getFsData(env, info);
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  int fd = getInt32(env, argv[0]);

  napi_value asyncReq = getAsyncReq(env, argc, argv, 1, fsData);
  if (asyncReq) {
    auto *wrap = new FSReqWrap();
    wrap->resultType = FSReqResultType::Void;
    napi_value result = startAsyncFsOp(env, wrap, asyncReq, fsData);
    uv_fs_fdatasync(s_fsLoop, &wrap->req, fd, fsAfterAsync);
    return result;
  }

  uv_fs_t req;
  int result = uv_fs_fdatasync(nullptr, &req, fd, nullptr);
  uv_fs_req_cleanup(&req);
  if (result < 0) {
    return throwUVException(env, result, "fdatasync");
  }
  return nullptr;
}

// binding.readFileUtf8(path, flags)
// Fast path for reading entire file as UTF-8 string (sync only).
static napi_value fsReadFileUtf8(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  std::string path = getStringArg(env, argv[0]);
  int flags = getInt32(env, argv[1], 0);

  // Open.
  uv_fs_t openReq;
  int fd = uv_fs_open(nullptr, &openReq, path.c_str(), flags, 0666, nullptr);
  uv_fs_req_cleanup(&openReq);
  if (fd < 0) {
    return throwUVException(env, fd, "open", path.c_str());
  }

  // Stat to get size.
  uv_fs_t statReq;
  int statResult = uv_fs_fstat(nullptr, &statReq, fd, nullptr);
  int64_t fileSize = 0;
  if (statResult == 0) {
    fileSize = statReq.statbuf.st_size;
  }
  uv_fs_req_cleanup(&statReq);

  // Read.
  std::string content;
  if (fileSize > 0) {
    content.resize(static_cast<size_t>(fileSize));
    uv_buf_t buf =
        uv_buf_init(&content[0], static_cast<unsigned int>(fileSize));
    uv_fs_t readReq;
    int bytesRead = uv_fs_read(nullptr, &readReq, fd, &buf, 1, 0, nullptr);
    uv_fs_req_cleanup(&readReq);
    if (bytesRead < 0) {
      uv_fs_t closeReq;
      uv_fs_close(nullptr, &closeReq, fd, nullptr);
      uv_fs_req_cleanup(&closeReq);
      return throwUVException(env, bytesRead, "read", path.c_str());
    }
    content.resize(static_cast<size_t>(bytesRead));
  } else {
    // Size unknown (e.g., /proc files). Read in chunks.
    char chunk[8192];
    for (;;) {
      uv_buf_t buf = uv_buf_init(chunk, sizeof(chunk));
      uv_fs_t readReq;
      int bytesRead = uv_fs_read(nullptr, &readReq, fd, &buf, 1, -1, nullptr);
      uv_fs_req_cleanup(&readReq);
      if (bytesRead < 0) {
        uv_fs_t closeReq;
        uv_fs_close(nullptr, &closeReq, fd, nullptr);
        uv_fs_req_cleanup(&closeReq);
        return throwUVException(env, bytesRead, "read", path.c_str());
      }
      if (bytesRead == 0)
        break;
      content.append(chunk, static_cast<size_t>(bytesRead));
    }
  }

  // Close.
  uv_fs_t closeReq;
  uv_fs_close(nullptr, &closeReq, fd, nullptr);
  uv_fs_req_cleanup(&closeReq);

  napi_value jsResult;
  napi_create_string_utf8(env, content.data(), content.size(), &jsResult);
  return jsResult;
}

// binding.writeFileUtf8(path, data, flags, mode)
// Fast path for writing string data as UTF-8 (sync only).
static napi_value fsWriteFileUtf8(napi_env env, napi_callback_info info) {
  size_t argc = 4;
  napi_value argv[4];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  std::string path = getStringArg(env, argv[0]);
  std::string data = getStringArg(env, argv[1]);
  int flags = getInt32(env, argv[2], 0);
  int mode = getInt32(env, argv[3], 0666);

  // Open.
  uv_fs_t openReq;
  int fd = uv_fs_open(nullptr, &openReq, path.c_str(), flags, mode, nullptr);
  uv_fs_req_cleanup(&openReq);
  if (fd < 0) {
    return throwUVException(env, fd, "open", path.c_str());
  }

  // Write all data.
  size_t offset = 0;
  while (offset < data.size()) {
    uv_buf_t buf = uv_buf_init(
        const_cast<char *>(data.data() + offset),
        static_cast<unsigned int>(data.size() - offset));
    uv_fs_t writeReq;
    int written = uv_fs_write(nullptr, &writeReq, fd, &buf, 1, -1, nullptr);
    uv_fs_req_cleanup(&writeReq);
    if (written < 0) {
      uv_fs_t closeReq2;
      uv_fs_close(nullptr, &closeReq2, fd, nullptr);
      uv_fs_req_cleanup(&closeReq2);
      return throwUVException(env, written, "write", path.c_str());
    }
    offset += static_cast<size_t>(written);
  }

  // Close.
  uv_fs_t closeReq;
  uv_fs_close(nullptr, &closeReq, fd, nullptr);
  uv_fs_req_cleanup(&closeReq);

  return nullptr;
}

// binding.internalModuleStat(path)
// Returns: 0 = file, 1 = directory, negative = error (does not throw).
static napi_value fsInternalModuleStat(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  std::string path = getStringArg(env, argv[0]);

  uv_fs_t req;
  int result = uv_fs_stat(nullptr, &req, path.c_str(), nullptr);
  napi_value jsResult;
  if (result < 0) {
    napi_create_int32(env, result, &jsResult);
  } else if (S_ISDIR(req.statbuf.st_mode)) {
    napi_create_int32(env, 1, &jsResult);
  } else {
    napi_create_int32(env, 0, &jsResult);
  }
  uv_fs_req_cleanup(&req);
  return jsResult;
}

// ---------------------------------------------------------------------------
// legacyMainResolve -- used by the ESM resolver for package.json "main" field.
// ---------------------------------------------------------------------------

/// Check if a path is a file (not a directory). Returns true if file exists
/// and is not a directory.
static bool filePathIsFile(const std::string &filePath) {
  uv_fs_t req;
  int rc = uv_fs_stat(nullptr, &req, filePath.c_str(), nullptr);
  if (rc == 0) {
    rc = S_ISDIR(req.statbuf.st_mode);
  }
  uv_fs_req_cleanup(&req);
  // rc == 0 means: stat succeeded AND path is NOT a directory.
  return rc == 0;
}

// Extensions tried when packageConfig.main is defined (indices 0-6),
// then fallback to ./index (indices 7-9).
static constexpr const char *kLegacyMainExtensions[] = {
    "", // 0: main as-is
    ".js", // 1: main + .js
    ".json", // 2: main + .json
    ".node", // 3: main + .node
    "/index.js", // 4: main + /index.js
    "/index.json", // 5: main + /index.json
    "/index.node", // 6: main + /index.node
    ".js", // 7: ./index.js
    ".json", // 8: ./index.json
    ".node", // 9: ./index.node
};
static constexpr int kLegacyMainWithMainEnd = 7;
static constexpr int kLegacyMainFallbackEnd = 10;

// binding.legacyMainResolve(packagePath, main?, base?)
// Returns integer index (0-9) into the extensions table, or throws
// ERR_MODULE_NOT_FOUND if no file is found.
static napi_value fsLegacyMainResolve(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  std::string pkgPath = getStringArg(env, argv[0]);
  namespace fs = std::filesystem;

  std::string packageInitialFile;

  // Phase 1: If main is provided, try pkgPath/main + extensions[0..6].
  if (argc >= 2) {
    napi_valuetype vt;
    napi_typeof(env, argv[1], &vt);
    if (vt == napi_string) {
      std::string main = getStringArg(env, argv[1]);
      std::string basePath =
          fs::path(pkgPath).append(main).lexically_normal().string();
      packageInitialFile = basePath;

      for (int i = 0; i < kLegacyMainWithMainEnd; i++) {
        std::string filePath = basePath + kLegacyMainExtensions[i];
        if (filePathIsFile(filePath)) {
          napi_value result;
          napi_create_int32(env, i, &result);
          return result;
        }
      }
    }
  }

  // Phase 2: Fallback to pkgPath/index + extensions[7..9].
  std::string indexPath =
      fs::path(pkgPath).append("index").lexically_normal().string();

  for (int i = kLegacyMainWithMainEnd; i < kLegacyMainFallbackEnd; i++) {
    std::string filePath = indexPath + kLegacyMainExtensions[i];
    if (filePathIsFile(filePath)) {
      napi_value result;
      napi_create_int32(env, i, &result);
      return result;
    }
  }

  // No file found — throw ERR_MODULE_NOT_FOUND.
  if (packageInitialFile.empty())
    packageInitialFile = indexPath + ".js";

  std::string errMsg = "Cannot find package '" + packageInitialFile + "'";
  if (argc >= 3) {
    napi_valuetype vt;
    napi_typeof(env, argv[2], &vt);
    if (vt == napi_string) {
      std::string base = getStringArg(env, argv[2]);
      errMsg += " imported from " + base;
    }
  }
  napi_throw_error(env, "ERR_MODULE_NOT_FOUND", errMsg.c_str());
  return nullptr;
}

// binding.readBuffers(fd, buffers, position, req?)
static napi_value fsReadBuffers(napi_env env, napi_callback_info info) {
  size_t argc = 4;
  napi_value argv[4];
  FsBindingData *fsData = getFsData(env, info);
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  int fd = getInt32(env, argv[0]);

  // Get array of buffers.
  uint32_t numBuffers = 0;
  napi_get_array_length(env, argv[1], &numBuffers);

  std::vector<uv_buf_t> bufs(numBuffers);
  for (uint32_t i = 0; i < numBuffers; i++) {
    napi_value elem;
    napi_get_element(env, argv[1], i, &elem);
    napi_typedarray_type arrType;
    size_t length;
    void *data;
    napi_get_typedarray_info(
        env, elem, &arrType, &length, &data, nullptr, nullptr);
    bufs[i] = uv_buf_init(
        static_cast<char *>(data), static_cast<unsigned int>(length));
  }

  int64_t position = -1;
  if (argc > 2 && !isNullOrUndefined(env, argv[2])) {
    position = getInt64(env, argv[2], -1);
  }

  napi_value asyncReq = getAsyncReq(env, argc, argv, 3, fsData);
  if (asyncReq) {
    auto *wrap = new FSReqWrap();
    wrap->resultType = FSReqResultType::Integer;
    // Keep buffer array alive.
    napi_create_reference(env, argv[1], 1, &wrap->bufferRef);
    napi_value result = startAsyncFsOp(env, wrap, asyncReq, fsData);
    uv_fs_read(
        s_fsLoop,
        &wrap->req,
        fd,
        bufs.data(),
        numBuffers,
        position,
        fsAfterAsync);
    return result;
  }

  uv_fs_t req;
  int result =
      uv_fs_read(nullptr, &req, fd, bufs.data(), numBuffers, position, nullptr);
  uv_fs_req_cleanup(&req);
  if (result < 0) {
    return throwUVException(env, result, "read");
  }

  napi_value jsResult;
  napi_create_int32(env, result, &jsResult);
  return jsResult;
}

// binding.writeBuffers(fd, buffers, position, req?, ctx?)
static napi_value fsWriteBuffers(napi_env env, napi_callback_info info) {
  size_t argc = 5;
  napi_value argv[5];
  FsBindingData *fsData = getFsData(env, info);
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  int fd = getInt32(env, argv[0]);

  uint32_t numBuffers = 0;
  napi_get_array_length(env, argv[1], &numBuffers);

  std::vector<uv_buf_t> bufs(numBuffers);
  for (uint32_t i = 0; i < numBuffers; i++) {
    napi_value elem;
    napi_get_element(env, argv[1], i, &elem);
    napi_typedarray_type arrType;
    size_t length;
    void *data;
    napi_get_typedarray_info(
        env, elem, &arrType, &length, &data, nullptr, nullptr);
    bufs[i] = uv_buf_init(
        static_cast<char *>(data), static_cast<unsigned int>(length));
  }

  int64_t position = -1;
  if (argc > 2 && !isNullOrUndefined(env, argv[2])) {
    position = getInt64(env, argv[2], -1);
  }

  napi_value asyncReq = getAsyncReq(env, argc, argv, 3, fsData);
  if (asyncReq) {
    auto *wrap = new FSReqWrap();
    wrap->resultType = FSReqResultType::Integer;
    napi_create_reference(env, argv[1], 1, &wrap->bufferRef);
    napi_value result = startAsyncFsOp(env, wrap, asyncReq, fsData);
    uv_fs_write(
        s_fsLoop,
        &wrap->req,
        fd,
        bufs.data(),
        numBuffers,
        position,
        fsAfterAsync);
    return result;
  }

  uv_fs_t req;
  int result = uv_fs_write(
      nullptr, &req, fd, bufs.data(), numBuffers, position, nullptr);
  uv_fs_req_cleanup(&req);

  if (result < 0 && argc > 4 && !isNullOrUndefined(env, argv[4])) {
    setCtxError(env, argv[4], result, "write");
    napi_value jsResult;
    napi_create_int32(env, 0, &jsResult);
    return jsResult;
  }
  if (result < 0) {
    return throwUVException(env, result, "write");
  }

  napi_value jsResult;
  napi_create_int32(env, result, &jsResult);
  return jsResult;
}

// binding.rmSync(path, maxRetries, recursive, retryDelay)
// Recursive remove. For non-recursive, binding.rmdir is used.
static napi_value fsRmSync(napi_env env, napi_callback_info info) {
  size_t argc = 4;
  napi_value argv[4];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  std::string path = getStringArg(env, argv[0]);
  int maxRetries = getInt32(env, argv[1], 0);
  bool recursive = getBool(env, argv[2], false);
  // retryDelay = argv[3], not used in our simple implementation.

  (void)maxRetries;

  if (!recursive) {
    // Just try to remove as file.
    uv_fs_t req;
    int result = uv_fs_unlink(nullptr, &req, path.c_str(), nullptr);
    uv_fs_req_cleanup(&req);
    if (result < 0) {
      return throwUVException(env, result, "unlink", path.c_str());
    }
    return nullptr;
  }

  // Recursive remove: use a simple DFS.
  uv_fs_t statReq;
  int statResult = uv_fs_lstat(nullptr, &statReq, path.c_str(), nullptr);
  if (statResult < 0) {
    uv_fs_req_cleanup(&statReq);
    return throwUVException(env, statResult, "stat", path.c_str());
  }
  bool isDir = S_ISDIR(statReq.statbuf.st_mode);
  uv_fs_req_cleanup(&statReq);

  if (!isDir) {
    uv_fs_t req;
    int result = uv_fs_unlink(nullptr, &req, path.c_str(), nullptr);
    uv_fs_req_cleanup(&req);
    if (result < 0) {
      return throwUVException(env, result, "unlink", path.c_str());
    }
    return nullptr;
  }

  // Directory: scan and remove children recursively.
  std::vector<std::string> stack;
  stack.push_back(path);
  std::vector<std::pair<std::string, bool>> entries; // path, isDir

  while (!stack.empty()) {
    std::string dir = stack.back();
    stack.pop_back();

    uv_fs_t scanReq;
    int scanResult = uv_fs_scandir(nullptr, &scanReq, dir.c_str(), 0, nullptr);
    if (scanResult < 0) {
      uv_fs_req_cleanup(&scanReq);
      return throwUVException(env, scanResult, "scandir", dir.c_str());
    }

    uv_dirent_t ent;
    while (uv_fs_scandir_next(&scanReq, &ent) == 0) {
      std::string childPath = dir + "/" + ent.name;
      if (ent.type == UV_DIRENT_DIR) {
        entries.push_back({childPath, true});
        stack.push_back(childPath);
      } else if (ent.type == UV_DIRENT_UNKNOWN) {
        uv_fs_t childStat;
        int childStatResult =
            uv_fs_lstat(nullptr, &childStat, childPath.c_str(), nullptr);
        if (childStatResult == 0 && S_ISDIR(childStat.statbuf.st_mode)) {
          entries.push_back({childPath, true});
          stack.push_back(childPath);
        } else {
          entries.push_back({childPath, false});
        }
        uv_fs_req_cleanup(&childStat);
      } else {
        entries.push_back({childPath, false});
      }
    }
    uv_fs_req_cleanup(&scanReq);
  }

  // Remove in reverse order (deepest first).
  for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
    uv_fs_t rmReq;
    int rmResult;
    if (it->second) {
      rmResult = uv_fs_rmdir(nullptr, &rmReq, it->first.c_str(), nullptr);
    } else {
      rmResult = uv_fs_unlink(nullptr, &rmReq, it->first.c_str(), nullptr);
    }
    uv_fs_req_cleanup(&rmReq);
    if (rmResult < 0) {
      const char *syscall = it->second ? "rmdir" : "unlink";
      return throwUVException(env, rmResult, syscall, it->first.c_str());
    }
  }

  // Remove the root directory itself.
  uv_fs_t rmReq;
  int rmResult = uv_fs_rmdir(nullptr, &rmReq, path.c_str(), nullptr);
  uv_fs_req_cleanup(&rmReq);
  if (rmResult < 0) {
    return throwUVException(env, rmResult, "rmdir", path.c_str());
  }

  return nullptr;
}

// ---------------------------------------------------------------------------
// cpSync helpers and bindings
// ---------------------------------------------------------------------------

/// Copy atime/mtime from src to dest. Returns false if an exception was thrown.
static bool
copyUtimes(napi_env env, const std::string &src, const std::string &dest) {
  uv_fs_t req;
  int result = uv_fs_stat(nullptr, &req, src.c_str(), nullptr);
  if (result < 0) {
    uv_fs_req_cleanup(&req);
    throwUVException(env, result, "stat", src.c_str());
    return false;
  }

  const double source_atime =
      req.statbuf.st_atim.tv_sec + req.statbuf.st_atim.tv_nsec / 1e9;
  const double source_mtime =
      req.statbuf.st_mtim.tv_sec + req.statbuf.st_mtim.tv_nsec / 1e9;
  uv_fs_req_cleanup(&req);

  int utime_result = uv_fs_utime(
      nullptr, &req, dest.c_str(), source_atime, source_mtime, nullptr);
  uv_fs_req_cleanup(&req);
  if (utime_result < 0) {
    throwUVException(env, utime_result, "utime", dest.c_str());
    return false;
  }
  return true;
}

/// Split a path into its components.
static std::vector<std::string> normalizePathToArray(
    const std::filesystem::path &path) {
  std::vector<std::string> parts;
  std::filesystem::path absPath = std::filesystem::absolute(path);
  for (const auto &part : absPath) {
    if (!part.empty())
      parts.push_back(part.string());
  }
  return parts;
}

/// Check if src is a prefix of dest (i.e. dest is inside src directory).
static bool isInsideDir(
    const std::filesystem::path &src,
    const std::filesystem::path &dest) {
  auto srcArr = normalizePathToArray(src);
  auto destArr = normalizePathToArray(dest);
  if (srcArr.size() > destArr.size())
    return false;
  return std::equal(srcArr.begin(), srcArr.end(), destArr.begin());
}

// binding.cpSyncCheckPaths(src, dest, dereference, recursive)
static napi_value fsCpSyncCheckPaths(napi_env env, napi_callback_info info) {
  size_t argc = 4;
  napi_value argv[4];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  std::string src = getStringArg(env, argv[0]);
  std::string dest = getStringArg(env, argv[1]);
  bool dereference = getBool(env, argv[2], false);
  bool recursive = getBool(env, argv[3], false);

  namespace fs = std::filesystem;
  std::error_code ec;

  // Note: Node uses inverted logic -- dereference=true means follow symlinks
  // (stat), dereference=false means don't follow (lstat/symlink_status).
  // But the JS caller passes opts.dereference which is false by default.
  // Node's C++ code: dereference ? symlink_status : status
  // This is CORRECT because Node's "dereference" param is actually the
  // negation: when dereference=false, we use status() (follows symlinks).
  auto src_status =
      dereference ? fs::symlink_status(src, ec) : fs::status(src, ec);

  if (ec) {
    int errorno = ec.value() > 0 ? -ec.value() : ec.value();
    return throwUVException(
        env, errorno, dereference ? "lstat" : "stat", src.c_str());
  }

  auto dest_status =
      dereference ? fs::status(dest, ec) : fs::symlink_status(dest, ec);

  bool dest_exists = !ec && dest_status.type() != fs::file_type::not_found;
  bool src_is_dir = (src_status.type() == fs::file_type::directory) ||
      (dereference && src_status.type() == fs::file_type::symlink);

  fs::path srcPath(src);
  fs::path destPath(dest);

  if (!ec) {
    // Check if src and dest are identical.
    if (fs::equivalent(srcPath, destPath, ec) && !ec) {
      std::string msg = "src and dest cannot be the same " + dest;
      return throwCpError(env, "ERR_FS_CP_EINVAL", msg, dest.c_str());
    }

    bool dest_is_dir = dest_status.type() == fs::file_type::directory;
    if (src_is_dir && !dest_is_dir) {
      std::string msg =
          "Cannot overwrite non-directory " + dest + " with directory " + src;
      return throwCpError(env, "ERR_FS_CP_DIR_TO_NON_DIR", msg, dest.c_str());
    }

    if (!src_is_dir && dest_is_dir) {
      std::string msg =
          "Cannot overwrite directory " + dest + " with non-directory " + src;
      return throwCpError(env, "ERR_FS_CP_NON_DIR_TO_DIR", msg, dest.c_str());
    }
  }

  // Check if dest is a subdirectory of src (string prefix check).
  std::string srcStr = src;
  if (!srcStr.empty() && srcStr.back() != fs::path::preferred_separator) {
    srcStr += fs::path::preferred_separator;
  }
  if (src_is_dir && dest.substr(0, srcStr.size()) == srcStr) {
    std::string msg =
        "Cannot copy " + src + " to a subdirectory of self " + dest;
    return throwCpError(env, "ERR_FS_CP_EINVAL", msg, dest.c_str());
  }

  // Walk parent paths to detect subdirectory-of-self.
  auto dest_parent = destPath.parent_path();
  while (srcPath.parent_path() != dest_parent &&
         dest_parent.has_parent_path() &&
         dest_parent.parent_path() != dest_parent) {
    ec.clear();
    if (fs::equivalent(srcPath, destPath.parent_path(), ec) && !ec) {
      std::string msg =
          "Cannot copy " + src + " to a subdirectory of self " + dest;
      return throwCpError(env, "ERR_FS_CP_EINVAL", msg, dest.c_str());
    }
    if (ec)
      break;
    dest_parent = dest_parent.parent_path();
  }

  if (src_is_dir && !recursive) {
    std::string msg =
        "Recursive option not enabled, cannot copy a directory: " + src;
    return throwCpError(env, "ERR_FS_EISDIR", msg, src.c_str());
  }

  switch (src_status.type()) {
    case fs::file_type::socket: {
      std::string msg = "Cannot copy a socket file: " + dest;
      return throwCpError(env, "ERR_FS_CP_SOCKET", msg, dest.c_str());
    }
    case fs::file_type::fifo: {
      std::string msg = "Cannot copy a FIFO pipe: " + dest;
      return throwCpError(env, "ERR_FS_CP_FIFO_PIPE", msg, dest.c_str());
    }
    case fs::file_type::unknown: {
      std::string msg = "Cannot copy an unknown file type: " + dest;
      return throwCpError(env, "ERR_FS_CP_UNKNOWN", msg, dest.c_str());
    }
    default:
      break;
  }

  // Create parent directories of dest if needed.
  if (!dest_exists || !fs::exists(destPath.parent_path())) {
    fs::create_directories(destPath.parent_path(), ec);
  }

  return nullptr;
}

// binding.cpSyncOverrideFile(src, dest, mode, preserveTimestamps)
static napi_value fsCpSyncOverrideFile(napi_env env, napi_callback_info info) {
  size_t argc = 4;
  napi_value argv[4];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  std::string src = getStringArg(env, argv[0]);
  std::string dest = getStringArg(env, argv[1]);
  int32_t mode = getInt32(env, argv[2], 0);
  bool preserveTimestamps = getBool(env, argv[3], false);

  std::error_code ec;

  // Remove destination first.
  if (!std::filesystem::remove(dest, ec) && ec) {
    return throwUVException(env, -(ec.value()), "unlink", dest.c_str());
  }

  if (mode == 0) {
    // No special mode: use faster std::filesystem API.
    if (!std::filesystem::copy_file(src, dest, ec)) {
      int errorno = ec.value() > 0 ? -ec.value() : ec.value();
      return throwUVException(env, errorno, "cp", dest.c_str());
    }
  } else {
    // Use uv_fs_copyfile for EXCL/FICLONE flags.
    uv_fs_t req;
    int result =
        uv_fs_copyfile(nullptr, &req, src.c_str(), dest.c_str(), mode, nullptr);
    uv_fs_req_cleanup(&req);
    if (result < 0) {
      return throwUVException(env, result, "cp", src.c_str(), dest.c_str());
    }
  }

  if (preserveTimestamps) {
    copyUtimes(env, src, dest);
  }

  return nullptr;
}

// binding.cpSyncCopyDir(src, dest, force, dereference, errorOnExist,
//                       verbatimSymlinks, preserveTimestamps)
static napi_value fsCpSyncCopyDir(napi_env env, napi_callback_info info) {
  size_t argc = 7;
  napi_value argv[7];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  std::string src = getStringArg(env, argv[0]);
  std::string dest = getStringArg(env, argv[1]);
  bool force = getBool(env, argv[2], false);
  bool dereference = getBool(env, argv[3], false);
  bool errorOnExist = getBool(env, argv[4], false);
  bool verbatimSymlinks = getBool(env, argv[5], false);
  bool preserveTimestamps = getBool(env, argv[6], false);

  namespace fs = std::filesystem;
  std::error_code ec;

  fs::create_directories(dest, ec);
  if (ec) {
    int errorno = ec.value() > 0 ? -ec.value() : ec.value();
    return throwUVException(env, errorno, "mkdir", dest.c_str());
  }

  auto fileCopyOpts = fs::copy_options::recursive;
  if (force) {
    fileCopyOpts |= fs::copy_options::overwrite_existing;
  } else if (!errorOnExist) {
    fileCopyOpts |= fs::copy_options::skip_existing;
  }

  // Recursive directory copy lambda.
  std::function<bool(fs::path, fs::path)> copyDirContents;
  copyDirContents = [&](fs::path srcDir, fs::path destDir) -> bool {
    std::error_code err;
    for (auto &dirEntry : fs::directory_iterator(srcDir, err)) {
      if (err) {
        int errorno = err.value() > 0 ? -err.value() : err.value();
        throwUVException(env, errorno, "scandir", srcDir.string().c_str());
        return false;
      }

      auto destFilePath = destDir / dirEntry.path().filename();

      if (dirEntry.is_symlink()) {
        if (verbatimSymlinks) {
          fs::copy_symlink(dirEntry.path(), destFilePath, err);
          if (err) {
            int errorno = err.value() > 0 ? -err.value() : err.value();
            throwUVException(env, errorno, "cp", destDir.string().c_str());
            return false;
          }
        } else {
          auto symlinkTarget = fs::read_symlink(dirEntry.path(), err);
          if (err) {
            int errorno = err.value() > 0 ? -err.value() : err.value();
            throwUVException(
                env, errorno, "readlink", dirEntry.path().string().c_str());
            return false;
          }

          if (fs::exists(destFilePath)) {
            if (fs::is_symlink(destFilePath)) {
              auto currentDestTarget = fs::read_symlink(destFilePath, err);
              if (err) {
                int errorno = err.value() > 0 ? -err.value() : err.value();
                throwUVException(
                    env, errorno, "readlink", destFilePath.string().c_str());
                return false;
              }

              if (!dereference && fs::is_directory(symlinkTarget) &&
                  isInsideDir(symlinkTarget, currentDestTarget)) {
                std::string msg = "Cannot copy " + symlinkTarget.string() +
                    " to a subdirectory of self " + currentDestTarget.string();
                throwCpError(
                    env,
                    "ERR_FS_CP_EINVAL",
                    msg,
                    destFilePath.string().c_str());
                return false;
              }

              if (fs::is_directory(destFilePath) &&
                  isInsideDir(currentDestTarget, symlinkTarget)) {
                std::string msg = "cannot overwrite " +
                    currentDestTarget.string() + " with " +
                    symlinkTarget.string();
                throwCpError(
                    env,
                    "ERR_FS_CP_SYMLINK_TO_SUBDIRECTORY",
                    msg,
                    destFilePath.string().c_str());
                return false;
              }

              fs::remove(destFilePath, err);
              if (err) {
                int errorno = err.value() > 0 ? -err.value() : err.value();
                throwUVException(
                    env, errorno, "unlink", destFilePath.string().c_str());
                return false;
              }
            } else if (fs::is_regular_file(destFilePath)) {
              if (!dereference || (!force && errorOnExist)) {
                int errorno = -EEXIST;
                throwUVException(
                    env, errorno, "cp", destFilePath.string().c_str());
                return false;
              }
            }
          }

          auto symlinkTargetAbsolute =
              fs::weakly_canonical(fs::absolute(srcDir / symlinkTarget));
          if (dirEntry.is_directory()) {
            fs::create_directory_symlink(
                symlinkTargetAbsolute, destFilePath, err);
          } else {
            fs::create_symlink(symlinkTargetAbsolute, destFilePath, err);
          }
          if (err) {
            int errorno = err.value() > 0 ? -err.value() : err.value();
            throwUVException(
                env, errorno, "symlink", destFilePath.string().c_str());
            return false;
          }
        }
      } else if (dirEntry.is_directory()) {
        auto entryDirPath = srcDir / dirEntry.path().filename();
        fs::create_directory(destFilePath, err);
        // Ignore error if directory already exists.
        if (err && err.value() != EEXIST) {
          int errorno = err.value() > 0 ? -err.value() : err.value();
          throwUVException(
              env, errorno, "mkdir", destFilePath.string().c_str());
          return false;
        }
        if (!copyDirContents(entryDirPath, destFilePath))
          return false;
      } else if (dirEntry.is_regular_file()) {
        fs::copy_file(dirEntry.path(), destFilePath, fileCopyOpts, err);
        if (err) {
          if (err.value() == EEXIST) {
            std::string msg =
                "[ERR_FS_CP_EEXIST]: Target already exists: "
                "cp returned EEXIST (" +
                destFilePath.string() + " already exists)";
            throwCpError(
                env, "ERR_FS_CP_EEXIST", msg, destFilePath.string().c_str());
            return false;
          }
          int errorno = err.value() > 0 ? -err.value() : err.value();
          throwUVException(env, errorno, "cp", destDir.string().c_str());
          return false;
        }

        if (preserveTimestamps) {
          auto srcFile = dirEntry.path().string();
          auto destFile = destFilePath.string();
          if (!copyUtimes(env, srcFile, destFile))
            return false;
        }
      }
    }
    return true;
  };

  copyDirContents(fs::path(src), fs::path(dest));
  return nullptr;
}

// ---------------------------------------------------------------------------
// FSReqCallback constructor
// ---------------------------------------------------------------------------

static napi_value fsReqCallbackCtor(napi_env env, napi_callback_info info) {
  napi_value thisVal;
  napi_get_cb_info(env, info, nullptr, nullptr, &thisVal, nullptr);
  return thisVal;
}

// ---------------------------------------------------------------------------
// StatWatcher — wraps uv_fs_poll_t for fs.watchFile()
// ---------------------------------------------------------------------------

struct StatWatcherWrap {
  uv_fs_poll_t handle;
  napi_env env;
  napi_ref selfRef; // prevent GC while active
  FsBindingData *fsData; // access to shared stat buffers
  bool useBigint;
  bool initialized;
  bool closing;

  StatWatcherWrap()
      : env(nullptr),
        selfRef(nullptr),
        fsData(nullptr),
        useBigint(false),
        initialized(false),
        closing(false) {
    memset(&handle, 0, sizeof(handle));
  }
};

static void onStatPoll(
    uv_fs_poll_t *handle,
    int status,
    const uv_stat_t *prev,
    const uv_stat_t *curr) {
  auto *wrap = static_cast<StatWatcherWrap *>(handle->data);
  if (!wrap || !wrap->env)
    return;

  napi_env env = wrap->env;
  napi_handle_scope scope;
  napi_open_handle_scope(env, &scope);

  napi_value thisObj;
  if (wrap->selfRef)
    napi_get_reference_value(env, wrap->selfRef, &thisObj);
  else
    napi_get_undefined(env, &thisObj);

  // Get the onchange callback.
  napi_value onchange;
  napi_get_named_property(env, thisObj, "onchange", &onchange);
  napi_valuetype onchangeType;
  napi_typeof(env, onchange, &onchangeType);
  if (onchangeType != napi_function) {
    napi_close_handle_scope(env, scope);
    return;
  }

  // Fill current stats at offset 0, previous stats at offset
  // kFsStatsFieldsNumber.
  napi_value statsArr =
      fillAndReturnStats(env, wrap->fsData, curr, wrap->useBigint, 0);
  fillAndReturnStats(
      env, wrap->fsData, prev, wrap->useBigint, kFsStatsFieldsNumber);

  // Call onchange(status, statsArr).
  napi_value args[2];
  napi_create_int32(env, status, &args[0]);
  args[1] = statsArr;

  napi_value retval;
  napi_call_function(env, thisObj, onchange, 2, args, &retval);

  bool hasPending = false;
  napi_is_exception_pending(env, &hasPending);
  if (hasPending) {
    napi_value exc;
    napi_get_and_clear_last_exception(env, &exc);
  }

  napi_close_handle_scope(env, scope);
}

/// StatWatcher constructor: new StatWatcher(useBigint)
/// The fsData pointer is passed via the constructor's callback data.
static napi_value statWatcherNew(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1], thisObj;
  void *data = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &thisObj, &data);

  auto *fsData = static_cast<FsBindingData *>(data);

  bool useBigint = false;
  if (argc > 0)
    napi_get_value_bool(env, argv[0], &useBigint);

  auto *wrap = new StatWatcherWrap();
  wrap->env = env;
  wrap->fsData = fsData;
  wrap->useBigint = useBigint;

  int err = uv_fs_poll_init(s_fsLoop, &wrap->handle);
  if (err != 0) {
    delete wrap;
    napi_throw_error(env, nullptr, "uv_fs_poll_init failed");
    return thisObj;
  }
  wrap->handle.data = wrap;
  wrap->initialized = true;

  napi_wrap(
      env,
      thisObj,
      wrap,
      [](napi_env, void *d, void *) {
        auto *w = static_cast<StatWatcherWrap *>(d);
        if (w->initialized) {
          // uv_close is async — the close callback will delete.
          uv_fs_poll_stop(&w->handle);
          uv_close(
              reinterpret_cast<uv_handle_t *>(&w->handle), [](uv_handle_t *h) {
                delete static_cast<StatWatcherWrap *>(h->data);
              });
          return;
        }
        delete w;
      },
      nullptr,
      nullptr);

  return thisObj;
}

/// StatWatcher.prototype.start(path, interval)
static napi_value statWatcherStart(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2], thisObj;
  napi_get_cb_info(env, info, &argc, argv, &thisObj, nullptr);

  StatWatcherWrap *wrap = nullptr;
  napi_unwrap(env, thisObj, reinterpret_cast<void **>(&wrap));
  if (!wrap)
    return nullptr;

  size_t pathLen = 0;
  napi_get_value_string_utf8(env, argv[0], nullptr, 0, &pathLen);
  std::string path(pathLen, '\0');
  napi_get_value_string_utf8(env, argv[0], &path[0], pathLen + 1, &pathLen);

  uint32_t interval = 5007;
  napi_get_value_uint32(env, argv[1], &interval);

  // Create a ref to prevent GC while polling.
  napi_create_reference(env, thisObj, 1, &wrap->selfRef);

  int err = uv_fs_poll_start(&wrap->handle, onStatPoll, path.c_str(), interval);
  if (err != 0) {
    napi_delete_reference(env, wrap->selfRef);
    wrap->selfRef = nullptr;
    napi_value result;
    napi_create_int32(env, err, &result);
    return result;
  }

  return nullptr; // success: undefined (no error code)
}

static void onStatWatcherClose(uv_handle_t *handle) {
  auto *wrap = static_cast<StatWatcherWrap *>(handle->data);
  if (!wrap)
    return;
  if (wrap->selfRef) {
    napi_delete_reference(wrap->env, wrap->selfRef);
    wrap->selfRef = nullptr;
  }
  delete wrap;
}

/// StatWatcher.prototype.close()
static napi_value statWatcherClose(napi_env env, napi_callback_info info) {
  napi_value thisObj;
  napi_get_cb_info(env, info, nullptr, nullptr, &thisObj, nullptr);

  StatWatcherWrap *wrap = nullptr;
  napi_unwrap(env, thisObj, reinterpret_cast<void **>(&wrap));
  if (!wrap || wrap->closing)
    return nullptr;

  wrap->closing = true;

  if (wrap->initialized &&
      !uv_is_closing(reinterpret_cast<uv_handle_t *>(&wrap->handle))) {
    uv_fs_poll_stop(&wrap->handle);
    uv_close(
        reinterpret_cast<uv_handle_t *>(&wrap->handle), onStatWatcherClose);
    // The close callback now owns 'wrap'. Remove the GC wrap so the
    // finalizer won't run and try to double-delete.
    napi_remove_wrap(env, thisObj, nullptr);
  } else if (wrap->selfRef) {
    napi_delete_reference(env, wrap->selfRef);
    wrap->selfRef = nullptr;
  }

  return nullptr;
}

/// StatWatcher.prototype.ref()
static napi_value statWatcherRef(napi_env env, napi_callback_info info) {
  napi_value thisObj;
  napi_get_cb_info(env, info, nullptr, nullptr, &thisObj, nullptr);

  StatWatcherWrap *wrap = nullptr;
  napi_unwrap(env, thisObj, reinterpret_cast<void **>(&wrap));
  if (wrap && wrap->initialized && !wrap->closing)
    uv_ref(reinterpret_cast<uv_handle_t *>(&wrap->handle));
  return thisObj;
}

/// StatWatcher.prototype.unref()
static napi_value statWatcherUnref(napi_env env, napi_callback_info info) {
  napi_value thisObj;
  napi_get_cb_info(env, info, nullptr, nullptr, &thisObj, nullptr);

  StatWatcherWrap *wrap = nullptr;
  napi_unwrap(env, thisObj, reinterpret_cast<void **>(&wrap));
  if (wrap && wrap->initialized && !wrap->closing)
    uv_unref(reinterpret_cast<uv_handle_t *>(&wrap->handle));
  return thisObj;
}

/// StatWatcher.prototype.getAsyncId() — stub
static napi_value statWatcherGetAsyncId(napi_env env, napi_callback_info) {
  napi_value result;
  napi_create_double(env, 0.0, &result);
  return result;
}

// ---------------------------------------------------------------------------
// Binding init
// ---------------------------------------------------------------------------

napi_value initFsBinding(napi_env env, napi_value exports) {
  // Allocate persistent data.
  auto *fsData = new FsBindingData();

  // Create shared stat typed arrays.
  {
    napi_value arrBuf;
    void *data;
    napi_create_arraybuffer(
        env, kFsStatsBufferLength * sizeof(double), &data, &arrBuf);
    napi_value typedArr;
    napi_create_typedarray(
        env, napi_float64_array, kFsStatsBufferLength, arrBuf, 0, &typedArr);
    napi_create_reference(env, typedArr, 1, &fsData->statValuesRef);
    napi_set_named_property(env, exports, "statValues", typedArr);
  }
  {
    napi_value arrBuf;
    void *data;
    napi_create_arraybuffer(
        env, kFsStatsBufferLength * sizeof(int64_t), &data, &arrBuf);
    napi_value typedArr;
    napi_create_typedarray(
        env, napi_bigint64_array, kFsStatsBufferLength, arrBuf, 0, &typedArr);
    napi_create_reference(env, typedArr, 1, &fsData->bigintStatValuesRef);
    napi_set_named_property(env, exports, "bigintStatValues", typedArr);
  }
  {
    napi_value arrBuf;
    void *data;
    napi_create_arraybuffer(
        env, kStatFsBufferLength * sizeof(double), &data, &arrBuf);
    napi_value typedArr;
    napi_create_typedarray(
        env, napi_float64_array, kStatFsBufferLength, arrBuf, 0, &typedArr);
    napi_create_reference(env, typedArr, 1, &fsData->statFsValuesRef);
    napi_set_named_property(env, exports, "statFsValues", typedArr);
  }
  {
    napi_value arrBuf;
    void *data;
    napi_create_arraybuffer(
        env, kStatFsBufferLength * sizeof(int64_t), &data, &arrBuf);
    napi_value typedArr;
    napi_create_typedarray(
        env, napi_bigint64_array, kStatFsBufferLength, arrBuf, 0, &typedArr);
    napi_create_reference(env, typedArr, 1, &fsData->bigintStatFsValuesRef);
    napi_set_named_property(env, exports, "bigintStatFsValues", typedArr);
  }

  // Create kUsePromises sentinel.
  {
    napi_value sentinel;
    napi_create_object(env, &sentinel);
    napi_create_reference(env, sentinel, 1, &fsData->kUsePromisesRef);
    napi_set_named_property(env, exports, "kUsePromises", sentinel);
  }

  // FSReqCallback constructor.
  {
    napi_value ctor;
    napi_create_function(
        env,
        "FSReqCallback",
        NAPI_AUTO_LENGTH,
        fsReqCallbackCtor,
        nullptr,
        &ctor);
    napi_set_named_property(env, exports, "FSReqCallback", ctor);
  }

  // StatWatcher constructor — gets fsData as callback data for stat buffer
  // access.
  {
    napi_value ctorFn;
    napi_create_function(
        env, "StatWatcher", NAPI_AUTO_LENGTH, statWatcherNew, fsData, &ctorFn);

    napi_value prototype;
    napi_get_named_property(env, ctorFn, "prototype", &prototype);

    napi_value fn;

    napi_create_function(
        env, "start", NAPI_AUTO_LENGTH, statWatcherStart, nullptr, &fn);
    napi_set_named_property(env, prototype, "start", fn);

    napi_create_function(
        env, "close", NAPI_AUTO_LENGTH, statWatcherClose, nullptr, &fn);
    napi_set_named_property(env, prototype, "close", fn);

    napi_create_function(
        env, "ref", NAPI_AUTO_LENGTH, statWatcherRef, nullptr, &fn);
    napi_set_named_property(env, prototype, "ref", fn);

    napi_create_function(
        env, "unref", NAPI_AUTO_LENGTH, statWatcherUnref, nullptr, &fn);
    napi_set_named_property(env, prototype, "unref", fn);

    napi_create_function(
        env,
        "getAsyncId",
        NAPI_AUTO_LENGTH,
        statWatcherGetAsyncId,
        nullptr,
        &fn);
    napi_set_named_property(env, prototype, "getAsyncId", fn);

    napi_set_named_property(env, exports, "StatWatcher", ctorFn);
  }

  // Export kFsStatsFieldsNumber constant.
  {
    napi_value val;
    napi_create_int32(env, kFsStatsFieldsNumber, &val);
    napi_set_named_property(env, exports, "kFsStatsFieldsNumber", val);
  }

  // Register all fs functions. All get fsData as callback data.
#define SET_FN(name, fn)                                                   \
  do {                                                                     \
    napi_value fnVal;                                                      \
    napi_create_function(env, name, NAPI_AUTO_LENGTH, fn, fsData, &fnVal); \
    napi_set_named_property(env, exports, name, fnVal);                    \
  } while (0)

  SET_FN("access", fsAccess);
  SET_FN("open", fsOpen);
  SET_FN("openFileHandle", fsOpenFileHandle);
  SET_FN("close", fsClose);
  SET_FN("read", fsRead);
  SET_FN("writeBuffer", fsWriteBuffer);
  SET_FN("writeString", fsWriteString);
  SET_FN("stat", fsStat);
  SET_FN("lstat", fsLstat);
  SET_FN("fstat", fsFstat);
  SET_FN("statfs", fsStatFs);
  SET_FN("rename", fsRename);
  SET_FN("unlink", fsUnlink);
  SET_FN("mkdir", fsMkdir);
  SET_FN("rmdir", fsRmdir);
  SET_FN("readdir", fsReaddir);
  SET_FN("chmod", fsChmod);
  SET_FN("fchmod", fsFchmod);
  SET_FN("chown", fsChown);
  SET_FN("fchown", fsFchown);
  SET_FN("lchown", fsLchown);
  SET_FN("link", fsLink);
  SET_FN("symlink", fsSymlink);
  SET_FN("readlink", fsReadlink);
  SET_FN("realpath", fsRealpath);
  SET_FN("ftruncate", fsFtruncate);
  SET_FN("utimes", fsUtimes);
  SET_FN("futimes", fsFutimes);
  SET_FN("lutimes", fsLutimes);
  SET_FN("mkdtemp", fsMkdtemp);
  SET_FN("copyFile", fsCopyFile);
  SET_FN("existsSync", fsExistsSync);
  SET_FN("fsync", fsFsync);
  SET_FN("fdatasync", fsFdatasync);
  SET_FN("readFileUtf8", fsReadFileUtf8);
  SET_FN("writeFileUtf8", fsWriteFileUtf8);
  SET_FN("internalModuleStat", fsInternalModuleStat);
  SET_FN("legacyMainResolve", fsLegacyMainResolve);
  SET_FN("readBuffers", fsReadBuffers);
  SET_FN("writeBuffers", fsWriteBuffers);
  SET_FN("rmSync", fsRmSync);
  SET_FN("cpSyncCheckPaths", fsCpSyncCheckPaths);
  SET_FN("cpSyncOverrideFile", fsCpSyncOverrideFile);
  SET_FN("cpSyncCopyDir", fsCpSyncCopyDir);

#undef SET_FN

  // Add finalizer to clean up fsData.
  napi_add_finalizer(
      env,
      exports,
      fsData,
      [](napi_env e, void *data, void *) {
        auto *d = static_cast<FsBindingData *>(data);
        napi_delete_reference(e, d->statValuesRef);
        napi_delete_reference(e, d->bigintStatValuesRef);
        napi_delete_reference(e, d->statFsValuesRef);
        napi_delete_reference(e, d->bigintStatFsValuesRef);
        if (d->kUsePromisesRef)
          napi_delete_reference(e, d->kUsePromisesRef);
        delete d;
      },
      nullptr,
      nullptr);

  return exports;
}

} // namespace node_compat
} // namespace hermes
