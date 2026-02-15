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
#include <string>
#include <vector>

namespace hermes {
namespace node_compat {

// ---------------------------------------------------------------------------
// Stat field layout — must match Node's FsStatsOffset enum
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

/// Throw a UVException. Sets errno, code, syscall, path, message on the Error.
/// Returns nullptr (for use as return value from napi_callback).
static napi_value throwUVException(
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

  napi_create_string_utf8(
      env, uv_strerror(errorno), NAPI_AUTO_LENGTH, &val);
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
  napi_ref statValuesRef;       // Float64Array(36)
  napi_ref bigintStatValuesRef; // BigInt64Array(36)
  napi_ref statFsValuesRef;     // Float64Array(14)
  napi_ref bigintStatFsValuesRef; // BigInt64Array(14)
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
    napi_get_typedarray_info(env, result, &type, &length, &data, nullptr, nullptr);
    auto *buf = static_cast<int64_t *>(data);
    fillBigIntStatValues(buf + offset, s);
  } else {
    napi_get_reference_value(env, fsData->statValuesRef, &result);
    napi_typedarray_type type;
    size_t length;
    void *data;
    napi_get_typedarray_info(env, result, &type, &length, &data, nullptr, nullptr);
    auto *buf = static_cast<double *>(data);
    fillStatValues(buf + offset, s);
  }
  return result;
}

// ---------------------------------------------------------------------------
// Sync FS operations
// ---------------------------------------------------------------------------

// binding.access(path, mode)
static napi_value fsAccess(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  std::string path = getStringArg(env, argv[0]);
  int mode = getInt32(env, argv[1], 0);

  uv_fs_t req;
  int result = uv_fs_access(nullptr, &req, path.c_str(), mode, nullptr);
  uv_fs_req_cleanup(&req);
  if (result < 0) {
    return throwUVException(env, result, "access", path.c_str());
  }
  return nullptr;
}

// binding.open(path, flags, mode)
static napi_value fsOpen(napi_env env, napi_callback_info info) {
  size_t argc = 4;
  napi_value argv[4];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  // If argc > 3 and argv[3] is an FSReqCallback, this is async. For now sync only.
  std::string path = getStringArg(env, argv[0]);
  int flags = getInt32(env, argv[1], 0);
  int mode = getInt32(env, argv[2], 0666);

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

// binding.close(fd)
static napi_value fsClose(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  int fd = getInt32(env, argv[0]);

  uv_fs_t req;
  int result = uv_fs_close(nullptr, &req, fd, nullptr);
  uv_fs_req_cleanup(&req);
  if (result < 0) {
    return throwUVException(env, result, "close");
  }
  return nullptr;
}

// binding.read(fd, buffer, offset, length, position)
static napi_value fsRead(napi_env env, napi_callback_info info) {
  size_t argc = 6;
  napi_value argv[6];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  int fd = getInt32(env, argv[0]);

  // Get the buffer data.
  napi_typedarray_type arrType;
  size_t arrLength;
  void *data;
  napi_get_typedarray_info(env, argv[1], &arrType, &arrLength, &data, nullptr, nullptr);
  auto *bufData = static_cast<uint8_t *>(data);

  int32_t offset = getInt32(env, argv[2], 0);
  int32_t length = getInt32(env, argv[3], 0);
  int64_t position = getInt64(env, argv[4], -1);

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

// binding.writeBuffer(fd, buffer, offset, length, position, undefined, ctx)
static napi_value fsWriteBuffer(napi_env env, napi_callback_info info) {
  size_t argc = 7;
  napi_value argv[7];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  int fd = getInt32(env, argv[0]);

  // Get the buffer data.
  napi_typedarray_type arrType;
  size_t arrLength;
  void *data;
  napi_get_typedarray_info(env, argv[1], &arrType, &arrLength, &data, nullptr, nullptr);
  auto *bufData = static_cast<uint8_t *>(data);

  int32_t offset = getInt32(env, argv[2], 0);
  int32_t length = getInt32(env, argv[3], 0);

  // position can be null (current position).
  int64_t position = -1;
  if (argc > 4 && !isNullOrUndefined(env, argv[4])) {
    position = getInt64(env, argv[4], -1);
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

// binding.writeString(fd, string, position, encoding, undefined, ctx)
static napi_value fsWriteString(napi_env env, napi_callback_info info) {
  size_t argc = 6;
  napi_value argv[6];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  int fd = getInt32(env, argv[0]);
  std::string str = getStringArg(env, argv[1]);

  // position can be null.
  int64_t position = -1;
  if (argc > 2 && !isNullOrUndefined(env, argv[2])) {
    position = getInt64(env, argv[2], -1);
  }

  // encoding is argv[3] but we always write as UTF-8.
  // TODO: handle other encodings if needed.

  uv_buf_t buf = uv_buf_init(
      const_cast<char *>(str.data()),
      static_cast<unsigned int>(str.size()));

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

// binding.stat(path, useBigint, req, throwIfNoEntry)
// Sync when req is undefined.
static napi_value fsStat(napi_env env, napi_callback_info info) {
  size_t argc = 4;
  napi_value argv[4];
  FsBindingData *fsData = getFsData(env, info);
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  std::string path = getStringArg(env, argv[0]);
  bool useBigint = argc > 1 && getBool(env, argv[1], false);

  // throwIfNoEntry: argv[3] (default true for statSync)
  bool throwIfNoEntry = true;
  if (argc > 3 && !isNullOrUndefined(env, argv[3])) {
    throwIfNoEntry = getBool(env, argv[3], true);
  }

  uv_fs_t req;
  int result = uv_fs_stat(nullptr, &req, path.c_str(), nullptr);
  if (result < 0) {
    uv_fs_req_cleanup(&req);
    if (!throwIfNoEntry && (result == UV_ENOENT || result == UV_ENOTDIR ||
                            result == UV_ELOOP)) {
      return nullptr; // return undefined
    }
    return throwUVException(env, result, "stat", path.c_str());
  }

  napi_value jsResult = fillAndReturnStats(env, fsData, &req.statbuf, useBigint);
  uv_fs_req_cleanup(&req);
  return jsResult;
}

// binding.lstat(path, useBigint, req, throwIfNoEntry)
static napi_value fsLstat(napi_env env, napi_callback_info info) {
  size_t argc = 4;
  napi_value argv[4];
  FsBindingData *fsData = getFsData(env, info);
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  std::string path = getStringArg(env, argv[0]);
  bool useBigint = argc > 1 && getBool(env, argv[1], false);

  bool throwIfNoEntry = true;
  if (argc > 3 && !isNullOrUndefined(env, argv[3])) {
    throwIfNoEntry = getBool(env, argv[3], true);
  }

  uv_fs_t req;
  int result = uv_fs_lstat(nullptr, &req, path.c_str(), nullptr);
  if (result < 0) {
    uv_fs_req_cleanup(&req);
    if (!throwIfNoEntry && (result == UV_ENOENT || result == UV_ENOTDIR ||
                            result == UV_ELOOP)) {
      return nullptr;
    }
    return throwUVException(env, result, "lstat", path.c_str());
  }

  napi_value jsResult = fillAndReturnStats(env, fsData, &req.statbuf, useBigint);
  uv_fs_req_cleanup(&req);
  return jsResult;
}

// binding.fstat(fd, useBigint, req, shouldNotThrow)
static napi_value fsFstat(napi_env env, napi_callback_info info) {
  size_t argc = 4;
  napi_value argv[4];
  FsBindingData *fsData = getFsData(env, info);
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  int fd = getInt32(env, argv[0]);
  bool useBigint = argc > 1 && getBool(env, argv[1], false);

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

  napi_value jsResult = fillAndReturnStats(env, fsData, &req.statbuf, useBigint);
  uv_fs_req_cleanup(&req);
  return jsResult;
}

// binding.statfs(path, useBigint)
static napi_value fsStatFs(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  FsBindingData *fsData = getFsData(env, info);
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  std::string path = getStringArg(env, argv[0]);
  bool useBigint = argc > 1 && getBool(env, argv[1], false);

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
    napi_get_typedarray_info(env, jsResult, &type, &length, &data, nullptr, nullptr);
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
    napi_get_typedarray_info(env, jsResult, &type, &length, &data, nullptr, nullptr);
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

// binding.rename(oldPath, newPath)
static napi_value fsRename(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  std::string oldPath = getStringArg(env, argv[0]);
  std::string newPath = getStringArg(env, argv[1]);

  uv_fs_t req;
  int result = uv_fs_rename(nullptr, &req, oldPath.c_str(), newPath.c_str(), nullptr);
  uv_fs_req_cleanup(&req);
  if (result < 0) {
    return throwUVException(env, result, "rename", oldPath.c_str(), newPath.c_str());
  }
  return nullptr;
}

// binding.unlink(path)
static napi_value fsUnlink(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  std::string path = getStringArg(env, argv[0]);

  uv_fs_t req;
  int result = uv_fs_unlink(nullptr, &req, path.c_str(), nullptr);
  uv_fs_req_cleanup(&req);
  if (result < 0) {
    return throwUVException(env, result, "unlink", path.c_str());
  }
  return nullptr;
}

// binding.mkdir(path, mode, recursive)
static napi_value fsMkdir(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  std::string path = getStringArg(env, argv[0]);
  int mode = getInt32(env, argv[1], 0777);
  bool recursive = argc > 2 && getBool(env, argv[2], false);

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
  // Walk the path and create directories as needed.
  std::string firstCreated;
  std::string current;
  size_t pos = 0;

  // Handle absolute path.
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
      return throwUVException(env, result, "mkdir", current.c_str());
    }
  }

  if (!firstCreated.empty()) {
    napi_value jsResult;
    napi_create_string_utf8(env, firstCreated.c_str(), firstCreated.size(), &jsResult);
    return jsResult;
  }
  return nullptr;
}

// binding.rmdir(path)
static napi_value fsRmdir(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  std::string path = getStringArg(env, argv[0]);

  uv_fs_t req;
  int result = uv_fs_rmdir(nullptr, &req, path.c_str(), nullptr);
  uv_fs_req_cleanup(&req);
  if (result < 0) {
    return throwUVException(env, result, "rmdir", path.c_str());
  }
  return nullptr;
}

// binding.readdir(path, encoding, withFileTypes)
static napi_value fsReaddir(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  std::string path = getStringArg(env, argv[0]);
  // encoding is argv[1] - we always return UTF-8 strings.
  bool withFileTypes = argc > 2 && getBool(env, argv[2], false);

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
    // Return array of name strings.
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

// binding.chmod(path, mode)
static napi_value fsChmod(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  std::string path = getStringArg(env, argv[0]);
  int mode = getInt32(env, argv[1]);

  uv_fs_t req;
  int result = uv_fs_chmod(nullptr, &req, path.c_str(), mode, nullptr);
  uv_fs_req_cleanup(&req);
  if (result < 0) {
    return throwUVException(env, result, "chmod", path.c_str());
  }
  return nullptr;
}

// binding.fchmod(fd, mode)
static napi_value fsFchmod(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  int fd = getInt32(env, argv[0]);
  int mode = getInt32(env, argv[1]);

  uv_fs_t req;
  int result = uv_fs_fchmod(nullptr, &req, fd, mode, nullptr);
  uv_fs_req_cleanup(&req);
  if (result < 0) {
    return throwUVException(env, result, "fchmod");
  }
  return nullptr;
}

// binding.chown(path, uid, gid)
static napi_value fsChown(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  std::string path = getStringArg(env, argv[0]);
  int uid = getInt32(env, argv[1]);
  int gid = getInt32(env, argv[2]);

  uv_fs_t req;
  int result = uv_fs_chown(nullptr, &req, path.c_str(), uid, gid, nullptr);
  uv_fs_req_cleanup(&req);
  if (result < 0) {
    return throwUVException(env, result, "chown", path.c_str());
  }
  return nullptr;
}

// binding.fchown(fd, uid, gid)
static napi_value fsFchown(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  int fd = getInt32(env, argv[0]);
  int uid = getInt32(env, argv[1]);
  int gid = getInt32(env, argv[2]);

  uv_fs_t req;
  int result = uv_fs_fchown(nullptr, &req, fd, uid, gid, nullptr);
  uv_fs_req_cleanup(&req);
  if (result < 0) {
    return throwUVException(env, result, "fchown");
  }
  return nullptr;
}

// binding.lchown(path, uid, gid)
static napi_value fsLchown(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  std::string path = getStringArg(env, argv[0]);
  int uid = getInt32(env, argv[1]);
  int gid = getInt32(env, argv[2]);

  uv_fs_t req;
  int result = uv_fs_lchown(nullptr, &req, path.c_str(), uid, gid, nullptr);
  uv_fs_req_cleanup(&req);
  if (result < 0) {
    return throwUVException(env, result, "lchown", path.c_str());
  }
  return nullptr;
}

// binding.link(existingPath, newPath)
static napi_value fsLink(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  std::string existingPath = getStringArg(env, argv[0]);
  std::string newPath = getStringArg(env, argv[1]);

  uv_fs_t req;
  int result = uv_fs_link(nullptr, &req, existingPath.c_str(), newPath.c_str(), nullptr);
  uv_fs_req_cleanup(&req);
  if (result < 0) {
    return throwUVException(env, result, "link", existingPath.c_str(), newPath.c_str());
  }
  return nullptr;
}

// binding.symlink(target, path, flags)
static napi_value fsSymlink(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  std::string target = getStringArg(env, argv[0]);
  std::string path = getStringArg(env, argv[1]);
  int flags = argc > 2 ? getInt32(env, argv[2], 0) : 0;

  uv_fs_t req;
  int result = uv_fs_symlink(nullptr, &req, target.c_str(), path.c_str(), flags, nullptr);
  uv_fs_req_cleanup(&req);
  if (result < 0) {
    return throwUVException(env, result, "symlink", target.c_str(), path.c_str());
  }
  return nullptr;
}

// binding.readlink(path, encoding)
static napi_value fsReadlink(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  std::string path = getStringArg(env, argv[0]);
  // encoding is argv[1] — we always return a UTF-8 string.

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

// binding.realpath(path, encoding)
static napi_value fsRealpath(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  std::string path = getStringArg(env, argv[0]);

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

// binding.ftruncate(fd, len)
static napi_value fsFtruncate(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  int fd = getInt32(env, argv[0]);
  int64_t len = getInt64(env, argv[1], 0);

  uv_fs_t req;
  int result = uv_fs_ftruncate(nullptr, &req, fd, len, nullptr);
  uv_fs_req_cleanup(&req);
  if (result < 0) {
    return throwUVException(env, result, "ftruncate");
  }
  return nullptr;
}

// binding.utimes(path, atime, mtime)
static napi_value fsUtimes(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  std::string path = getStringArg(env, argv[0]);
  double atime = getDouble(env, argv[1]);
  double mtime = getDouble(env, argv[2]);

  uv_fs_t req;
  int result = uv_fs_utime(nullptr, &req, path.c_str(), atime, mtime, nullptr);
  uv_fs_req_cleanup(&req);
  if (result < 0) {
    return throwUVException(env, result, "utime", path.c_str());
  }
  return nullptr;
}

// binding.futimes(fd, atime, mtime)
static napi_value fsFutimes(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  int fd = getInt32(env, argv[0]);
  double atime = getDouble(env, argv[1]);
  double mtime = getDouble(env, argv[2]);

  uv_fs_t req;
  int result = uv_fs_futime(nullptr, &req, fd, atime, mtime, nullptr);
  uv_fs_req_cleanup(&req);
  if (result < 0) {
    return throwUVException(env, result, "futime");
  }
  return nullptr;
}

// binding.lutimes(path, atime, mtime)
static napi_value fsLutimes(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  std::string path = getStringArg(env, argv[0]);
  double atime = getDouble(env, argv[1]);
  double mtime = getDouble(env, argv[2]);

  uv_fs_t req;
  int result = uv_fs_lutime(nullptr, &req, path.c_str(), atime, mtime, nullptr);
  uv_fs_req_cleanup(&req);
  if (result < 0) {
    return throwUVException(env, result, "lutime", path.c_str());
  }
  return nullptr;
}

// binding.mkdtemp(prefix, encoding)
static napi_value fsMkdtemp(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  std::string prefix = getStringArg(env, argv[0]);
  // libuv requires "XXXXXX" suffix for the template.
  std::string tmpl = prefix + "XXXXXX";

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

// binding.copyFile(src, dest, mode)
static napi_value fsCopyFile(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  std::string src = getStringArg(env, argv[0]);
  std::string dest = getStringArg(env, argv[1]);
  int flags = argc > 2 ? getInt32(env, argv[2], 0) : 0;

  uv_fs_t req;
  int result = uv_fs_copyfile(nullptr, &req, src.c_str(), dest.c_str(), flags, nullptr);
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

// binding.fsync(fd)
static napi_value fsFsync(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  int fd = getInt32(env, argv[0]);

  uv_fs_t req;
  int result = uv_fs_fsync(nullptr, &req, fd, nullptr);
  uv_fs_req_cleanup(&req);
  if (result < 0) {
    return throwUVException(env, result, "fsync");
  }
  return nullptr;
}

// binding.fdatasync(fd)
static napi_value fsFdatasync(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  int fd = getInt32(env, argv[0]);

  uv_fs_t req;
  int result = uv_fs_fdatasync(nullptr, &req, fd, nullptr);
  uv_fs_req_cleanup(&req);
  if (result < 0) {
    return throwUVException(env, result, "fdatasync");
  }
  return nullptr;
}

// binding.readFileUtf8(path, flags)
// Fast path for reading entire file as UTF-8 string.
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
    uv_buf_t buf = uv_buf_init(&content[0], static_cast<unsigned int>(fileSize));
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
// Fast path for writing string data as UTF-8.
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
      uv_fs_t closeReq;
      uv_fs_close(nullptr, &closeReq, fd, nullptr);
      uv_fs_req_cleanup(&closeReq);
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

// binding.readBuffers(fd, buffers, position) - read into array of buffers
static napi_value fsReadBuffers(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3];
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
    napi_get_typedarray_info(env, elem, &arrType, &length, &data, nullptr, nullptr);
    bufs[i] = uv_buf_init(static_cast<char *>(data), static_cast<unsigned int>(length));
  }

  int64_t position = -1;
  if (argc > 2 && !isNullOrUndefined(env, argv[2])) {
    position = getInt64(env, argv[2], -1);
  }

  uv_fs_t req;
  int result = uv_fs_read(nullptr, &req, fd, bufs.data(), numBuffers, position, nullptr);
  uv_fs_req_cleanup(&req);
  if (result < 0) {
    return throwUVException(env, result, "read");
  }

  napi_value jsResult;
  napi_create_int32(env, result, &jsResult);
  return jsResult;
}

// binding.writeBuffers(fd, buffers, position, undefined, ctx)
static napi_value fsWriteBuffers(napi_env env, napi_callback_info info) {
  size_t argc = 5;
  napi_value argv[5];
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
    napi_get_typedarray_info(env, elem, &arrType, &length, &data, nullptr, nullptr);
    bufs[i] = uv_buf_init(static_cast<char *>(data), static_cast<unsigned int>(length));
  }

  int64_t position = -1;
  if (argc > 2 && !isNullOrUndefined(env, argv[2])) {
    position = getInt64(env, argv[2], -1);
  }

  uv_fs_t req;
  int result = uv_fs_write(nullptr, &req, fd, bufs.data(), numBuffers, position, nullptr);
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
  // First stat to determine type.
  uv_fs_t statReq;
  int statResult = uv_fs_lstat(nullptr, &statReq, path.c_str(), nullptr);
  if (statResult < 0) {
    uv_fs_req_cleanup(&statReq);
    // If it doesn't exist, that's OK for rm with force (caller handles).
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

  // Collect all entries depth-first, then remove in reverse order.
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
        // Need to stat to determine type.
        uv_fs_t childStat;
        int childStatResult = uv_fs_lstat(nullptr, &childStat, childPath.c_str(), nullptr);
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
// FSReqCallback constructor (stub for now — needed for async ops in Step 28)
// ---------------------------------------------------------------------------

static napi_value fsReqCallbackCtor(napi_env env, napi_callback_info info) {
  napi_value thisVal;
  napi_get_cb_info(env, info, nullptr, nullptr, &thisVal, nullptr);
  return thisVal;
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
    napi_create_arraybuffer(env, kFsStatsBufferLength * sizeof(double), &data, &arrBuf);
    napi_value typedArr;
    napi_create_typedarray(env, napi_float64_array, kFsStatsBufferLength, arrBuf, 0, &typedArr);
    napi_create_reference(env, typedArr, 1, &fsData->statValuesRef);
    napi_set_named_property(env, exports, "statValues", typedArr);
  }
  {
    napi_value arrBuf;
    void *data;
    napi_create_arraybuffer(env, kFsStatsBufferLength * sizeof(int64_t), &data, &arrBuf);
    napi_value typedArr;
    napi_create_typedarray(env, napi_bigint64_array, kFsStatsBufferLength, arrBuf, 0, &typedArr);
    napi_create_reference(env, typedArr, 1, &fsData->bigintStatValuesRef);
    napi_set_named_property(env, exports, "bigintStatValues", typedArr);
  }
  {
    napi_value arrBuf;
    void *data;
    napi_create_arraybuffer(env, kStatFsBufferLength * sizeof(double), &data, &arrBuf);
    napi_value typedArr;
    napi_create_typedarray(env, napi_float64_array, kStatFsBufferLength, arrBuf, 0, &typedArr);
    napi_create_reference(env, typedArr, 1, &fsData->statFsValuesRef);
    napi_set_named_property(env, exports, "statFsValues", typedArr);
  }
  {
    napi_value arrBuf;
    void *data;
    napi_create_arraybuffer(env, kStatFsBufferLength * sizeof(int64_t), &data, &arrBuf);
    napi_value typedArr;
    napi_create_typedarray(env, napi_bigint64_array, kStatFsBufferLength, arrBuf, 0, &typedArr);
    napi_create_reference(env, typedArr, 1, &fsData->bigintStatFsValuesRef);
    napi_set_named_property(env, exports, "bigintStatFsValues", typedArr);
  }

  // FSReqCallback constructor.
  {
    napi_value ctor;
    napi_create_function(env, "FSReqCallback", NAPI_AUTO_LENGTH, fsReqCallbackCtor, nullptr, &ctor);
    napi_set_named_property(env, exports, "FSReqCallback", ctor);
  }

  // Register all sync fs functions. Stat functions need fsData as callback data.
#define SET_FN(name, fn) \
  do { \
    napi_value fnVal; \
    napi_create_function(env, name, NAPI_AUTO_LENGTH, fn, nullptr, &fnVal); \
    napi_set_named_property(env, exports, name, fnVal); \
  } while (0)

#define SET_FN_DATA(name, fn, data) \
  do { \
    napi_value fnVal; \
    napi_create_function(env, name, NAPI_AUTO_LENGTH, fn, data, &fnVal); \
    napi_set_named_property(env, exports, name, fnVal); \
  } while (0)

  SET_FN("access", fsAccess);
  SET_FN("open", fsOpen);
  SET_FN("close", fsClose);
  SET_FN("read", fsRead);
  SET_FN("writeBuffer", fsWriteBuffer);
  SET_FN("writeString", fsWriteString);
  SET_FN_DATA("stat", fsStat, fsData);
  SET_FN_DATA("lstat", fsLstat, fsData);
  SET_FN_DATA("fstat", fsFstat, fsData);
  SET_FN_DATA("statfs", fsStatFs, fsData);
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
  SET_FN("readBuffers", fsReadBuffers);
  SET_FN("writeBuffers", fsWriteBuffers);
  SET_FN("rmSync", fsRmSync);

#undef SET_FN
#undef SET_FN_DATA

  // Add finalizer to clean up fsData.
  napi_add_finalizer(env, exports, fsData, [](napi_env e, void *data, void *) {
    auto *d = static_cast<FsBindingData *>(data);
    napi_delete_reference(e, d->statValuesRef);
    napi_delete_reference(e, d->bigintStatValuesRef);
    napi_delete_reference(e, d->statFsValuesRef);
    napi_delete_reference(e, d->bigintStatFsValuesRef);
    delete d;
  }, nullptr, nullptr);

  return exports;
}

} // namespace node_compat
} // namespace hermes
