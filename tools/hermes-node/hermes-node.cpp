/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "napi/hermes_napi.h"

#include "hermes/Public/RuntimeConfig.h"
#include "hermes/VM/Runtime.h"

#include <hermes/node-compat/binding-registry/binding_registry.h>
#include <hermes/node-compat/bindings/handle_wrap_base.h>
#include <hermes/node-compat/bindings/node_async_context_frame.h>
#include <hermes/node-compat/bindings/node_async_wrap.h>
#include <hermes/node-compat/bindings/node_buffer.h>
#include <hermes/node-compat/bindings/node_cares_wrap.h>
#include <hermes/node-compat/bindings/node_config.h>
#include <hermes/node-compat/bindings/node_constants.h>
#include <hermes/node-compat/bindings/node_credentials.h>
#include <hermes/node-compat/bindings/node_encoding.h>
#include <hermes/node-compat/bindings/node_errors.h>
#include <hermes/node-compat/bindings/node_file.h>
#include <hermes/node-compat/bindings/node_file_dir.h>
#include <hermes/node-compat/bindings/node_fs_event_wrap.h>
#include <hermes/node-compat/bindings/node_http_parser.h>
#include <hermes/node-compat/bindings/node_os.h>
#include <hermes/node-compat/bindings/node_pipe_wrap.h>
#include <hermes/node-compat/bindings/node_stdio.h>
#include <hermes/node-compat/bindings/node_stream_wrap.h>
#include <hermes/node-compat/bindings/node_string_decoder.h>
#include <hermes/node-compat/bindings/node_symbols.h>
#include <hermes/node-compat/bindings/node_task_queue.h>
#include <hermes/node-compat/bindings/node_tcp_wrap.h>
#include <hermes/node-compat/bindings/node_timers.h>
#include <hermes/node-compat/bindings/node_trace_events.h>
#include <hermes/node-compat/bindings/node_tty_wrap.h>
#include <hermes/node-compat/bindings/node_types.h>
#include <hermes/node-compat/bindings/node_udp_wrap.h>
#include <hermes/node-compat/bindings/node_url.h>
#include <hermes/node-compat/bindings/node_util.h>
#include <hermes/node-compat/bindings/node_uv.h>
#include <hermes/node-compat/event-loop/uv_event_loop.h>
#include <hermes/node-compat/module-loader/module_loader.h>
#include <hermes/node-compat/process/node_process.h>

#include <uv.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace hermes::node_compat;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/// Read a file into a string. Returns empty string on failure.
static std::string readFile(const char *path) {
  FILE *f = std::fopen(path, "rb");
  if (!f)
    return "";
  std::fseek(f, 0, SEEK_END);
  long size = std::ftell(f);
  if (size < 0) {
    std::fclose(f);
    return "";
  }
  std::rewind(f);
  std::string content(static_cast<size_t>(size), '\0');
  size_t nread = std::fread(&content[0], 1, static_cast<size_t>(size), f);
  std::fclose(f);
  content.resize(nread);
  return content;
}

/// Print a JS exception to stderr and clear it.
static void printAndClearException(napi_env env) {
  bool pending = false;
  napi_is_exception_pending(env, &pending);
  if (!pending)
    return;

  napi_value exc;
  napi_get_and_clear_last_exception(env, &exc);

  // Try to get the stack trace (Error objects have .stack).
  napi_value stack;
  napi_status st = napi_get_named_property(env, exc, "stack", &stack);
  napi_valuetype stackType = napi_undefined;
  if (st == napi_ok)
    napi_typeof(env, stack, &stackType);

  napi_value msg;
  if (stackType == napi_string) {
    msg = stack;
  } else {
    napi_coerce_to_string(env, exc, &msg);
  }

  char buf[4096];
  size_t len = 0;
  napi_get_value_string_utf8(env, msg, buf, sizeof(buf), &len);
  std::fprintf(stderr, "%.*s\n", static_cast<int>(len), buf);
}

// ---------------------------------------------------------------------------
// Minimal console implementation
// ---------------------------------------------------------------------------

/// Write stringified arguments to a FILE*, separated by spaces, with newline.
static napi_value
consolePrint(napi_env env, napi_callback_info info, FILE *out) {
  size_t argc = 16;
  napi_value argv[16];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  for (size_t i = 0; i < argc; ++i) {
    if (i > 0)
      std::fputc(' ', out);
    napi_value str;
    napi_coerce_to_string(env, argv[i], &str);
    size_t len = 0;
    napi_get_value_string_utf8(env, str, nullptr, 0, &len);
    std::string buf(len, '\0');
    napi_get_value_string_utf8(env, str, &buf[0], len + 1, &len);
    std::fwrite(buf.data(), 1, len, out);
  }
  std::fputc('\n', out);

  napi_value undef;
  napi_get_undefined(env, &undef);
  return undef;
}

static napi_value consoleLog(napi_env env, napi_callback_info info) {
  return consolePrint(env, info, stdout);
}

static napi_value consoleError(napi_env env, napi_callback_info info) {
  return consolePrint(env, info, stderr);
}

/// Install a minimal console object with log/warn/error/info methods.
static napi_status installConsole(napi_env env) {
  napi_value global;
  napi_status st = napi_get_global(env, &global);
  if (st != napi_ok)
    return st;

  napi_value console;
  st = napi_create_object(env, &console);
  if (st != napi_ok)
    return st;

  napi_value logFn, errorFn;
  st = napi_create_function(
      env, "log", NAPI_AUTO_LENGTH, consoleLog, nullptr, &logFn);
  if (st != napi_ok)
    return st;
  st = napi_create_function(
      env, "error", NAPI_AUTO_LENGTH, consoleError, nullptr, &errorFn);
  if (st != napi_ok)
    return st;

  // log and info go to stdout; warn and error go to stderr.
  st = napi_set_named_property(env, console, "log", logFn);
  if (st != napi_ok)
    return st;
  st = napi_set_named_property(env, console, "info", logFn);
  if (st != napi_ok)
    return st;
  st = napi_set_named_property(env, console, "warn", errorFn);
  if (st != napi_ok)
    return st;
  st = napi_set_named_property(env, console, "error", errorFn);
  if (st != napi_ok)
    return st;

  return napi_set_named_property(env, global, "console", console);
}

// ---------------------------------------------------------------------------
// Minimal process.stdout / process.stderr
// ---------------------------------------------------------------------------

/// write(data, callback) — synchronous write to the stream's fd.
/// Handles strings and Uint8Arrays. Calls callback if provided.
static napi_value stdioWrite(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  napi_value thisArg;
  napi_get_cb_info(env, info, &argc, argv, &thisArg, nullptr);

  // Get fd from this.fd.
  napi_value fdVal;
  napi_get_named_property(env, thisArg, "fd", &fdVal);
  int32_t fd;
  napi_get_value_int32(env, fdVal, &fd);

  napi_valuetype dataType;
  napi_typeof(env, argv[0], &dataType);

  int bytesWritten = 0;
  if (dataType == napi_string) {
    // Write string as UTF-8.
    size_t len = 0;
    napi_get_value_string_utf8(env, argv[0], nullptr, 0, &len);
    std::string buf(len, '\0');
    napi_get_value_string_utf8(env, argv[0], &buf[0], len + 1, &len);

    uv_buf_t uvBuf = uv_buf_init(&buf[0], static_cast<unsigned int>(len));
    uv_fs_t req;
    bytesWritten = uv_fs_write(nullptr, &req, fd, &uvBuf, 1, -1, nullptr);
    uv_fs_req_cleanup(&req);
  } else {
    // Try as typed array (Buffer/Uint8Array).
    void *data = nullptr;
    size_t byteLength = 0;
    napi_status st = napi_get_typedarray_info(
        env, argv[0], nullptr, &byteLength, &data, nullptr, nullptr);
    if (st != napi_ok) {
      // Try coercing to string.
      napi_value str;
      napi_coerce_to_string(env, argv[0], &str);
      size_t len = 0;
      napi_get_value_string_utf8(env, str, nullptr, 0, &len);
      std::string buf(len, '\0');
      napi_get_value_string_utf8(env, str, &buf[0], len + 1, &len);

      uv_buf_t uvBuf = uv_buf_init(&buf[0], static_cast<unsigned int>(len));
      uv_fs_t req;
      bytesWritten = uv_fs_write(nullptr, &req, fd, &uvBuf, 1, -1, nullptr);
      uv_fs_req_cleanup(&req);
    } else if (byteLength > 0) {
      uv_buf_t uvBuf = uv_buf_init(
          static_cast<char *>(data), static_cast<unsigned int>(byteLength));
      uv_fs_t req;
      bytesWritten = uv_fs_write(nullptr, &req, fd, &uvBuf, 1, -1, nullptr);
      uv_fs_req_cleanup(&req);
    }
  }

  if (bytesWritten < 0) {
    std::string msg = "write failed: ";
    msg += uv_strerror(bytesWritten);
    napi_throw_error(env, uv_err_name(bytesWritten), msg.c_str());
    return nullptr;
  }

  // Call callback if provided.
  if (argc >= 2) {
    napi_valuetype cbType;
    napi_typeof(env, argv[1], &cbType);
    if (cbType == napi_function) {
      napi_value cbResult;
      napi_call_function(env, thisArg, argv[1], 0, nullptr, &cbResult);
    }
  }

  napi_value trueVal;
  napi_get_boolean(env, true, &trueVal);
  return trueVal;
}

/// No-op stub for event emitter methods.  Returns `this` for chaining.
static napi_value stdioNoop(napi_env env, napi_callback_info info) {
  napi_value thisArg;
  napi_get_cb_info(env, info, nullptr, nullptr, &thisArg, nullptr);
  return thisArg;
}

/// listenerCount() — always returns 0.
static napi_value stdioListenerCount(
    napi_env env,
    napi_callback_info /*info*/) {
  napi_value result;
  napi_create_int32(env, 0, &result);
  return result;
}

/// Create a minimal writable stdio stream object for the given fd.
static napi_status createStdioStream(napi_env env, int fd, napi_value *result) {
  napi_value stream;
  napi_status st = napi_create_object(env, &stream);
  if (st != napi_ok)
    return st;

  // stream.fd
  napi_value fdVal;
  st = napi_create_int32(env, fd, &fdVal);
  if (st != napi_ok)
    return st;
  st = napi_set_named_property(env, stream, "fd", fdVal);
  if (st != napi_ok)
    return st;

  // stream.isTTY
  uv_handle_type handleType = uv_guess_handle(fd);
  napi_value isTTY;
  st = napi_get_boolean(env, handleType == UV_TTY, &isTTY);
  if (st != napi_ok)
    return st;
  st = napi_set_named_property(env, stream, "isTTY", isTTY);
  if (st != napi_ok)
    return st;

  // stream._isStdio = true
  napi_value trueVal;
  st = napi_get_boolean(env, true, &trueVal);
  if (st != napi_ok)
    return st;
  st = napi_set_named_property(env, stream, "_isStdio", trueVal);
  if (st != napi_ok)
    return st;

  // stream.write(data, callback)
  napi_value writeFn;
  st = napi_create_function(
      env, "write", NAPI_AUTO_LENGTH, stdioWrite, nullptr, &writeFn);
  if (st != napi_ok)
    return st;
  st = napi_set_named_property(env, stream, "write", writeFn);
  if (st != napi_ok)
    return st;

  // Minimal event emitter stubs needed by console module.
  napi_value noopFn;
  st = napi_create_function(
      env, "on", NAPI_AUTO_LENGTH, stdioNoop, nullptr, &noopFn);
  if (st != napi_ok)
    return st;
  st = napi_set_named_property(env, stream, "on", noopFn);
  if (st != napi_ok)
    return st;
  st = napi_set_named_property(env, stream, "once", noopFn);
  if (st != napi_ok)
    return st;
  st = napi_set_named_property(env, stream, "removeListener", noopFn);
  if (st != napi_ok)
    return st;

  napi_value listenerCountFn;
  st = napi_create_function(
      env,
      "listenerCount",
      NAPI_AUTO_LENGTH,
      stdioListenerCount,
      nullptr,
      &listenerCountFn);
  if (st != napi_ok)
    return st;
  st = napi_set_named_property(env, stream, "listenerCount", listenerCountFn);
  if (st != napi_ok)
    return st;

  *result = stream;
  return napi_ok;
}

// ---------------------------------------------------------------------------
// Event loop tick integration
// ---------------------------------------------------------------------------

/// Data passed to the uv_check_t callback for draining microtasks and ticks.
struct TickDrainData {
  napi_env env;
  hermes::vm::Runtime *runtime;
  napi_ref tickCallbackRef;
};

/// Called on each event loop iteration (after I/O polling). Drains the
/// microtask queue and then calls the JS tick callback (runNextTicks).
static void onCheckDrainTicks(uv_check_t *handle) {
  auto *data = static_cast<TickDrainData *>(handle->data);

  // Open a handle scope for NAPI calls.
  napi_handle_scope scope;
  napi_open_handle_scope(data->env, &scope);

  // Drain microtasks first.
  data->runtime->drainJobs();

  // Call the JS tick callback (runNextTicks) if set.
  if (data->tickCallbackRef) {
    napi_value tickCb;
    napi_get_reference_value(data->env, data->tickCallbackRef, &tickCb);

    napi_valuetype cbType;
    napi_typeof(data->env, tickCb, &cbType);
    if (cbType == napi_function) {
      napi_value global;
      napi_get_global(data->env, &global);
      napi_value result;
      napi_status st =
          napi_call_function(data->env, global, tickCb, 0, nullptr, &result);
      if (st != napi_ok) {
        // Print and clear any exception from the tick callback.
        printAndClearException(data->env);
      }
    }
  }

  napi_close_handle_scope(data->env, scope);
}

// ---------------------------------------------------------------------------
// Bootstrap
// ---------------------------------------------------------------------------

/// Run the bootstrap sequence and user script. Returns the exit code.
static int runBootstrap(
    int argc,
    char **argv,
    const char *scriptPath,
    const std::string &libJsPath,
    const std::string &libJsNodePath) {
  // 1. Create Hermes runtime with microtask queue enabled.
  auto config = hermes::vm::RuntimeConfig::Builder()
                    .withMicrotaskQueue(true)
                    .withEnableAsyncGenerators(true)
                    .withES6BlockScoping(true)
                    .build();
  auto runtime = hermes::vm::Runtime::create(config);

  // 2. Create libuv event loop adapter.
  UvEventLoop eventLoop;
  int uvErr = eventLoop.init();
  if (uvErr != 0) {
    std::fprintf(
        stderr,
        "Error: failed to initialize event loop: %s\n",
        uv_strerror(uvErr));
    return 1;
  }

  // 3. Create napi_env with event loop.
  napi_env env =
      hermes_napi_create_env(runtime.get(), eventLoop.getEventLoop());

  napi_handle_scope scope;
  napi_open_handle_scope(env, &scope);

  int exitCode = 0;

  // 4. Install console object.
  if (installConsole(env) != napi_ok) {
    std::fprintf(stderr, "Error: failed to install console\n");
    exitCode = 1;
  }

  // 5. Register native bindings.
  // Set host callbacks before binding init.
  setTaskQueueDrainMicrotasks(
      [](void *data) { static_cast<hermes::vm::Runtime *>(data)->drainJobs(); },
      runtime.get());
  setTimersEventLoop(eventLoop.getLoop());
  setFsEventLoop(eventLoop.getLoop());
  setFsDirEventLoop(eventLoop.getLoop());
  setFsEventWrapEventLoop(eventLoop.getLoop());
  setHandleWrapEventLoop(eventLoop.getLoop());
  setCaresWrapEventLoop(eventLoop.getLoop());

  BindingRegistry registry;
  registry.registerBinding("async_context_frame", initAsyncContextFrameBinding);
  registry.registerBinding("async_wrap", initAsyncWrapBinding);
  registry.registerBinding("buffer", initBufferBinding);
  registry.registerBinding("cares_wrap", initCaresWrapBinding);
  registry.registerBinding("config", initConfigBinding);
  registry.registerBinding("constants", initConstantsBinding);
  registry.registerBinding("credentials", initCredentialsBinding);
  registry.registerBinding("encoding_binding", initEncodingBinding);
  registry.registerBinding("errors", initErrorsBinding);
  registry.registerBinding("fs", initFsBinding);
  registry.registerBinding("fs_dir", initFsDirBinding);
  registry.registerBinding("fs_event_wrap", initFsEventWrapBinding);
  registry.registerBinding("http_parser", initHttpParserBinding);
  registry.registerBinding("os", initOsBinding);
  registry.registerBinding("pipe_wrap", initPipeWrapBinding);
  registry.registerBinding("stdio", initStdioBinding);
  registry.registerBinding("stream_wrap", initStreamWrapBinding);
  registry.registerBinding("string_decoder", initStringDecoderBinding);
  registry.registerBinding("symbols", initSymbolsBinding);
  registry.registerBinding("task_queue", initTaskQueueBinding);
  registry.registerBinding("tcp_wrap", initTcpWrapBinding);
  registry.registerBinding("udp_wrap", initUdpWrapBinding);
  registry.registerBinding("timers", initTimersBinding);
  registry.registerBinding("trace_events", initTraceEventsBinding);
  registry.registerBinding("tty_wrap", initTtyWrapBinding);
  registry.registerBinding("types", initTypesBinding);
  registry.registerBinding("url", initUrlBinding);
  registry.registerBinding("url_pattern", initUrlPatternBinding);
  registry.registerBinding("util", initUtilBinding);
  registry.registerBinding("uv", initUvBinding);
  registry.attach(env);

  // 6. Load and execute primordials.js.
  if (exitCode == 0) {
    std::string primordialsPath = libJsPath + "primordials.js";
    std::string source = readFile(primordialsPath.c_str());
    if (source.empty()) {
      std::fprintf(
          stderr,
          "Error: failed to read primordials from '%s'\n",
          primordialsPath.c_str());
      exitCode = 1;
    } else {
      source += "\n//# sourceURL=" + primordialsPath + "\n";

      napi_value scriptStr;
      napi_create_string_utf8(env, source.c_str(), source.size(), &scriptStr);
      napi_value result;
      if (napi_run_script(env, scriptStr, &result) != napi_ok) {
        std::fprintf(stderr, "Error: failed to execute primordials.js\n");
        printAndClearException(env);
        exitCode = 1;
      }
    }
  }

  // Get primordials from global.
  napi_value global;
  napi_get_global(env, &global);
  napi_value primordials;
  napi_get_named_property(env, global, "primordials", &primordials);

  // 7. Create internalBinding function.
  napi_value internalBindingFn;
  registry.createInternalBindingFunction(env, &internalBindingFn);

  // 8. Create and set process global.
  NodeProcess proc;
  {
    std::vector<std::string> nodeArgv;
    nodeArgv.reserve(static_cast<size_t>(argc));
    for (int i = 0; i < argc; ++i)
      nodeArgv.push_back(argv[i]);
    proc.setArgv(std::move(nodeArgv));

    char execBuf[4096];
    size_t execSize = sizeof(execBuf);
    if (uv_exepath(execBuf, &execSize) == 0) {
      proc.setExecPath(std::string(execBuf, execSize));
    } else {
      proc.setExecPath(argv[0]);
    }
  }

  if (exitCode == 0) {
    napi_value processObj;
    if (proc.create(env, &processObj) != napi_ok) {
      std::fprintf(stderr, "Error: failed to create process object\n");
      printAndClearException(env);
      exitCode = 1;
    } else {
      napi_set_named_property(env, global, "process", processObj);

      // Set up process.stdout (fd 1) and process.stderr (fd 2).
      napi_value stdoutStream, stderrStream;
      if (createStdioStream(env, 1, &stdoutStream) == napi_ok) {
        napi_set_named_property(env, processObj, "stdout", stdoutStream);
      }
      if (createStdioStream(env, 2, &stderrStream) == napi_ok) {
        napi_set_named_property(env, processObj, "stderr", stderrStream);
      }

      // Add minimal process event emitter methods (on/emit/emitWarning).
      // Needed for process.on('exit'), process.on('warning'), etc.
      const char *processEventEmitterCode =
          "(function(process) {\n"
          "  var handlers = {};\n"
          "  process.on = function(event, fn) {\n"
          "    if (!handlers[event]) handlers[event] = [];\n"
          "    handlers[event].push(fn);\n"
          "    return process;\n"
          "  };\n"
          "  process.off = process.removeListener = function(event, fn) {\n"
          "    var list = handlers[event];\n"
          "    if (list) {\n"
          "      var idx = list.indexOf(fn);\n"
          "      if (idx >= 0) list.splice(idx, 1);\n"
          "    }\n"
          "    return process;\n"
          "  };\n"
          "  process.once = function(event, fn) {\n"
          "    function wrapper() {\n"
          "      process.off(event, wrapper);\n"
          "      fn.apply(this, arguments);\n"
          "    }\n"
          "    return process.on(event, wrapper);\n"
          "  };\n"
          "  process.emit = function(event) {\n"
          "    var list = handlers[event];\n"
          "    if (!list) return false;\n"
          "    var args = Array.prototype.slice.call(arguments, 1);\n"
          "    var copy = list.slice();\n"
          "    for (var i = 0; i < copy.length; i++) {\n"
          "      copy[i].apply(process, args);\n"
          "    }\n"
          "    return true;\n"
          "  };\n"
          "  process.listeners = function(event) {\n"
          "    return (handlers[event] || []).slice();\n"
          "  };\n"
          "  process.listenerCount = function(event) {\n"
          "    return (handlers[event] || []).length;\n"
          "  };\n"
          "  process.emitWarning = function(warning, type, code) {\n"
          "    if (typeof type === 'object' && type !== null) {\n"
          "      code = type.code; type = type.type || type.name;\n"
          "    }\n"
          "    if (typeof warning === 'string') {\n"
          "      var w = new Error(warning);\n"
          "      w.name = type || 'Warning';\n"
          "      if (code) w.code = code;\n"
          "      warning = w;\n"
          "    }\n"
          "    process.emit('warning', warning);\n"
          "  };\n"
          "});\n"
          "//# sourceURL=hermes-node:process-events\n";
      napi_value processEventEmitterStr;
      napi_create_string_utf8(
          env,
          processEventEmitterCode,
          NAPI_AUTO_LENGTH,
          &processEventEmitterStr);
      napi_value setupFn;
      if (napi_run_script(env, processEventEmitterStr, &setupFn) == napi_ok) {
        napi_value callResult;
        napi_call_function(env, global, setupFn, 1, &processObj, &callResult);
      }
    }
  }

  // 9. Initialize the module loader.
  ModuleLoader loader;
  loader.setLibJsPath(libJsPath);
  loader.setLibJsNodePath(libJsNodePath);

  if (exitCode == 0) {
    if (loader.init(env, primordials, internalBindingFn) != napi_ok) {
      std::fprintf(stderr, "Error: failed to initialize module loader\n");
      printAndClearException(env);
      exitCode = 1;
    }
  }

  // 10. Set up process.nextTick via internal/process/task_queues.
  napi_ref tickCallbackRef = nullptr;
  if (exitCode == 0) {
    napi_value taskQueuesModule;
    if (loader.require(
            env, "internal/process/task_queues", &taskQueuesModule) !=
        napi_ok) {
      std::fprintf(
          stderr, "Error: failed to load internal/process/task_queues\n");
      printAndClearException(env);
      exitCode = 1;
    } else {
      // Call setupTaskQueue() to get nextTick and runNextTicks.
      napi_value setupFn;
      napi_get_named_property(
          env, taskQueuesModule, "setupTaskQueue", &setupFn);

      napi_value setupResult;
      napi_status st = napi_call_function(
          env, taskQueuesModule, setupFn, 0, nullptr, &setupResult);
      if (st != napi_ok) {
        std::fprintf(stderr, "Error: failed to call setupTaskQueue()\n");
        printAndClearException(env);
        exitCode = 1;
      } else {
        // setupResult = { nextTick, runNextTicks }
        napi_value nextTickFn;
        napi_get_named_property(env, setupResult, "nextTick", &nextTickFn);

        napi_value runNextTicksFn;
        napi_get_named_property(
            env, setupResult, "runNextTicks", &runNextTicksFn);

        // Set process.nextTick = nextTick
        napi_value processObj;
        napi_get_named_property(env, global, "process", &processObj);
        napi_set_named_property(env, processObj, "nextTick", nextTickFn);
        napi_set_named_property(
            env, processObj, "_tickCallback", runNextTicksFn);

        // Store runNextTicks as a ref for the event loop check callback.
        napi_create_reference(env, runNextTicksFn, 1, &tickCallbackRef);
      }
    }
  }

  // 11. Set up timers (setTimeout, setInterval, setImmediate).
  if (exitCode == 0 && tickCallbackRef) {
    // Load internal/timers module.
    napi_value internalTimersModule;
    if (loader.require(env, "internal/timers", &internalTimersModule) !=
        napi_ok) {
      std::fprintf(stderr, "Error: failed to load internal/timers\n");
      printAndClearException(env);
      exitCode = 1;
    } else {
      // Get runNextTicks function from the stored ref.
      napi_value runNextTicksFn;
      napi_get_reference_value(env, tickCallbackRef, &runNextTicksFn);

      // Call getTimerCallbacks(runNextTicks) to get processImmediate and
      // processTimers.
      napi_value getTimerCallbacksFn;
      napi_get_named_property(
          env, internalTimersModule, "getTimerCallbacks", &getTimerCallbacksFn);

      napi_value timerCallbacks;
      napi_status st = napi_call_function(
          env,
          internalTimersModule,
          getTimerCallbacksFn,
          1,
          &runNextTicksFn,
          &timerCallbacks);
      if (st != napi_ok) {
        std::fprintf(stderr, "Error: failed to call getTimerCallbacks()\n");
        printAndClearException(env);
        exitCode = 1;
      } else {
        napi_value processImmediateFn;
        napi_get_named_property(
            env, timerCallbacks, "processImmediate", &processImmediateFn);
        napi_value processTimersFn;
        napi_get_named_property(
            env, timerCallbacks, "processTimers", &processTimersFn);

        // Call setupTimers(processImmediate, processTimers) from the timers
        // binding.
        napi_value timersBinding;
        registry.getBinding(env, "timers", &timersBinding);

        napi_value setupTimersFn;
        napi_get_named_property(
            env, timersBinding, "setupTimers", &setupTimersFn);

        napi_value setupArgs[2] = {processImmediateFn, processTimersFn};
        napi_value setupResult;
        st = napi_call_function(
            env, timersBinding, setupTimersFn, 2, setupArgs, &setupResult);
        if (st != napi_ok) {
          std::fprintf(stderr, "Error: failed to call setupTimers()\n");
          printAndClearException(env);
          exitCode = 1;
        }
      }
    }

    // Load public timers module and set globals.
    if (exitCode == 0) {
      napi_value timersModule;
      if (loader.require(env, "timers", &timersModule) != napi_ok) {
        std::fprintf(stderr, "Error: failed to load timers module\n");
        printAndClearException(env);
        exitCode = 1;
      } else {
        // Set timer globals: setTimeout, clearTimeout, setInterval,
        // clearInterval, setImmediate, clearImmediate.
        const char *timerGlobals[] = {
            "setTimeout",
            "clearTimeout",
            "setInterval",
            "clearInterval",
            "setImmediate",
            "clearImmediate",
        };
        for (const char *name : timerGlobals) {
          napi_value fn;
          napi_get_named_property(env, timersModule, name, &fn);
          napi_set_named_property(env, global, name, fn);
        }
      }
    }
  }

  // 11a2. Set globalThis.Buffer.
  if (exitCode == 0) {
    napi_value bufferModule;
    if (loader.require(env, "buffer", &bufferModule) == napi_ok) {
      napi_value bufferCtor;
      napi_get_named_property(env, bufferModule, "Buffer", &bufferCtor);
      napi_set_named_property(env, global, "Buffer", bufferCtor);
    }
  }

  // 11a3. Set globalThis.URL and globalThis.URLSearchParams.
  // Loading internal/url also sets these globals, but we do it explicitly
  // during bootstrap so they are available before any user code runs.
  if (exitCode == 0) {
    napi_value urlModule;
    if (loader.require(env, "internal/url", &urlModule) == napi_ok) {
      napi_value urlCtor;
      napi_get_named_property(env, urlModule, "URL", &urlCtor);
      napi_set_named_property(env, global, "URL", urlCtor);

      napi_value urlSPCtor;
      napi_get_named_property(env, urlModule, "URLSearchParams", &urlSPCtor);
      napi_set_named_property(env, global, "URLSearchParams", urlSPCtor);
    }
  }

  // 11b. Initialize debuglog (must happen before any debug() call).
  if (exitCode == 0) {
    napi_value debuglogModule;
    if (loader.require(env, "internal/util/debuglog", &debuglogModule) ==
        napi_ok) {
      napi_value initDebugEnvFn;
      napi_get_named_property(
          env, debuglogModule, "initializeDebugEnv", &initDebugEnvFn);

      // Get process.env.NODE_DEBUG value.
      napi_value processObj;
      napi_get_named_property(env, global, "process", &processObj);
      napi_value envObj;
      napi_get_named_property(env, processObj, "env", &envObj);
      napi_value nodeDebugVal;
      napi_get_named_property(env, envObj, "NODE_DEBUG", &nodeDebugVal);

      napi_value initResult;
      napi_call_function(
          env, debuglogModule, initDebugEnvFn, 1, &nodeDebugVal, &initResult);
      bool pending = false;
      napi_is_exception_pending(env, &pending);
      if (pending)
        printAndClearException(env);
    }
  }

  // 11c. Load and install the real console module.
  // This replaces the minimal C++ console installed in step 4 with Node's
  // full implementation (util.inspect formatting, console.time/table/dir,
  // etc.).  Node's global.js creates the console object and binds method
  // properties, but the stdout/stderr streams are bound separately via
  // kBindStreamsLazy (called from pre_execution.js in Node).
  if (exitCode == 0) {
    const char *consoleInitCode =
        "(function() {"
        "  var c = require('console');"
        "  var ctor = require('internal/console/constructor');"
        "  c[ctor.kBindStreamsLazy](process);"
        "  globalThis.console = c;"
        "})()";
    napi_value initScript, initResult;
    napi_create_string_utf8(
        env, consoleInitCode, NAPI_AUTO_LENGTH, &initScript);
    if (napi_run_script(env, initScript, &initResult) != napi_ok) {
      // Non-fatal: fall back to the minimal C++ console from step 4.
      printAndClearException(env);
    }
  }

  // 12. Set up the event loop check handle for tick draining.
  uv_check_t checkHandle;
  TickDrainData tickDrainData{env, runtime.get(), tickCallbackRef};
  bool checkHandleActive = false;

  if (exitCode == 0 && tickCallbackRef) {
    uv_check_init(eventLoop.getLoop(), &checkHandle);
    checkHandle.data = &tickDrainData;
    uv_check_start(&checkHandle, onCheckDrainTicks);
    // Unref so the check handle alone doesn't keep the loop alive.
    uv_unref(reinterpret_cast<uv_handle_t *>(&checkHandle));
    checkHandleActive = true;
  }

  // 12. Load and execute the user script.
  // Use the module loader's __loadUserScript() so the script gets a
  // path-aware require() that supports relative imports (e.g.
  // require('../foo')).
  if (exitCode == 0) {
    napi_value loadUserScriptFn;
    napi_get_named_property(env, global, "__loadUserScript", &loadUserScriptFn);

    napi_value scriptPathStr;
    napi_create_string_utf8(env, scriptPath, NAPI_AUTO_LENGTH, &scriptPathStr);

    napi_value result;
    if (napi_call_function(
            env, global, loadUserScriptFn, 1, &scriptPathStr, &result) !=
        napi_ok) {
      printAndClearException(env);
      exitCode = 1;
    }
  }

  // 13. Run event loop.
  if (exitCode == 0) {
    // Drain microtasks queued during script execution.
    runtime->drainJobs();

    // Drain any nextTick callbacks queued during script execution.
    if (tickCallbackRef) {
      napi_value tickCb;
      napi_get_reference_value(env, tickCallbackRef, &tickCb);
      napi_value tickResult;
      napi_call_function(env, global, tickCb, 0, nullptr, &tickResult);

      bool pending = false;
      napi_is_exception_pending(env, &pending);
      if (pending)
        printAndClearException(env);
    }

    eventLoop.run();
  }

  // 14. Emit 'exit' event on process object.
  {
    napi_value processObj;
    napi_get_named_property(env, global, "process", &processObj);
    napi_value emitFn;
    napi_get_named_property(env, processObj, "emit", &emitFn);
    napi_valuetype emitType;
    napi_typeof(env, emitFn, &emitType);
    if (emitType == napi_function) {
      napi_value exitStr, exitCodeVal;
      napi_create_string_utf8(env, "exit", NAPI_AUTO_LENGTH, &exitStr);
      napi_create_int32(env, exitCode, &exitCodeVal);
      napi_value emitArgs[2] = {exitStr, exitCodeVal};
      napi_value emitResult;
      napi_call_function(env, processObj, emitFn, 2, emitArgs, &emitResult);
      bool pending = false;
      napi_is_exception_pending(env, &pending);
      if (pending)
        printAndClearException(env);
    }
  }

  // 15. Cleanup (reverse order of creation).
  // Close timer and fs_event libuv handles first.
  closeTimersHandles();
  closeFsEventWrapHandles();

  if (checkHandleActive) {
    uv_check_stop(&checkHandle);
    uv_close(reinterpret_cast<uv_handle_t *>(&checkHandle), nullptr);
  }

  // Run the loop once more to process close callbacks.
  uv_run(eventLoop.getLoop(), UV_RUN_NOWAIT);

  // Mark the event loop as no longer alive for cares_wrap GC finalizers.
  caresWrapShutdown();

  // Close the event loop. This runs uv_run(UV_RUN_DEFAULT) which may
  // complete pending async fs operations, so NAPI env must still be valid.
  eventLoop.close();

  if (tickCallbackRef) {
    napi_delete_reference(env, tickCallbackRef);
  }

  loader.detach(env);
  proc.detach(env);
  registry.detach(env);

  napi_close_handle_scope(env, scope);
  hermes_napi_destroy_env(env);
  runtime.reset();

  return exitCode;
}

// ---------------------------------------------------------------------------
// Argument parsing
// ---------------------------------------------------------------------------

static void printUsage(const char *argv0) {
  std::fprintf(
      stderr,
      "Usage: %s [options] <script.js>\n"
      "\nOptions:\n"
      "  --node-lib-path <dir>  Path to the project root containing libjs/"
      " and libjs-node/\n",
      argv0);
}

int main(int argc, char **argv) {
  const char *scriptPath = nullptr;
  const char *nodeLibPath = nullptr;

  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--node-lib-path") == 0) {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "Error: --node-lib-path requires an argument\n");
        return 1;
      }
      nodeLibPath = argv[++i];
    } else if (
        std::strcmp(argv[i], "--help") == 0 ||
        std::strcmp(argv[i], "-h") == 0) {
      printUsage(argv[0]);
      return 0;
    } else if (argv[i][0] == '-') {
      std::fprintf(stderr, "Error: unknown option '%s'\n", argv[i]);
      return 1;
    } else {
      scriptPath = argv[i];
      break;
    }
  }

  if (!scriptPath) {
    printUsage(argv[0]);
    return 1;
  }

  // Resolve paths to libjs/ and libjs-node/.
  std::string libJsPath;
  std::string libJsNodePath;

  if (nodeLibPath) {
    // --node-lib-path points to the project root.
    libJsPath = std::string(nodeLibPath) + "/libjs/";
    libJsNodePath = std::string(nodeLibPath) + "/libjs-node/";
  } else {
    // Default: look relative to the executable (bin/../libjs/).
    char execBuf[4096];
    size_t execSize = sizeof(execBuf);
    std::string execDir;
    if (uv_exepath(execBuf, &execSize) == 0) {
      std::string execStr(execBuf, execSize);
      auto slash = execStr.rfind('/');
      if (slash != std::string::npos)
        execDir = execStr.substr(0, slash);
    }
    if (execDir.empty())
      execDir = ".";

    // Go up from bin/ to the project root.
    auto slash = execDir.rfind('/');
    std::string rootDir;
    if (slash != std::string::npos)
      rootDir = execDir.substr(0, slash);
    else
      rootDir = ".";

    libJsPath = rootDir + "/libjs/";
    libJsNodePath = rootDir + "/libjs-node/";
  }

  return runBootstrap(argc, argv, scriptPath, libJsPath, libJsNodePath);
}
