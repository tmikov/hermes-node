/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <hermes/node-compat/bindings/handle_wrap_base.h>
#include <hermes/node-compat/bindings/libuv_stream_base.h>
#include <hermes/node-compat/bindings/node_process_wrap.h>
#include <node_api.h>
#include <uv.h>

#include <cassert>
#include <csignal>
#include <cstring>
#include <string>
#include <vector>

namespace hermes {
namespace node_compat {

/// Convert a signal number to its string name (e.g. SIGTERM).
/// Returns empty string for unknown signals.
static const char *signoString(int signo) {
#define SIGNO_CASE(e) \
  case e:             \
    return #e;
  switch (signo) {
#ifdef SIGHUP
    SIGNO_CASE(SIGHUP);
#endif
#ifdef SIGINT
    SIGNO_CASE(SIGINT);
#endif
#ifdef SIGQUIT
    SIGNO_CASE(SIGQUIT);
#endif
#ifdef SIGILL
    SIGNO_CASE(SIGILL);
#endif
#ifdef SIGTRAP
    SIGNO_CASE(SIGTRAP);
#endif
#ifdef SIGABRT
    SIGNO_CASE(SIGABRT);
#endif
#ifdef SIGIOT
#if SIGABRT != SIGIOT
    SIGNO_CASE(SIGIOT);
#endif
#endif
#ifdef SIGBUS
    SIGNO_CASE(SIGBUS);
#endif
#ifdef SIGFPE
    SIGNO_CASE(SIGFPE);
#endif
#ifdef SIGKILL
    SIGNO_CASE(SIGKILL);
#endif
#ifdef SIGUSR1
    SIGNO_CASE(SIGUSR1);
#endif
#ifdef SIGSEGV
    SIGNO_CASE(SIGSEGV);
#endif
#ifdef SIGUSR2
    SIGNO_CASE(SIGUSR2);
#endif
#ifdef SIGPIPE
    SIGNO_CASE(SIGPIPE);
#endif
#ifdef SIGALRM
    SIGNO_CASE(SIGALRM);
#endif
    SIGNO_CASE(SIGTERM);
#ifdef SIGCHLD
    SIGNO_CASE(SIGCHLD);
#endif
#ifdef SIGSTKFLT
    SIGNO_CASE(SIGSTKFLT);
#endif
#ifdef SIGCONT
    SIGNO_CASE(SIGCONT);
#endif
#ifdef SIGSTOP
    SIGNO_CASE(SIGSTOP);
#endif
#ifdef SIGTSTP
    SIGNO_CASE(SIGTSTP);
#endif
#ifdef SIGTTIN
    SIGNO_CASE(SIGTTIN);
#endif
#ifdef SIGTTOU
    SIGNO_CASE(SIGTTOU);
#endif
#ifdef SIGURG
    SIGNO_CASE(SIGURG);
#endif
#ifdef SIGXCPU
    SIGNO_CASE(SIGXCPU);
#endif
#ifdef SIGXFSZ
    SIGNO_CASE(SIGXFSZ);
#endif
#ifdef SIGVTALRM
    SIGNO_CASE(SIGVTALRM);
#endif
#ifdef SIGPROF
    SIGNO_CASE(SIGPROF);
#endif
#ifdef SIGWINCH
    SIGNO_CASE(SIGWINCH);
#endif
#ifdef SIGIO
    SIGNO_CASE(SIGIO);
#endif
#ifdef SIGPOLL
#if SIGPOLL != SIGIO
    SIGNO_CASE(SIGPOLL);
#endif
#endif
#ifdef SIGLOST
#if SIGLOST != SIGABRT
    SIGNO_CASE(SIGLOST);
#endif
#endif
#ifdef SIGPWR
#if SIGPWR != SIGLOST
    SIGNO_CASE(SIGPWR);
#endif
#endif
#ifdef SIGINFO
#if !defined(SIGPWR) || SIGINFO != SIGPWR
    SIGNO_CASE(SIGINFO);
#endif
#endif
#ifdef SIGSYS
    SIGNO_CASE(SIGSYS);
#endif
    default:
      return "";
  }
#undef SIGNO_CASE
}

// ---------------------------------------------------------------------------
// ProcessWrap -- wraps uv_process_t, inherits from HandleWrapBase
// ---------------------------------------------------------------------------

class ProcessWrap : public HandleWrapBase {
 public:
  /// Construct a ProcessWrap. Note: uv_process_t is NOT initialized here;
  /// uv_spawn initializes it. We just zero-init and register with
  /// HandleWrapBase after a successful spawn.
  ProcessWrap() : process_() {
    memset(&process_, 0, sizeof(process_));
  }

 private:
  uv_process_t process_;

  // --- Helper to extract uv_stream_t* from a JS handle object ---

  static uv_stream_t *streamForWrap(napi_env env, napi_value stdioObj) {
    napi_value handleVal;
    napi_get_named_property(env, stdioObj, "handle", &handleVal);

    napi_valuetype handleType;
    napi_typeof(env, handleVal, &handleType);
    if (handleType != napi_object)
      return nullptr;

    auto *wrap = HandleWrapBase::unwrap(env, handleVal);
    if (!wrap)
      return nullptr;

    // The handle must be a stream type (LibuvStreamBase subclass).
    // We can safely cast since HandleWrapBase::handle() returns the
    // underlying uv_handle_t*.
    return reinterpret_cast<uv_stream_t *>(wrap->handle());
  }

  // --- Parse stdio options ---

  static bool parseStdioOptions(
      napi_env env,
      napi_value jsOptions,
      std::vector<uv_stdio_container_t> &containers) {
    napi_value stdioVal;
    napi_get_named_property(env, jsOptions, "stdio", &stdioVal);

    bool isArray = false;
    napi_is_array(env, stdioVal, &isArray);
    if (!isArray)
      return false;

    uint32_t len = 0;
    napi_get_array_length(env, stdioVal, &len);
    containers.resize(len);

    for (uint32_t i = 0; i < len; i++) {
      napi_value elem;
      napi_get_element(env, stdioVal, i, &elem);

      napi_value typeVal;
      napi_get_named_property(env, elem, "type", &typeVal);

      char typeBuf[32];
      size_t typeLen = 0;
      napi_get_value_string_utf8(
          env, typeVal, typeBuf, sizeof(typeBuf), &typeLen);

      if (strcmp(typeBuf, "ignore") == 0) {
        containers[i].flags = UV_IGNORE;
      } else if (strcmp(typeBuf, "pipe") == 0) {
        containers[i].flags = static_cast<uv_stdio_flags>(
            UV_CREATE_PIPE | UV_READABLE_PIPE | UV_WRITABLE_PIPE);
        uv_stream_t *stream = streamForWrap(env, elem);
        if (!stream)
          return false;
        containers[i].data.stream = stream;
      } else if (strcmp(typeBuf, "overlapped") == 0) {
        containers[i].flags = static_cast<uv_stdio_flags>(
            UV_CREATE_PIPE | UV_READABLE_PIPE | UV_WRITABLE_PIPE |
            UV_OVERLAPPED_PIPE);
        uv_stream_t *stream = streamForWrap(env, elem);
        if (!stream)
          return false;
        containers[i].data.stream = stream;
      } else if (strcmp(typeBuf, "wrap") == 0) {
        containers[i].flags = UV_INHERIT_STREAM;
        uv_stream_t *stream = streamForWrap(env, elem);
        if (!stream)
          return false;
        containers[i].data.stream = stream;
      } else {
        // fd-based: "inherit", "fd", or any other type with an fd field.
        napi_value fdVal;
        napi_get_named_property(env, elem, "fd", &fdVal);
        int32_t fd = 0;
        napi_get_value_int32(env, fdVal, &fd);
        containers[i].flags = UV_INHERIT_FD;
        containers[i].data.fd = fd;
      }
    }

    return true;
  }

  // --- Helper to extract string array from JS array ---

  static void extractStringArray(
      napi_env env,
      napi_value arrayVal,
      std::vector<std::string> &strings,
      std::vector<char *> &ptrs) {
    bool isArray = false;
    napi_is_array(env, arrayVal, &isArray);
    if (!isArray)
      return;

    uint32_t len = 0;
    napi_get_array_length(env, arrayVal, &len);
    strings.reserve(len);

    for (uint32_t i = 0; i < len; i++) {
      napi_value elem;
      napi_get_element(env, arrayVal, i, &elem);

      // Get string length first.
      size_t strLen = 0;
      napi_get_value_string_utf8(env, elem, nullptr, 0, &strLen);

      std::string s(strLen, '\0');
      napi_get_value_string_utf8(env, elem, &s[0], strLen + 1, &strLen);
      strings.push_back(std::move(s));
    }

    ptrs.resize(strings.size() + 1);
    for (size_t i = 0; i < strings.size(); i++) {
      ptrs[i] = const_cast<char *>(strings[i].c_str());
    }
    ptrs.back() = nullptr;
  }

  // --- NAPI callbacks ---

  /// new Process()
  static napi_value New(napi_env env, napi_callback_info info) {
    napi_handle_scope scope;
    napi_open_handle_scope(env, &scope);

    napi_value thisObj;
    napi_get_cb_info(env, info, nullptr, nullptr, &thisObj, nullptr);

    // Allocate ProcessWrap. It is NOT yet initialized by HandleWrapBase;
    // that happens in Spawn after a successful uv_spawn.
    auto *wrap = new ProcessWrap();

    // Store the wrap pointer in the JS object using napi_wrap with a
    // destructor. If GC collects this before spawn or after close, delete.
    napi_wrap(
        env,
        thisObj,
        wrap,
        [](napi_env /*env*/, void *data, void * /*hint*/) {
          auto *w = static_cast<ProcessWrap *>(data);
          if (w->state() == kClosed) {
            delete w;
          } else {
            // Handle is still active; close it first.
            w->doClose();
          }
        },
        nullptr,
        nullptr);

    napi_close_handle_scope(env, scope);
    return thisObj;
  }

  /// spawn(options) -> err
  static napi_value Spawn(napi_env env, napi_callback_info info) {
    napi_handle_scope scope;
    napi_open_handle_scope(env, &scope);

    size_t argc = 1;
    napi_value argv[1];
    napi_value thisObj;
    napi_get_cb_info(env, info, &argc, argv, &thisObj, nullptr);

    auto *wrap =
        static_cast<ProcessWrap *>(HandleWrapBase::unwrap(env, thisObj));
    if (!wrap) {
      napi_value result;
      napi_create_int32(env, UV_ESRCH, &result);
      napi_close_handle_scope(env, scope);
      return result;
    }

    napi_value jsOptions = argv[0];
    uv_process_options_t options;
    memset(&options, 0, sizeof(options));
    options.exit_cb = OnExit;

    // options.file
    napi_value fileVal;
    napi_get_named_property(env, jsOptions, "file", &fileVal);
    size_t fileLen = 0;
    napi_get_value_string_utf8(env, fileVal, nullptr, 0, &fileLen);
    std::string file(fileLen, '\0');
    napi_get_value_string_utf8(env, fileVal, &file[0], fileLen + 1, &fileLen);
    options.file = file.c_str();

    // options.args
    napi_value argsVal;
    napi_get_named_property(env, jsOptions, "args", &argsVal);
    std::vector<std::string> argsStrings;
    std::vector<char *> argsPtrs;
    extractStringArray(env, argsVal, argsStrings, argsPtrs);
    if (!argsPtrs.empty())
      options.args = argsPtrs.data();

    // options.cwd
    napi_value cwdVal;
    napi_get_named_property(env, jsOptions, "cwd", &cwdVal);
    napi_valuetype cwdType;
    napi_typeof(env, cwdVal, &cwdType);
    std::string cwd;
    if (cwdType == napi_string) {
      size_t cwdLen = 0;
      napi_get_value_string_utf8(env, cwdVal, nullptr, 0, &cwdLen);
      cwd.resize(cwdLen);
      napi_get_value_string_utf8(env, cwdVal, &cwd[0], cwdLen + 1, &cwdLen);
      if (!cwd.empty())
        options.cwd = cwd.c_str();
    }

    // options.envPairs
    napi_value envPairsVal;
    napi_get_named_property(env, jsOptions, "envPairs", &envPairsVal);
    std::vector<std::string> envStrings;
    std::vector<char *> envPtrs;
    extractStringArray(env, envPairsVal, envStrings, envPtrs);
    if (!envPtrs.empty())
      options.env = envPtrs.data();

    // options.stdio
    std::vector<uv_stdio_container_t> stdioContainers;
    if (!parseStdioOptions(env, jsOptions, stdioContainers)) {
      napi_value result;
      napi_create_int32(env, UV_EINVAL, &result);
      napi_close_handle_scope(env, scope);
      return result;
    }
    options.stdio = stdioContainers.data();
    options.stdio_count = static_cast<int>(stdioContainers.size());

    // options.uid
    napi_value uidVal;
    napi_get_named_property(env, jsOptions, "uid", &uidVal);
    napi_valuetype uidType;
    napi_typeof(env, uidVal, &uidType);
    if (uidType == napi_number) {
      int32_t uid = 0;
      napi_get_value_int32(env, uidVal, &uid);
      options.flags |= UV_PROCESS_SETUID;
      options.uid = static_cast<uv_uid_t>(uid);
    }

    // options.gid
    napi_value gidVal;
    napi_get_named_property(env, jsOptions, "gid", &gidVal);
    napi_valuetype gidType;
    napi_typeof(env, gidVal, &gidType);
    if (gidType == napi_number) {
      int32_t gid = 0;
      napi_get_value_int32(env, gidVal, &gid);
      options.flags |= UV_PROCESS_SETGID;
      options.gid = static_cast<uv_gid_t>(gid);
    }

    // options.detached
    napi_value detachedVal;
    napi_get_named_property(env, jsOptions, "detached", &detachedVal);
    bool detached = false;
    napi_get_value_bool(env, detachedVal, &detached);
    if (detached)
      options.flags |= UV_PROCESS_DETACHED;

    // options.windowsHide
    napi_value windowsHideVal;
    napi_get_named_property(env, jsOptions, "windowsHide", &windowsHideVal);
    bool windowsHide = false;
    napi_get_value_bool(env, windowsHideVal, &windowsHide);
    if (windowsHide)
      options.flags |= UV_PROCESS_WINDOWS_HIDE;

    // options.windowsVerbatimArguments
    napi_value wvaVal;
    napi_get_named_property(
        env, jsOptions, "windowsVerbatimArguments", &wvaVal);
    bool wva = false;
    napi_get_value_bool(env, wvaVal, &wva);
    if (wva)
      options.flags |= UV_PROCESS_WINDOWS_VERBATIM_ARGUMENTS;

    // Spawn the process.
    int err = uv_spawn(getHandleWrapEventLoop(), &wrap->process_, &options);

    // uv_spawn always calls uv__handle_init, even on failure, so the handle
    // is registered with the loop regardless.  We must call init() in both
    // cases so HandleWrapBase will uv_close() it during cleanup (matching
    // Node's MarkAsInitialized() pattern).
    wrap->init(env, thisObj, reinterpret_cast<uv_handle_t *>(&wrap->process_));

    if (err == 0) {
      // Set pid on the JS object.
      napi_value pidVal;
      napi_create_int32(env, wrap->process_.pid, &pidVal);
      napi_set_named_property(env, thisObj, "pid", pidVal);
    }

    napi_value result;
    napi_create_int32(env, err, &result);
    napi_close_handle_scope(env, scope);
    return result;
  }

  /// kill(signal) -> err
  static napi_value Kill(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1];
    napi_value thisObj;
    napi_get_cb_info(env, info, &argc, argv, &thisObj, nullptr);

    auto *wrap =
        static_cast<ProcessWrap *>(HandleWrapBase::unwrap(env, thisObj));
    if (!wrap) {
      napi_value result;
      napi_create_int32(env, UV_ESRCH, &result);
      return result;
    }

    int32_t signal = 0;
    napi_get_value_int32(env, argv[0], &signal);

#ifdef _WIN32
    // On Windows, non-standard signals are remapped to SIGKILL (matches Node).
    if (signal != SIGKILL && signal != SIGTERM && signal != SIGINT &&
        signal != SIGQUIT && signal != 0) {
      signal = SIGKILL;
    }
#endif

    int err = uv_process_kill(&wrap->process_, signal);

    napi_value result;
    napi_create_int32(env, err, &result);
    return result;
  }

  /// libuv exit callback -- fired when the child process exits.
  static void OnExit(uv_process_t *handle, int64_t exitStatus, int termSignal) {
    auto *wrap = static_cast<ProcessWrap *>(handle->data);
    if (!wrap || !wrap->env())
      return;

    napi_env env = wrap->env();
    napi_handle_scope scope;
    napi_open_handle_scope(env, &scope);

    napi_value thisObj = wrap->getJsObject();
    if (!thisObj) {
      napi_close_handle_scope(env, scope);
      return;
    }

    // Get the onexit callback.
    napi_value onexit;
    napi_get_named_property(env, thisObj, "onexit", &onexit);

    napi_valuetype onexitType;
    napi_typeof(env, onexit, &onexitType);
    if (onexitType == napi_function) {
      napi_value args[2];

      // exitStatus as Number (can exceed int32 range).
      napi_create_double(env, static_cast<double>(exitStatus), &args[0]);

      // termSignal as string (e.g. "SIGTERM") or empty string.
      const char *sigName = signoString(termSignal);
      napi_create_string_utf8(env, sigName, NAPI_AUTO_LENGTH, &args[1]);

      napi_value retval;
      napi_call_function(env, thisObj, onexit, 2, args, &retval);

      bool hasPending = false;
      napi_is_exception_pending(env, &hasPending);
      if (hasPending) {
        napi_value exc;
        napi_get_and_clear_last_exception(env, &exc);
      }
    }

    napi_close_handle_scope(env, scope);
  }

  friend napi_value initProcessWrapBinding(napi_env env, napi_value exports);
};

// ---------------------------------------------------------------------------
// initProcessWrapBinding
// ---------------------------------------------------------------------------

napi_value initProcessWrapBinding(napi_env env, napi_value exports) {
  napi_handle_scope scope;
  napi_open_handle_scope(env, &scope);

  // --- Process constructor ---
  napi_property_descriptor protoProps[] = {
      {"spawn",
       nullptr,
       ProcessWrap::Spawn,
       nullptr,
       nullptr,
       nullptr,
       napi_default,
       nullptr},
      {"kill",
       nullptr,
       ProcessWrap::Kill,
       nullptr,
       nullptr,
       nullptr,
       napi_default,
       nullptr},
  };

  napi_value processCtor;
  napi_define_class(
      env,
      "Process",
      NAPI_AUTO_LENGTH,
      ProcessWrap::New,
      nullptr,
      sizeof(protoProps) / sizeof(protoProps[0]),
      protoProps,
      &processCtor);

  // Get the prototype and add handle methods
  // (ref/unref/hasRef/close/getAsyncId).
  napi_value prototype;
  napi_get_named_property(env, processCtor, "prototype", &prototype);
  HandleWrapBase::addHandleWrapMethods(env, prototype);

  // --- Export Process constructor ---
  napi_set_named_property(env, exports, "Process", processCtor);

  napi_close_handle_scope(env, scope);
  return exports;
}

} // namespace node_compat
} // namespace hermes
