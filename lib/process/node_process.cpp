/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <hermes/node-compat/process/node_process.h>

#include <js_native_api.h>
#include <node_api.h>

#include <uv.h>

#include <sys/types.h>
#include <unistd.h>
#include <cassert>
#include <cerrno>
#include <cstdlib>
#include <cstring>

// Helper macros for NAPI error checking.
#define NAPI_RETURN_IF_NOT_OK(expr) \
  do {                              \
    napi_status s__ = (expr);       \
    if (s__ != napi_ok)             \
      return s__;                   \
  } while (0)

#define NAPI_THROW_IF_NOT_OK(env, expr, msg) \
  do {                                       \
    napi_status s__ = (expr);                \
    if (s__ != napi_ok) {                    \
      napi_throw_error(env, nullptr, msg);   \
      return nullptr;                        \
    }                                        \
  } while (0)

namespace hermes {
namespace node_compat {

// ============================================================================
// process.env implementation
//
// We create a JS Proxy object backed by native getter/setter/deleter functions
// that call getenv/setenv/unsetenv. The Proxy handler implements: get, set,
// deleteProperty, has, ownKeys, getOwnPropertyDescriptor, enumerate.
// ============================================================================

/// Helper: get environment variable by name. Returns empty string and sets
/// *found=false if not present.
static std::string getEnvVar(const char *name, bool *found) {
  const char *val = getenv(name);
  if (val) {
    *found = true;
    return std::string(val);
  }
  *found = false;
  return {};
}

/// Proxy handler: get(target, prop, receiver)
static napi_value envProxyGet(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  // prop is argv[1]
  napi_valuetype propType;
  napi_typeof(env, argv[1], &propType);

  // Only handle string property access.
  if (propType == napi_symbol) {
    // Return undefined for symbol access (e.g. Symbol.toPrimitive).
    napi_value undef;
    napi_get_undefined(env, &undef);
    return undef;
  }

  if (propType != napi_string) {
    napi_value undef;
    napi_get_undefined(env, &undef);
    return undef;
  }

  char nameBuf[4096];
  size_t nameLen = 0;
  napi_get_value_string_utf8(env, argv[1], nameBuf, sizeof(nameBuf), &nameLen);

  bool found;
  std::string val = getEnvVar(nameBuf, &found);
  if (!found) {
    napi_value undef;
    napi_get_undefined(env, &undef);
    return undef;
  }

  napi_value result;
  napi_create_string_utf8(env, val.c_str(), val.size(), &result);
  return result;
}

/// Proxy handler: set(target, prop, value, receiver)
static napi_value envProxySet(napi_env env, napi_callback_info info) {
  size_t argc = 4;
  napi_value argv[4];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  napi_valuetype propType;
  napi_typeof(env, argv[1], &propType);
  if (propType != napi_string) {
    napi_value trueVal;
    napi_get_boolean(env, true, &trueVal);
    return trueVal;
  }

  char nameBuf[4096];
  size_t nameLen = 0;
  napi_get_value_string_utf8(env, argv[1], nameBuf, sizeof(nameBuf), &nameLen);

  // Coerce value to string.
  napi_value strVal;
  napi_coerce_to_string(env, argv[2], &strVal);

  char valBuf[32768];
  size_t valLen = 0;
  napi_get_value_string_utf8(env, strVal, valBuf, sizeof(valBuf), &valLen);

  setenv(nameBuf, valBuf, 1);

  napi_value trueVal;
  napi_get_boolean(env, true, &trueVal);
  return trueVal;
}

/// Proxy handler: deleteProperty(target, prop)
static napi_value envProxyDeleteProperty(
    napi_env env,
    napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  napi_valuetype propType;
  napi_typeof(env, argv[1], &propType);
  if (propType != napi_string) {
    napi_value trueVal;
    napi_get_boolean(env, true, &trueVal);
    return trueVal;
  }

  char nameBuf[4096];
  size_t nameLen = 0;
  napi_get_value_string_utf8(env, argv[1], nameBuf, sizeof(nameBuf), &nameLen);

  unsetenv(nameBuf);

  napi_value trueVal;
  napi_get_boolean(env, true, &trueVal);
  return trueVal;
}

/// Proxy handler: has(target, prop)
static napi_value envProxyHas(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  napi_valuetype propType;
  napi_typeof(env, argv[1], &propType);
  if (propType != napi_string) {
    napi_value falseVal;
    napi_get_boolean(env, false, &falseVal);
    return falseVal;
  }

  char nameBuf[4096];
  size_t nameLen = 0;
  napi_get_value_string_utf8(env, argv[1], nameBuf, sizeof(nameBuf), &nameLen);

  const char *val = getenv(nameBuf);
  napi_value result;
  napi_get_boolean(env, val != nullptr, &result);
  return result;
}

/// Proxy handler: ownKeys(target)
/// Returns an array of all environment variable names.
static napi_value envProxyOwnKeys(napi_env env, napi_callback_info info) {
  uv_env_item_t *envItems = nullptr;
  int envCount = 0;
  int err = uv_os_environ(&envItems, &envCount);

  napi_value result;
  napi_create_array_with_length(env, envCount > 0 ? envCount : 0, &result);

  if (err == 0 && envItems) {
    for (int i = 0; i < envCount; i++) {
      napi_value name;
      napi_create_string_utf8(env, envItems[i].name, NAPI_AUTO_LENGTH, &name);
      napi_set_element(env, result, i, name);
    }
    uv_os_free_environ(envItems, envCount);
  }

  return result;
}

/// Proxy handler: getOwnPropertyDescriptor(target, prop)
static napi_value envProxyGetOwnPropertyDescriptor(
    napi_env env,
    napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  napi_valuetype propType;
  napi_typeof(env, argv[1], &propType);
  if (propType != napi_string) {
    napi_value undef;
    napi_get_undefined(env, &undef);
    return undef;
  }

  char nameBuf[4096];
  size_t nameLen = 0;
  napi_get_value_string_utf8(env, argv[1], nameBuf, sizeof(nameBuf), &nameLen);

  bool found;
  std::string val = getEnvVar(nameBuf, &found);
  if (!found) {
    napi_value undef;
    napi_get_undefined(env, &undef);
    return undef;
  }

  // Return a property descriptor: { value, writable: true, enumerable: true,
  // configurable: true }
  napi_value desc;
  napi_create_object(env, &desc);

  napi_value valJs;
  napi_create_string_utf8(env, val.c_str(), val.size(), &valJs);
  napi_set_named_property(env, desc, "value", valJs);

  napi_value trueVal;
  napi_get_boolean(env, true, &trueVal);
  napi_set_named_property(env, desc, "writable", trueVal);
  napi_set_named_property(env, desc, "enumerable", trueVal);
  napi_set_named_property(env, desc, "configurable", trueVal);

  return desc;
}

/// Create the process.env Proxy object.
static napi_status createEnvProxy(napi_env env, napi_value *result) {
  // We create the Proxy via JS: new Proxy({}, handler)
  // where handler is an object with our native trap functions.
  napi_value handler;
  NAPI_RETURN_IF_NOT_OK(napi_create_object(env, &handler));

  auto setTrap = [&](const char *name, napi_callback cb) -> napi_status {
    napi_value fn;
    NAPI_RETURN_IF_NOT_OK(
        napi_create_function(env, name, NAPI_AUTO_LENGTH, cb, nullptr, &fn));
    return napi_set_named_property(env, handler, name, fn);
  };

  NAPI_RETURN_IF_NOT_OK(setTrap("get", envProxyGet));
  NAPI_RETURN_IF_NOT_OK(setTrap("set", envProxySet));
  NAPI_RETURN_IF_NOT_OK(setTrap("deleteProperty", envProxyDeleteProperty));
  NAPI_RETURN_IF_NOT_OK(setTrap("has", envProxyHas));
  NAPI_RETURN_IF_NOT_OK(setTrap("ownKeys", envProxyOwnKeys));
  NAPI_RETURN_IF_NOT_OK(
      setTrap("getOwnPropertyDescriptor", envProxyGetOwnPropertyDescriptor));

  // Get the global Proxy constructor and create new Proxy({}, handler).
  napi_value global;
  NAPI_RETURN_IF_NOT_OK(napi_get_global(env, &global));

  napi_value proxyCtor;
  NAPI_RETURN_IF_NOT_OK(
      napi_get_named_property(env, global, "Proxy", &proxyCtor));

  napi_value target;
  NAPI_RETURN_IF_NOT_OK(napi_create_object(env, &target));

  napi_value args[2] = {target, handler};
  return napi_new_instance(env, proxyCtor, 2, args, result);
}

// ============================================================================
// process methods (callbacks)
// ============================================================================

/// process.cwd()
static napi_value processCwd(napi_env env, napi_callback_info /*info*/) {
  char buf[4096];
  size_t size = sizeof(buf);
  int err = uv_cwd(buf, &size);
  if (err != 0) {
    std::string msg = "uv_cwd failed: ";
    msg += uv_strerror(err);
    napi_throw_error(env, uv_err_name(err), msg.c_str());
    return nullptr;
  }
  napi_value result;
  napi_create_string_utf8(env, buf, size, &result);
  return result;
}

/// process.chdir(directory)
static napi_value processChdir(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  if (argc < 1) {
    napi_throw_type_error(
        env,
        "ERR_INVALID_ARG_TYPE",
        "The \"directory\" argument must be of type string");
    return nullptr;
  }

  char dirBuf[4096];
  size_t dirLen = 0;
  napi_status status =
      napi_get_value_string_utf8(env, argv[0], dirBuf, sizeof(dirBuf), &dirLen);
  if (status != napi_ok) {
    napi_throw_type_error(
        env,
        "ERR_INVALID_ARG_TYPE",
        "The \"directory\" argument must be of type string");
    return nullptr;
  }

  int err = uv_chdir(dirBuf);
  if (err != 0) {
    std::string msg = "chdir failed: ";
    msg += uv_strerror(err);
    napi_throw_error(env, uv_err_name(err), msg.c_str());
    return nullptr;
  }

  napi_value undef;
  napi_get_undefined(env, &undef);
  return undef;
}

/// process.hrtime([prevTime])
/// Returns [seconds, nanoseconds] as a two-element array.
static napi_value processHrtime(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  uint64_t t = uv_hrtime();

  uint64_t seconds = t / 1000000000ULL;
  uint64_t nanoseconds = t % 1000000000ULL;

  // If a previous time is provided, compute the delta.
  if (argc >= 1) {
    bool isArray = false;
    napi_is_array(env, argv[0], &isArray);
    if (isArray) {
      napi_value prevSec, prevNsec;
      napi_get_element(env, argv[0], 0, &prevSec);
      napi_get_element(env, argv[0], 1, &prevNsec);

      int64_t ps, pns;
      napi_get_value_int64(env, prevSec, &ps);
      napi_get_value_int64(env, prevNsec, &pns);

      int64_t deltaSec = (int64_t)seconds - ps;
      int64_t deltaNsec = (int64_t)nanoseconds - pns;
      if (deltaNsec < 0) {
        deltaSec--;
        deltaNsec += 1000000000LL;
      }

      napi_value result;
      napi_create_array_with_length(env, 2, &result);
      napi_value v0, v1;
      napi_create_int64(env, deltaSec, &v0);
      napi_create_int64(env, deltaNsec, &v1);
      napi_set_element(env, result, 0, v0);
      napi_set_element(env, result, 1, v1);
      return result;
    }
  }

  napi_value result;
  napi_create_array_with_length(env, 2, &result);
  napi_value v0, v1;
  napi_create_int64(env, (int64_t)seconds, &v0);
  napi_create_int64(env, (int64_t)nanoseconds, &v1);
  napi_set_element(env, result, 0, v0);
  napi_set_element(env, result, 1, v1);
  return result;
}

/// process.hrtime.bigint()
/// Returns nanoseconds since an arbitrary epoch as a BigInt.
static napi_value processHrtimeBigint(
    napi_env env,
    napi_callback_info /*info*/) {
  uint64_t t = uv_hrtime();
  napi_value result;
  // napi_create_bigint_uint64 takes a uint64_t.
  napi_status status = napi_create_bigint_uint64(env, t, &result);
  if (status != napi_ok) {
    napi_throw_error(env, nullptr, "Failed to create BigInt for hrtime");
    return nullptr;
  }
  return result;
}

/// process.cpuUsage([prevValue])
/// Returns { user, system } in microseconds.
static napi_value processCpuUsage(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  uv_rusage_t rusage;
  int err = uv_getrusage(&rusage);
  if (err != 0) {
    napi_throw_error(env, nullptr, "uv_getrusage failed");
    return nullptr;
  }

  double userUsec =
      1e6 * rusage.ru_utime.tv_sec + (double)rusage.ru_utime.tv_usec;
  double sysUsec =
      1e6 * rusage.ru_stime.tv_sec + (double)rusage.ru_stime.tv_usec;

  // If prevValue is provided, compute the delta.
  if (argc >= 1) {
    napi_valuetype argType;
    napi_typeof(env, argv[0], &argType);
    if (argType == napi_object) {
      napi_value prevUser, prevSystem;
      napi_get_named_property(env, argv[0], "user", &prevUser);
      napi_get_named_property(env, argv[0], "system", &prevSystem);

      double pu, ps;
      napi_get_value_double(env, prevUser, &pu);
      napi_get_value_double(env, prevSystem, &ps);

      userUsec -= pu;
      sysUsec -= ps;
    }
  }

  napi_value result;
  napi_create_object(env, &result);

  napi_value userVal, sysVal;
  napi_create_double(env, userUsec, &userVal);
  napi_create_double(env, sysUsec, &sysVal);
  napi_set_named_property(env, result, "user", userVal);
  napi_set_named_property(env, result, "system", sysVal);

  return result;
}

/// process.memoryUsage()
/// Returns { rss, heapTotal, heapUsed, external, arrayBuffers }.
static napi_value processMemoryUsage(
    napi_env env,
    napi_callback_info /*info*/) {
  size_t rss = 0;
  int err = uv_resident_set_memory(&rss);
  if (err != 0) {
    napi_throw_error(env, nullptr, "uv_resident_set_memory failed");
    return nullptr;
  }

  // We don't have direct access to Hermes heap stats via NAPI, so we
  // provide rss and stub the rest.
  napi_value result;
  napi_create_object(env, &result);

  auto setField = [&](const char *name, double val) {
    napi_value v;
    napi_create_double(env, val, &v);
    napi_set_named_property(env, result, name, v);
  };

  setField("rss", (double)rss);
  setField("heapTotal", 0);
  setField("heapUsed", 0);
  setField("external", 0);
  setField("arrayBuffers", 0);

  return result;
}

/// process.uptime()
/// Returns seconds since process start.
static napi_value processUptime(napi_env env, napi_callback_info info) {
  void *data = nullptr;
  napi_get_cb_info(env, info, nullptr, nullptr, nullptr, &data);
  auto *proc = static_cast<NodeProcess *>(data);

  uint64_t now = uv_hrtime();
  double uptimeSec = (double)(now - proc->getStartTime()) / 1e9;

  napi_value result;
  napi_create_double(env, uptimeSec, &result);
  return result;
}

/// process.exit([code])
static napi_value processExit(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  int32_t code = 0;
  if (argc >= 1) {
    napi_get_value_int32(env, argv[0], &code);
  }

  // Use _exit() to skip atexit handlers (including ASAN leak detection).
  // The Hermes runtime is a stack variable in runBootstrap() and won't be
  // destructed by exit(), causing ASAN to report massive false-positive
  // leaks and the symbolizer to hang on the large ASAN binary.
  // TODO: Properly integrate with event loop shutdown instead of
  // terminating immediately.
  _exit(code);
  // Unreachable, but satisfy the compiler.
  return nullptr;
}

/// process.abort()
static napi_value processAbort(napi_env /*env*/, napi_callback_info /*info*/) {
  abort();
  return nullptr;
}

/// process.umask([mask])
static napi_value processUmask(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  uint32_t old;

  if (argc >= 1) {
    napi_valuetype argType;
    napi_typeof(env, argv[0], &argType);
    if (argType == napi_number) {
      uint32_t mask;
      napi_get_value_uint32(env, argv[0], &mask);
      old = umask(static_cast<mode_t>(mask));
    } else if (argType == napi_undefined) {
      // Query current umask without changing it.
      old = umask(0);
      umask(static_cast<mode_t>(old));
    } else {
      napi_throw_type_error(
          env,
          "ERR_INVALID_ARG_TYPE",
          "The \"mask\" argument must be a number or undefined");
      return nullptr;
    }
  } else {
    // No arguments: query current umask.
    old = umask(0);
    umask(static_cast<mode_t>(old));
  }

  napi_value result;
  napi_create_uint32(env, old, &result);
  return result;
}

/// process.kill(pid, signal)
static napi_value processKill(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  if (argc < 2) {
    napi_throw_type_error(
        env, "ERR_MISSING_ARGS", "process.kill requires pid and signal");
    return nullptr;
  }

  int32_t pid;
  napi_get_value_int32(env, argv[0], &pid);

  int32_t sig;
  napi_get_value_int32(env, argv[1], &sig);

  int err = uv_kill(pid, sig);
  if (err != 0) {
    std::string msg = "kill failed: ";
    msg += uv_strerror(err);
    napi_throw_error(env, uv_err_name(err), msg.c_str());
    return nullptr;
  }

  napi_value undef;
  napi_get_undefined(env, &undef);
  return undef;
}

/// Getter for process.title.
static napi_value processTitleGetter(
    napi_env env,
    napi_callback_info /*info*/) {
  char buf[256];
  int err = uv_get_process_title(buf, sizeof(buf));
  if (err != 0) {
    napi_value empty;
    napi_create_string_utf8(env, "", 0, &empty);
    return empty;
  }
  napi_value result;
  napi_create_string_utf8(env, buf, NAPI_AUTO_LENGTH, &result);
  return result;
}

/// Setter for process.title.
static napi_value processTitleSetter(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  if (argc < 1)
    return nullptr;

  char buf[256];
  size_t len = 0;
  napi_get_value_string_utf8(env, argv[0], buf, sizeof(buf), &len);
  uv_set_process_title(buf);

  return nullptr;
}

// ============================================================================
// NodeProcess implementation
// ============================================================================

NodeProcess::NodeProcess() : startTime_(uv_hrtime()) {}

NodeProcess::~NodeProcess() = default;

void NodeProcess::setArgv(std::vector<std::string> argv) {
  argv_ = std::move(argv);
}

void NodeProcess::setExecPath(std::string execPath) {
  execPath_ = std::move(execPath);
}

// Helper: set a named property on an object.
static napi_status
setProp(napi_env env, napi_value obj, const char *name, napi_value val) {
  return napi_set_named_property(env, obj, name, val);
}

// Helper: set a string property.
static napi_status
setStringProp(napi_env env, napi_value obj, const char *name, const char *val) {
  napi_value v;
  NAPI_RETURN_IF_NOT_OK(
      napi_create_string_utf8(env, val, NAPI_AUTO_LENGTH, &v));
  return setProp(env, obj, name, v);
}

// Helper: set an int32 property.
static napi_status
setInt32Prop(napi_env env, napi_value obj, const char *name, int32_t val) {
  napi_value v;
  NAPI_RETURN_IF_NOT_OK(napi_create_int32(env, val, &v));
  return setProp(env, obj, name, v);
}

// Helper: set a method on an object.
static napi_status setMethod(
    napi_env env,
    napi_value obj,
    const char *name,
    napi_callback cb,
    void *data = nullptr) {
  napi_value fn;
  NAPI_RETURN_IF_NOT_OK(
      napi_create_function(env, name, NAPI_AUTO_LENGTH, cb, data, &fn));
  return setProp(env, obj, name, fn);
}

napi_status NodeProcess::create(napi_env env, napi_value *result) {
  napi_value process;
  NAPI_RETURN_IF_NOT_OK(napi_create_object(env, &process));

  // --- Static properties ---

  // process.pid
  NAPI_RETURN_IF_NOT_OK(
      setInt32Prop(env, process, "pid", static_cast<int32_t>(uv_os_getpid())));

  // process.ppid
  NAPI_RETURN_IF_NOT_OK(setInt32Prop(
      env, process, "ppid", static_cast<int32_t>(uv_os_getppid())));

  // process.platform
#if defined(__linux__)
  NAPI_RETURN_IF_NOT_OK(setStringProp(env, process, "platform", "linux"));
#elif defined(__APPLE__)
  NAPI_RETURN_IF_NOT_OK(setStringProp(env, process, "platform", "darwin"));
#elif defined(_WIN32)
  NAPI_RETURN_IF_NOT_OK(setStringProp(env, process, "platform", "win32"));
#else
  NAPI_RETURN_IF_NOT_OK(setStringProp(env, process, "platform", "unknown"));
#endif

  // process.arch
#if defined(__x86_64__) || defined(_M_X64)
  NAPI_RETURN_IF_NOT_OK(setStringProp(env, process, "arch", "x64"));
#elif defined(__aarch64__) || defined(_M_ARM64)
  NAPI_RETURN_IF_NOT_OK(setStringProp(env, process, "arch", "arm64"));
#elif defined(__arm__) || defined(_M_ARM)
  NAPI_RETURN_IF_NOT_OK(setStringProp(env, process, "arch", "arm"));
#elif defined(__i386__) || defined(_M_IX86)
  NAPI_RETURN_IF_NOT_OK(setStringProp(env, process, "arch", "ia32"));
#else
  NAPI_RETURN_IF_NOT_OK(setStringProp(env, process, "arch", "unknown"));
#endif

  // process.version
  NAPI_RETURN_IF_NOT_OK(
      setStringProp(env, process, "version", "v0.1.0-hermes"));

  // process.versions
  {
    napi_value versions;
    NAPI_RETURN_IF_NOT_OK(napi_create_object(env, &versions));
    NAPI_RETURN_IF_NOT_OK(setStringProp(env, versions, "hermes", "0.1.0"));
    NAPI_RETURN_IF_NOT_OK(
        setStringProp(env, versions, "uv", uv_version_string()));
    NAPI_RETURN_IF_NOT_OK(setStringProp(env, versions, "node", "24.13.0"));
    NAPI_RETURN_IF_NOT_OK(setProp(env, process, "versions", versions));
  }

  // process.features
  {
    napi_value features;
    NAPI_RETURN_IF_NOT_OK(napi_create_object(env, &features));
    napi_value falseVal;
    NAPI_RETURN_IF_NOT_OK(napi_get_boolean(env, false, &falseVal));
    NAPI_RETURN_IF_NOT_OK(setProp(env, features, "inspector", falseVal));
    NAPI_RETURN_IF_NOT_OK(setProp(env, features, "tls", falseVal));
    NAPI_RETURN_IF_NOT_OK(setProp(env, features, "ipv6", falseVal));
    NAPI_RETURN_IF_NOT_OK(setProp(env, process, "features", features));
  }

  // process.argv
  {
    napi_value argvArray;
    NAPI_RETURN_IF_NOT_OK(
        napi_create_array_with_length(env, argv_.size(), &argvArray));
    for (size_t i = 0; i < argv_.size(); i++) {
      napi_value v;
      NAPI_RETURN_IF_NOT_OK(
          napi_create_string_utf8(env, argv_[i].c_str(), argv_[i].size(), &v));
      NAPI_RETURN_IF_NOT_OK(napi_set_element(env, argvArray, i, v));
    }
    NAPI_RETURN_IF_NOT_OK(setProp(env, process, "argv", argvArray));
  }

  // process.execPath
  {
    std::string ep = execPath_;
    if (ep.empty()) {
      // Try to get from /proc/self/exe or uv_exepath.
      char buf[4096];
      size_t size = sizeof(buf);
      if (uv_exepath(buf, &size) == 0) {
        ep.assign(buf, size);
      }
    }
    NAPI_RETURN_IF_NOT_OK(setStringProp(env, process, "execPath", ep.c_str()));
  }

  // process.title (getter/setter via defineProperty)
  {
    napi_property_descriptor titleProp = {
        "title", // utf8name
        nullptr, // name
        nullptr, // method
        processTitleGetter, // getter
        processTitleSetter, // setter
        nullptr, // value
        napi_enumerable, // attributes
        nullptr // data
    };
    NAPI_RETURN_IF_NOT_OK(napi_define_properties(env, process, 1, &titleProp));
  }

  // process.env
  {
    napi_value envProxy;
    NAPI_RETURN_IF_NOT_OK(createEnvProxy(env, &envProxy));
    NAPI_RETURN_IF_NOT_OK(setProp(env, process, "env", envProxy));
  }

  // --- Methods ---

  NAPI_RETURN_IF_NOT_OK(setMethod(env, process, "cwd", processCwd));
  NAPI_RETURN_IF_NOT_OK(setMethod(env, process, "chdir", processChdir));
  NAPI_RETURN_IF_NOT_OK(setMethod(env, process, "kill", processKill));

  // process.hrtime and process.hrtime.bigint
  {
    napi_value hrtimeFn;
    NAPI_RETURN_IF_NOT_OK(napi_create_function(
        env, "hrtime", NAPI_AUTO_LENGTH, processHrtime, nullptr, &hrtimeFn));

    napi_value bigintFn;
    NAPI_RETURN_IF_NOT_OK(napi_create_function(
        env,
        "bigint",
        NAPI_AUTO_LENGTH,
        processHrtimeBigint,
        nullptr,
        &bigintFn));

    NAPI_RETURN_IF_NOT_OK(setProp(env, hrtimeFn, "bigint", bigintFn));
    NAPI_RETURN_IF_NOT_OK(setProp(env, process, "hrtime", hrtimeFn));
  }

  NAPI_RETURN_IF_NOT_OK(setMethod(env, process, "cpuUsage", processCpuUsage));
  NAPI_RETURN_IF_NOT_OK(
      setMethod(env, process, "memoryUsage", processMemoryUsage));
  NAPI_RETURN_IF_NOT_OK(setMethod(env, process, "uptime", processUptime, this));
  NAPI_RETURN_IF_NOT_OK(setMethod(env, process, "exit", processExit));
  NAPI_RETURN_IF_NOT_OK(setMethod(env, process, "abort", processAbort));
  NAPI_RETURN_IF_NOT_OK(setMethod(env, process, "umask", processUmask));

  // Cache the process object.
  NAPI_RETURN_IF_NOT_OK(napi_create_reference(env, process, 1, &processRef_));

  *result = process;
  return napi_ok;
}

void NodeProcess::detach(napi_env env) {
  if (processRef_) {
    napi_delete_reference(env, processRef_);
    processRef_ = nullptr;
  }
}

} // namespace node_compat
} // namespace hermes
