/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <hermes/node-compat/bindings/node_spawn_sync.h>
#include <node_api.h>
#include <uv.h>

#include <cassert>
#include <csignal>
#include <cstring>
#include <memory>
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
// OutputBuffer -- linked list of output capture buffers
// ---------------------------------------------------------------------------

static constexpr size_t kOutputBufferSize = 65536;

struct OutputBuffer {
  char data[kOutputBufferSize];
  unsigned used = 0;
  OutputBuffer *next = nullptr;

  unsigned available() const {
    return kOutputBufferSize - used;
  }

  void onAlloc(uv_buf_t *buf) const {
    if (used == kOutputBufferSize)
      *buf = uv_buf_init(nullptr, 0);
    else
      *buf = uv_buf_init(
          const_cast<char *>(data) + used, kOutputBufferSize - used);
  }

  void onRead(const uv_buf_t * /*buf*/, size_t nread) {
    used += static_cast<unsigned>(nread);
  }
};

// ---------------------------------------------------------------------------
// SyncProcessRunner -- runs a child process synchronously
// ---------------------------------------------------------------------------

class SyncProcessRunner;

/// StdioPipe -- manages a single stdio pipe on the temporary loop.
class StdioPipe {
 public:
  StdioPipe(
      SyncProcessRunner *runner,
      bool readable,
      bool writable,
      uv_buf_t inputBuf)
      : runner_(runner),
        readable_(readable),
        writable_(writable),
        inputBuf_(inputBuf) {}

  ~StdioPipe() {
    OutputBuffer *buf = firstOutput_;
    while (buf) {
      OutputBuffer *next = buf->next;
      delete buf;
      buf = next;
    }
  }

  int initialize(uv_loop_t *loop) {
    int r = uv_pipe_init(loop, &pipe_, 0);
    if (r < 0)
      return r;
    pipe_.data = this;
    initialized_ = true;
    return 0;
  }

  int start() {
    // readable_ means the child reads from this pipe (we write to it).
    if (readable_) {
      if (inputBuf_.len > 0) {
        int r = uv_write(&writeReq_, uvStream(), &inputBuf_, 1, writeCb);
        if (r < 0)
          return r;
      }
      int r = uv_shutdown(&shutdownReq_, uvStream(), shutdownCb);
      if (r < 0)
        return r;
    }

    // writable_ means the child writes to this pipe (we read from it).
    if (writable_) {
      int r = uv_read_start(uvStream(), allocCb, readCb);
      if (r < 0)
        return r;
    }

    return 0;
  }

  void close() {
    if (initialized_) {
      uv_close(uvHandle(), closeCb);
      initialized_ = false;
    }
  }

  bool readable() const {
    return readable_;
  }
  bool writable() const {
    return writable_;
  }

  uv_stdio_flags uvFlags() const {
    unsigned flags = UV_CREATE_PIPE;
    if (readable_)
      flags |= UV_READABLE_PIPE;
    if (writable_)
      flags |= UV_WRITABLE_PIPE;
    return static_cast<uv_stdio_flags>(flags);
  }

  uv_pipe_t *uvPipe() {
    return &pipe_;
  }
  uv_stream_t *uvStream() {
    return reinterpret_cast<uv_stream_t *>(&pipe_);
  }
  uv_handle_t *uvHandle() {
    return reinterpret_cast<uv_handle_t *>(&pipe_);
  }

  /// Total output captured.
  size_t outputLength() const {
    size_t total = 0;
    for (auto *buf = firstOutput_; buf; buf = buf->next)
      total += buf->used;
    return total;
  }

  /// Copy output to dest.
  void copyOutput(char *dest) const {
    size_t offset = 0;
    for (auto *buf = firstOutput_; buf; buf = buf->next) {
      if (buf->used > 0) {
        memcpy(dest + offset, buf->data, buf->used);
        offset += buf->used;
      }
    }
  }

 private:
  void onAlloc(uv_buf_t *buf) {
    if (!lastOutput_) {
      firstOutput_ = lastOutput_ = new OutputBuffer();
    } else if (lastOutput_->available() == 0) {
      auto *newBuf = new OutputBuffer();
      lastOutput_->next = newBuf;
      lastOutput_ = newBuf;
    }
    lastOutput_->onAlloc(buf);
  }

  void onRead(const uv_buf_t *buf, ssize_t nread);

  void onWriteDone(int result);
  void onShutdownDone(int result);

  static void allocCb(uv_handle_t *handle, size_t, uv_buf_t *buf) {
    static_cast<StdioPipe *>(handle->data)->onAlloc(buf);
  }

  static void readCb(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf) {
    static_cast<StdioPipe *>(stream->data)->onRead(buf, nread);
  }

  static void writeCb(uv_write_t *req, int status) {
    static_cast<StdioPipe *>(req->handle->data)->onWriteDone(status);
  }

  static void shutdownCb(uv_shutdown_t *req, int status) {
    // On macOS/BSDs, shutdown on a pipe whose other end has closed returns
    // ENOTCONN. This is benign -- treat it as success.
    if (status == UV_ENOTCONN)
      status = 0;
    static_cast<StdioPipe *>(req->handle->data)->onShutdownDone(status);
  }

  static void closeCb(uv_handle_t *) {
    // No-op. We track lifecycle via initialized_ flag.
  }

  SyncProcessRunner *runner_;
  bool readable_;
  bool writable_;
  uv_buf_t inputBuf_;
  bool initialized_ = false;

  uv_pipe_t pipe_{};
  uv_write_t writeReq_{};
  uv_shutdown_t shutdownReq_{};

  OutputBuffer *firstOutput_ = nullptr;
  OutputBuffer *lastOutput_ = nullptr;
};

// ---------------------------------------------------------------------------
// SyncProcessRunner
// ---------------------------------------------------------------------------

class SyncProcessRunner {
 public:
  SyncProcessRunner() = default;
  ~SyncProcessRunner() = default;

  /// Parse options, spawn, run loop, build result. Returns nullptr on
  /// exception.
  napi_value run(napi_env env, napi_value jsOptions) {
    env_ = env;

    // --- Parse options ---
    if (!parseOptions(jsOptions))
      return nullptr;

    // --- Create temporary loop ---
    uv_loop_t loop;
    int r = uv_loop_init(&loop);
    if (r < 0) {
      setError(r);
      return buildResult();
    }
    loop_ = &loop;

    // --- Initialize stdio pipes on the temp loop ---
    for (auto &pipe : stdioPipes_) {
      if (pipe) {
        r = pipe->initialize(loop_);
        if (r < 0) {
          setPipeError(r);
          break;
        }
      }
    }

    // --- Set up timeout timer ---
    uv_timer_t timer;
    bool timerInitialized = false;
    if (error_ == 0 && timeout_ > 0) {
      r = uv_timer_init(loop_, &timer);
      if (r < 0) {
        setError(r);
      } else {
        uv_unref(reinterpret_cast<uv_handle_t *>(&timer));
        timer.data = this;
        timerInitialized = true;
        r = uv_timer_start(&timer, killTimerCb, timeout_, 0);
        if (r < 0)
          setError(r);
      }
    }

    // --- Spawn ---
    bool spawned = false;
    if (error_ == 0 && pipeError_ == 0) {
      uvOptions_.exit_cb = exitCb;
      r = uv_spawn(loop_, &process_, &uvOptions_);
      if (r < 0) {
        setError(r);
      } else {
        process_.data = this;
        spawned = true;

        // Start pipes (write input, read output).
        for (auto &pipe : stdioPipes_) {
          if (pipe) {
            r = pipe->start();
            if (r < 0) {
              setPipeError(r);
              break;
            }
          }
        }
      }
    }

    // --- Run the loop ---
    if (spawned) {
      r = uv_run(loop_, UV_RUN_DEFAULT);
      (void)r; // If loop fails, we still clean up.
    }

    // --- Close all handles ---
    // Close stdio pipes.
    for (auto &pipe : stdioPipes_) {
      if (pipe)
        pipe->close();
    }

    // Close kill timer.
    if (timerInitialized) {
      uv_ref(reinterpret_cast<uv_handle_t *>(&timer));
      uv_close(reinterpret_cast<uv_handle_t *>(&timer), nullptr);
    }

    // Close process handle if still open.
    auto *processHandle = reinterpret_cast<uv_handle_t *>(&process_);
    if (processHandle->type == UV_PROCESS && !uv_is_closing(processHandle))
      uv_close(processHandle, nullptr);

    // Run loop again to let close callbacks fire.
    uv_run(loop_, UV_RUN_DEFAULT);

    // Destroy the loop.
    uv_loop_close(loop_);
    loop_ = nullptr;

    return buildResult();
  }

  void setError(int error) {
    if (error_ == 0)
      error_ = error;
  }

  void setPipeError(int error) {
    if (pipeError_ == 0)
      pipeError_ = error;
  }

  void incrementBufferSizeAndCheckOverflow(ssize_t length) {
    bufferedOutputSize_ += length;
    if (maxBuffer_ > 0 &&
        static_cast<double>(bufferedOutputSize_) > maxBuffer_) {
      setError(UV_ENOBUFS);
      kill();
    }
  }

 private:
  int getError() const {
    return error_ != 0 ? error_ : pipeError_;
  }

  void onExit(int64_t exitStatus, int termSignal) {
    if (exitStatus < 0) {
      setError(static_cast<int>(exitStatus));
      return;
    }
    exitStatus_ = exitStatus;
    termSignal_ = termSignal;
  }

  void kill() {
    if (killed_)
      return;
    killed_ = true;

    if (exitStatus_ < 0) {
      int r = uv_process_kill(&process_, killSignal_);
      if (r < 0 && r != UV_ESRCH) {
        setError(r);
        uv_process_kill(&process_, SIGKILL);
      }
    }

    // Close all stdio pipes.
    for (auto &pipe : stdioPipes_) {
      if (pipe)
        pipe->close();
    }
  }

  // --- Option parsing ---

  static void extractString(napi_env env, napi_value val, std::string &out) {
    size_t len = 0;
    napi_get_value_string_utf8(env, val, nullptr, 0, &len);
    out.resize(len);
    napi_get_value_string_utf8(env, val, &out[0], len + 1, &len);
  }

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

      size_t strLen = 0;
      napi_get_value_string_utf8(env, elem, nullptr, 0, &strLen);

      std::string s(strLen, '\0');
      napi_get_value_string_utf8(env, elem, &s[0], strLen + 1, &strLen);
      strings.push_back(std::move(s));
    }

    ptrs.resize(strings.size() + 1);
    for (size_t i = 0; i < strings.size(); i++)
      ptrs[i] = const_cast<char *>(strings[i].c_str());
    ptrs.back() = nullptr;
  }

  bool parseOptions(napi_value jsOptions) {
    memset(&uvOptions_, 0, sizeof(uvOptions_));

    // options.file
    napi_value fileVal;
    napi_get_named_property(env_, jsOptions, "file", &fileVal);
    extractString(env_, fileVal, file_);
    uvOptions_.file = file_.c_str();

    // options.args
    napi_value argsVal;
    napi_get_named_property(env_, jsOptions, "args", &argsVal);
    extractStringArray(env_, argsVal, argsStrings_, argsPtrs_);
    if (!argsPtrs_.empty())
      uvOptions_.args = argsPtrs_.data();

    // options.cwd
    napi_value cwdVal;
    napi_get_named_property(env_, jsOptions, "cwd", &cwdVal);
    napi_valuetype cwdType;
    napi_typeof(env_, cwdVal, &cwdType);
    if (cwdType == napi_string) {
      extractString(env_, cwdVal, cwd_);
      uvOptions_.cwd = cwd_.c_str();
    }

    // options.envPairs
    napi_value envPairsVal;
    napi_get_named_property(env_, jsOptions, "envPairs", &envPairsVal);
    extractStringArray(env_, envPairsVal, envStrings_, envPtrs_);
    if (!envPtrs_.empty())
      uvOptions_.env = envPtrs_.data();

    // options.uid
    napi_value uidVal;
    napi_get_named_property(env_, jsOptions, "uid", &uidVal);
    napi_valuetype uidType;
    napi_typeof(env_, uidVal, &uidType);
    if (uidType == napi_number) {
      int32_t uid = 0;
      napi_get_value_int32(env_, uidVal, &uid);
      uvOptions_.flags |= UV_PROCESS_SETUID;
      uvOptions_.uid = static_cast<uv_uid_t>(uid);
    }

    // options.gid
    napi_value gidVal;
    napi_get_named_property(env_, jsOptions, "gid", &gidVal);
    napi_valuetype gidType;
    napi_typeof(env_, gidVal, &gidType);
    if (gidType == napi_number) {
      int32_t gid = 0;
      napi_get_value_int32(env_, gidVal, &gid);
      uvOptions_.flags |= UV_PROCESS_SETGID;
      uvOptions_.gid = static_cast<uv_gid_t>(gid);
    }

    // options.detached
    napi_value detachedVal;
    napi_get_named_property(env_, jsOptions, "detached", &detachedVal);
    bool detached = false;
    napi_get_value_bool(env_, detachedVal, &detached);
    if (detached)
      uvOptions_.flags |= UV_PROCESS_DETACHED;

    // options.windowsHide
    napi_value whVal;
    napi_get_named_property(env_, jsOptions, "windowsHide", &whVal);
    bool windowsHide = false;
    napi_get_value_bool(env_, whVal, &windowsHide);
    if (windowsHide)
      uvOptions_.flags |= UV_PROCESS_WINDOWS_HIDE;

    // options.windowsVerbatimArguments
    napi_value wvaVal;
    napi_get_named_property(
        env_, jsOptions, "windowsVerbatimArguments", &wvaVal);
    bool wva = false;
    napi_get_value_bool(env_, wvaVal, &wva);
    if (wva)
      uvOptions_.flags |= UV_PROCESS_WINDOWS_VERBATIM_ARGUMENTS;

    // options.timeout
    napi_value timeoutVal;
    napi_get_named_property(env_, jsOptions, "timeout", &timeoutVal);
    napi_valuetype timeoutType;
    napi_typeof(env_, timeoutVal, &timeoutType);
    if (timeoutType == napi_number) {
      int64_t timeout = 0;
      napi_get_value_int64(env_, timeoutVal, &timeout);
      if (timeout > 0)
        timeout_ = static_cast<uint64_t>(timeout);
    }

    // options.maxBuffer
    napi_value maxBufVal;
    napi_get_named_property(env_, jsOptions, "maxBuffer", &maxBufVal);
    napi_valuetype maxBufType;
    napi_typeof(env_, maxBufVal, &maxBufType);
    if (maxBufType == napi_number) {
      napi_get_value_double(env_, maxBufVal, &maxBuffer_);
    }

    // options.killSignal
    napi_value killSigVal;
    napi_get_named_property(env_, jsOptions, "killSignal", &killSigVal);
    napi_valuetype killSigType;
    napi_typeof(env_, killSigVal, &killSigType);
    if (killSigType == napi_number) {
      int32_t sig = 0;
      napi_get_value_int32(env_, killSigVal, &sig);
      killSignal_ = sig;
    }

    // options.stdio
    return parseStdioOptions(jsOptions);
  }

  bool parseStdioOptions(napi_value jsOptions) {
    napi_value stdioVal;
    napi_get_named_property(env_, jsOptions, "stdio", &stdioVal);

    bool isArray = false;
    napi_is_array(env_, stdioVal, &isArray);
    if (!isArray) {
      setError(UV_EINVAL);
      return true; // Not a JS exception, just an error code.
    }

    uint32_t len = 0;
    napi_get_array_length(env_, stdioVal, &len);

    stdioContainers_.resize(len);
    stdioPipes_.resize(len);

    for (uint32_t i = 0; i < len; i++) {
      napi_value elem;
      napi_get_element(env_, stdioVal, i, &elem);

      napi_value typeVal;
      napi_get_named_property(env_, elem, "type", &typeVal);

      char typeBuf[32];
      size_t typeLen = 0;
      napi_get_value_string_utf8(
          env_, typeVal, typeBuf, sizeof(typeBuf), &typeLen);

      if (strcmp(typeBuf, "ignore") == 0) {
        stdioContainers_[i].flags = UV_IGNORE;

      } else if (
          strcmp(typeBuf, "pipe") == 0 || strcmp(typeBuf, "overlapped") == 0) {
        // For sync spawn, pipes are created internally.
        napi_value readableVal, writableVal;
        napi_get_named_property(env_, elem, "readable", &readableVal);
        napi_get_named_property(env_, elem, "writable", &writableVal);
        bool readable = false, writable = false;
        napi_get_value_bool(env_, readableVal, &readable);
        napi_get_value_bool(env_, writableVal, &writable);

        uv_buf_t inputBuf = uv_buf_init(nullptr, 0);

        if (readable) {
          // Check for input data to write to the child's stdin.
          napi_value inputVal;
          napi_get_named_property(env_, elem, "input", &inputVal);

          napi_valuetype inputType;
          napi_typeof(env_, inputVal, &inputType);

          if (inputType == napi_object) {
            // Could be a Buffer/TypedArray.
            bool isTypedArray = false;
            napi_is_typedarray(env_, inputVal, &isTypedArray);
            if (isTypedArray) {
              void *data = nullptr;
              size_t length = 0;
              napi_get_typedarray_info(
                  env_, inputVal, nullptr, &length, &data, nullptr, nullptr);
              inputBuf = uv_buf_init(
                  static_cast<char *>(data), static_cast<unsigned>(length));
            }
          }
        }

        auto pipe =
            std::make_unique<StdioPipe>(this, readable, writable, inputBuf);
        stdioContainers_[i].flags = pipe->uvFlags();
        stdioContainers_[i].data.stream = pipe->uvStream();
        stdioPipes_[i] = std::move(pipe);

      } else {
        // fd-based: "inherit", "fd", or anything else with an fd field.
        napi_value fdVal;
        napi_get_named_property(env_, elem, "fd", &fdVal);
        int32_t fd = 0;
        napi_get_value_int32(env_, fdVal, &fd);
        stdioContainers_[i].flags = UV_INHERIT_FD;
        stdioContainers_[i].data.fd = fd;
      }
    }

    uvOptions_.stdio = stdioContainers_.data();
    uvOptions_.stdio_count = static_cast<int>(stdioContainers_.size());
    return true;
  }

  // --- Build the result object ---

  napi_value buildResult() {
    napi_value result;
    napi_create_object(env_, &result);

    int error = getError();

    // result.error (numeric error code, JS side converts to Error)
    if (error != 0) {
      napi_value errorVal;
      napi_create_int32(env_, error, &errorVal);
      napi_set_named_property(env_, result, "error", errorVal);
    }

    // result.status
    napi_value statusVal;
    if (exitStatus_ >= 0 && termSignal_ <= 0) {
      napi_create_double(env_, static_cast<double>(exitStatus_), &statusVal);
    } else {
      napi_get_null(env_, &statusVal);
    }
    napi_set_named_property(env_, result, "status", statusVal);

    // result.signal
    napi_value signalVal;
    if (termSignal_ > 0) {
      const char *sigName = signoString(termSignal_);
      napi_create_string_utf8(env_, sigName, NAPI_AUTO_LENGTH, &signalVal);
    } else {
      napi_get_null(env_, &signalVal);
    }
    napi_set_named_property(env_, result, "signal", signalVal);

    // result.output -- array of [null, stdout_buffer, stderr_buffer, ...]
    if (exitStatus_ >= 0) {
      napi_value outputArr;
      napi_create_array_with_length(env_, stdioPipes_.size(), &outputArr);
      for (uint32_t i = 0; i < stdioPipes_.size(); i++) {
        napi_value item;
        auto &pipe = stdioPipes_[i];
        if (pipe && pipe->writable()) {
          size_t len = pipe->outputLength();
          // Collect output into a temp buffer, then create a Buffer.
          // Always pass a valid (non-null) pointer to napi_create_buffer_copy
          // even for empty output, because Hermes NAPI may misbehave with
          // nullptr data.
          std::vector<char> tmp(len > 0 ? len : 1);
          if (len > 0)
            pipe->copyOutput(tmp.data());
          void *bufCopy = nullptr;
          napi_create_buffer_copy(env_, len, tmp.data(), &bufCopy, &item);
        } else {
          napi_get_null(env_, &item);
        }
        napi_set_element(env_, outputArr, i, item);
      }
      napi_set_named_property(env_, result, "output", outputArr);
    } else {
      napi_value nullVal;
      napi_get_null(env_, &nullVal);
      napi_set_named_property(env_, result, "output", nullVal);
    }

    // result.pid
    napi_value pidVal;
    napi_create_int32(env_, process_.pid, &pidVal);
    napi_set_named_property(env_, result, "pid", pidVal);

    return result;
  }

  // --- libuv callbacks ---

  static void exitCb(uv_process_t *handle, int64_t exitStatus, int termSignal) {
    auto *self = static_cast<SyncProcessRunner *>(handle->data);
    uv_close(reinterpret_cast<uv_handle_t *>(handle), nullptr);
    self->onExit(exitStatus, termSignal);
  }

  static void killTimerCb(uv_timer_t *handle) {
    auto *self = static_cast<SyncProcessRunner *>(handle->data);
    self->setError(UV_ETIMEDOUT);
    self->kill();
  }

  // --- State ---
  napi_env env_ = nullptr;
  uv_loop_t *loop_ = nullptr;
  uv_process_t process_{};
  uv_process_options_t uvOptions_{};

  // Parsed options storage (must outlive uv_spawn).
  std::string file_;
  std::vector<std::string> argsStrings_;
  std::vector<char *> argsPtrs_;
  std::string cwd_;
  std::vector<std::string> envStrings_;
  std::vector<char *> envPtrs_;

  std::vector<uv_stdio_container_t> stdioContainers_;
  std::vector<std::unique_ptr<StdioPipe>> stdioPipes_;

  uint64_t timeout_ = 0;
  double maxBuffer_ = 0;
  int killSignal_ = SIGTERM;

  int64_t exitStatus_ = -1;
  int termSignal_ = -1;
  int error_ = 0;
  int pipeError_ = 0;
  bool killed_ = false;
  ssize_t bufferedOutputSize_ = 0;

  friend class StdioPipe;
};

// --- StdioPipe methods that reference SyncProcessRunner ---

void StdioPipe::onRead(const uv_buf_t *buf, ssize_t nread) {
  if (nread == UV_EOF) {
    // Libuv stops reading on EOF.
  } else if (nread < 0) {
    runner_->setPipeError(static_cast<int>(nread));
    uv_read_stop(uvStream());
  } else {
    lastOutput_->onRead(buf, static_cast<size_t>(nread));
    runner_->incrementBufferSizeAndCheckOverflow(nread);
  }
}

void StdioPipe::onWriteDone(int result) {
  if (result < 0)
    runner_->setPipeError(result);
}

void StdioPipe::onShutdownDone(int result) {
  if (result < 0)
    runner_->setPipeError(result);
}

// ---------------------------------------------------------------------------
// NAPI entry point
// ---------------------------------------------------------------------------

static napi_value spawnSync(napi_env env, napi_callback_info info) {
  napi_handle_scope scope;
  napi_open_handle_scope(env, &scope);

  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  SyncProcessRunner runner;
  napi_value result = runner.run(env, argv[0]);

  napi_close_handle_scope(env, scope);
  return result;
}

napi_value initSpawnSyncBinding(napi_env env, napi_value exports) {
  napi_value spawnFn;
  napi_create_function(
      env, "spawn", NAPI_AUTO_LENGTH, spawnSync, nullptr, &spawnFn);
  napi_set_named_property(env, exports, "spawn", spawnFn);
  return exports;
}

} // namespace node_compat
} // namespace hermes
