/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <hermes/node-compat/runtime/hermes_node_runtime.h>

#include "napi/hermes_napi.h"

#include "hermes/Public/RuntimeConfig.h"
#include "hermes/VM/Runtime.h"
#include "hermes/hermes.h"

#ifdef HERMES_ENABLE_DEBUGGER
#include <hermes/RuntimeTaskRunner.h>
#include <hermes/cdp/CDPAgent.h>
#include <hermes/cdp/CDPDebugAPI.h>

#include <atomic>
#include <mutex>
#include <queue>
#include <thread>
#endif

#include <hermes/node-compat/binding-registry/binding_registry.h>
#include <hermes/node-compat/bindings/handle_wrap_base.h>
#include <hermes/node-compat/bindings/node_async_context_frame.h>
#include <hermes/node-compat/bindings/node_async_wrap.h>
#include <hermes/node-compat/bindings/node_buffer.h>
#include <hermes/node-compat/bindings/node_cares_wrap.h>
#include <hermes/node-compat/bindings/node_config.h>
#include <hermes/node-compat/bindings/node_constants.h>
#include <hermes/node-compat/bindings/node_contextify.h>
#include <hermes/node-compat/bindings/node_credentials.h>
#include <hermes/node-compat/bindings/node_crypto.h>
#include <hermes/node-compat/bindings/node_encoding.h>
#include <hermes/node-compat/bindings/node_errors.h>
#include <hermes/node-compat/bindings/node_file.h>
#include <hermes/node-compat/bindings/node_file_dir.h>
#include <hermes/node-compat/bindings/node_fs_event_wrap.h>
#include <hermes/node-compat/bindings/node_http_parser.h>
#include <hermes/node-compat/bindings/node_module_wrap.h>
#include <hermes/node-compat/bindings/node_modules.h>
#include <hermes/node-compat/bindings/node_os.h>
#include <hermes/node-compat/bindings/node_pipe_wrap.h>
#include <hermes/node-compat/bindings/node_process_wrap.h>
#include <hermes/node-compat/bindings/node_spawn_sync.h>
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
#include <hermes/node-compat/bindings/node_zlib.h>
#include <hermes/node-compat/embedded-modules/embedded_modules.h>
#include <hermes/node-compat/event-loop/uv_event_loop.h>
#include <hermes/node-compat/inspector/inspector_bridge.h>
#include <hermes/node-compat/module-loader/module_loader.h>
#include <hermes/node-compat/process/node_process.h>
#include <hermes/node-compat/runtime/runtime_state.h>

#include <uv.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#ifdef HERMES_ENABLE_DEBUGGER
#include <spawn.h>
#include <sys/wait.h>
#include <climits>
#include <cstdlib>
extern char **environ;
#endif

using namespace hermes::node_compat;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

#ifdef HERMES_ENABLE_DEBUGGER
/// Spawn the platform browser-opener (open on macOS, xdg-open on Linux)
/// to open \p url. Best-effort: failures are reported on stderr but do not
/// abort the parent. The DevTools URL printed above is still usable.
static void openDevToolsUrl(const char *url) {
#if defined(__APPLE__)
  const char *opener = "/usr/bin/open";
#elif defined(__linux__)
  const char *opener = "/usr/bin/xdg-open";
#else
  std::fprintf(stderr, "--inspect-open: unsupported platform\n");
  return;
#endif
  char *const argv[] = {
      const_cast<char *>(opener), const_cast<char *>(url), nullptr};
  pid_t pid = 0;
  int rc = posix_spawn(&pid, opener, nullptr, nullptr, argv, environ);
  if (rc != 0) {
    std::fprintf(
        stderr,
        "--inspect-open: posix_spawn(%s) failed: %s\n",
        opener,
        std::strerror(rc));
    return;
  }
  // Reap the child so it doesn't become a zombie. The opener returns
  // immediately after handing off to the browser.
  int status = 0;
  waitpid(pid, &status, 0);
}
#endif

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
// Fatal exception handler (napi_fatal_exception callback)
// ---------------------------------------------------------------------------

/// Called by Hermes NAPI when napi_fatal_exception() is invoked.
/// Routes to process.emit('uncaughtException', err). If no handler is
/// installed or the emit fails, prints the error and aborts.
static void onFatalException(void * /*data*/, napi_env env, napi_value err) {
  napi_handle_scope scope;
  napi_open_handle_scope(env, &scope);

  napi_value global;
  napi_get_global(env, &global);

  napi_value processObj;
  napi_get_named_property(env, global, "process", &processObj);

  napi_value emitFn;
  napi_get_named_property(env, processObj, "emit", &emitFn);

  napi_valuetype emitType;
  napi_typeof(env, emitFn, &emitType);

  bool handled = false;
  if (emitType == napi_function) {
    napi_value eventStr;
    napi_create_string_utf8(
        env, "uncaughtException", NAPI_AUTO_LENGTH, &eventStr);
    napi_value emitArgs[2] = {eventStr, err};
    napi_value emitResult;
    napi_status st =
        napi_call_function(env, processObj, emitFn, 2, emitArgs, &emitResult);
    if (st == napi_ok) {
      // process.emit returns true if there were listeners.
      bool result = false;
      napi_get_value_bool(env, emitResult, &result);
      handled = result;
    }
  }

  if (!handled) {
    // No listener handled it -- print and abort, matching Node.js behavior
    // for unhandled uncaughtException.
    napi_value errStr;
    napi_coerce_to_string(env, err, &errStr);
    char buf[4096];
    size_t len = 0;
    napi_get_value_string_utf8(env, errStr, buf, sizeof(buf), &len);
    std::fprintf(stderr, "%.*s\n", static_cast<int>(len), buf);

    // Clear any exception that may have been set during error formatting.
    bool pending = false;
    napi_is_exception_pending(env, &pending);
    if (pending) {
      napi_value exc;
      napi_get_and_clear_last_exception(env, &exc);
    }

    napi_close_handle_scope(env, scope);
    std::abort();
  }

  napi_close_handle_scope(env, scope);
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
// Event loop tick integration
// ---------------------------------------------------------------------------

/// Data passed to the uv_check_t callback for draining microtasks and ticks.
struct TickDrainData {
  napi_env env;
  hermes::vm::Runtime *runtime;
  napi_ref tickCallbackRef;
};

/// Drain the microtask queue and call the JS tick callback (runNextTicks).
/// Used by both the check and prepare handles.
static void drainTicksImpl(TickDrainData *data) {
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

/// Called on each event loop iteration (after I/O polling).
static void onCheckDrainTicks(uv_check_t *handle) {
  drainTicksImpl(static_cast<TickDrainData *>(handle->data));
}

/// Called on each event loop iteration (before I/O polling).
static void onPrepareDrainTicks(uv_prepare_t *handle) {
  drainTicksImpl(static_cast<TickDrainData *>(handle->data));
}

// ---------------------------------------------------------------------------
// Inspector state for CDP debugging (cross-thread communication)
// ---------------------------------------------------------------------------

#ifdef HERMES_ENABLE_DEBUGGER

/// Main-thread state for cross-thread CDP communication. Holds mutex-protected
/// queues for inbound CDP commands and RuntimeTasks, plus a uv_async_t handle
/// that signals the main event loop to drain them.
struct InspectorState {
  std::mutex mutex;
  std::queue<std::string> inboundCommands;
  std::queue<facebook::hermes::debugger::RuntimeTask> runtimeTasks;
  uv_async_t asyncHandle{};
  facebook::hermes::cdp::CDPAgent *agent = nullptr;
  facebook::hermes::HermesRuntime *hermesRT = nullptr;
  /// Set to false before closing the async handle to prevent uv_async_send
  /// calls on a closed handle (e.g. during CDPAgent destruction).
  std::atomic<bool> asyncActive{false};
};

/// uv_async_t callback: drain inbound CDP commands and runtime tasks.
static void onInspectorAsync(uv_async_t *handle) {
  auto *state = static_cast<InspectorState *>(handle->data);

  // Swap queues under lock to minimize lock hold time.
  std::queue<std::string> commands;
  std::queue<facebook::hermes::debugger::RuntimeTask> tasks;
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    commands.swap(state->inboundCommands);
    tasks.swap(state->runtimeTasks);
  }

  // Process inbound CDP commands.
  while (!commands.empty()) {
    if (state->agent) {
      state->agent->handleCommand(commands.front());
    }
    commands.pop();
  }

  // Execute runtime tasks.
  while (!tasks.empty()) {
    if (state->hermesRT) {
      tasks.front()(*state->hermesRT);
    }
    tasks.pop();
  }
}

/// Generate a UUID-like session ID for the inspector WebSocket path.
static std::string generateSessionId() {
  unsigned char bytes[16];
  int err = uv_random(nullptr, nullptr, bytes, sizeof(bytes), 0, nullptr);
  if (err != 0) {
    // Fallback: use high-res timer + process ID.
    uint64_t t = uv_hrtime();
    uint32_t pid = uv_os_getpid();
    std::memcpy(bytes, &t, sizeof(t));
    std::memcpy(bytes + sizeof(t), &pid, sizeof(pid));
    std::memset(
        bytes + sizeof(t) + sizeof(pid),
        0,
        sizeof(bytes) - sizeof(t) - sizeof(pid));
  }
  char buf[37];
  std::snprintf(
      buf,
      sizeof(buf),
      "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
      bytes[0],
      bytes[1],
      bytes[2],
      bytes[3],
      bytes[4],
      bytes[5],
      bytes[6],
      bytes[7],
      bytes[8],
      bytes[9],
      bytes[10],
      bytes[11],
      bytes[12],
      bytes[13],
      bytes[14],
      bytes[15]);
  return buf;
}

#endif // HERMES_ENABLE_DEBUGGER

// ---------------------------------------------------------------------------
// runHermesNode
// ---------------------------------------------------------------------------

namespace hermes {
namespace node_compat {

int runHermesNode(const HermesNodeConfig &config) {
  // 1. Create Hermes runtime with microtask queue enabled.
  auto rtConfig = hermes::vm::RuntimeConfig::Builder()
                      .withMicrotaskQueue(true)
                      .withEnableAsyncGenerators(true)
                      .withES6BlockScoping(true)
                      .build();
  auto hermesRT = facebook::hermes::makeHermesRuntime(rtConfig);
  auto *vmRuntime =
      static_cast<hermes::vm::Runtime *>(hermesRT->getVMRuntimeUnsafe());

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

  // 3. Create napi_env with host integration.
  eventLoop.getHost()->fatal_exception = onFatalException;

  napi_env env = hermes_napi_create_env(vmRuntime, eventLoop.getHost());

  // Allocate and install per-instance state for bindings.
  auto *runtimeState = new RuntimeState();
  runtimeState->loop = eventLoop.getLoop();
  runtimeState->drainMicrotasksFn = [](void *data) {
    static_cast<hermes::vm::Runtime *>(data)->drainJobs();
  };
  runtimeState->drainMicrotasksData = vmRuntime;
  // IMPORTANT: dispatch through HermesRuntime's virtual method, NOT through
  // vm::Runtime::triggerTimeoutAsyncBreak() directly. The latter is inline in
  // Runtime.h, and its layout depends on Hermes-private compile defines
  // (HERMES_MEMORY_INSTRUMENTATION, HERMES_CHECK_NATIVE_STACK, etc.) that this
  // TU does not see. Inlining it here would compute a wrong field offset and
  // corrupt unrelated Runtime state. The virtual dispatch ensures the body
  // executes in Hermes's TU with the correct layout.
  runtimeState->triggerAsyncBreakFn = [](void *data) {
    static_cast<facebook::hermes::HermesRuntime *>(data)->asyncTriggerTimeout();
  };
  runtimeState->triggerAsyncBreakData = hermesRT.get();
  runtimeState->inspectorBridgeContext = config.inspectorBridgeContext;
  // Use a no-op finalizer: RuntimeState must outlive the env because GC
  // finalizers (which run during runtime destruction, after env is freed) may
  // still reference it via cached rtState_ pointers. We delete it manually
  // after hermesRT.reset() below.
  napi_set_instance_data(
      env, runtimeState, [](napi_env, void *, void *) {}, nullptr);

  // Create CDP objects and inspector async handle for debugger support.
#ifdef HERMES_ENABLE_DEBUGGER
  std::unique_ptr<facebook::hermes::cdp::CDPDebugAPI> cdpDebugAPI;
  std::unique_ptr<facebook::hermes::cdp::CDPAgent> cdpAgent;
  InspectorState inspectorState;
  bool inspectorAsyncActive = false;
  std::thread inspectorThread;
  InspectorBridgeContext *bridgeCtx = nullptr;
  if (config.inspect) {
    cdpDebugAPI = facebook::hermes::cdp::CDPDebugAPI::create(*hermesRT);

    // Initialize the async handle for cross-thread CDP signaling.
    uv_async_init(
        eventLoop.getLoop(), &inspectorState.asyncHandle, onInspectorAsync);
    inspectorState.asyncHandle.data = &inspectorState;
    uv_unref(reinterpret_cast<uv_handle_t *>(&inspectorState.asyncHandle));
    inspectorState.asyncActive.store(true, std::memory_order_release);
    inspectorAsyncActive = true;

    // Allocate the bridge context before CDPAgent so the outbound
    // messageCallback can capture it.
    std::string sessionId = generateSessionId();
    bridgeCtx = new InspectorBridgeContext();
    bridgeCtx->host = config.inspectHost;
    bridgeCtx->port = config.inspectPort;
    bridgeCtx->scriptName = config.scriptPath;
    bridgeCtx->sessionId = sessionId;

    // Wire up inbound (inspector -> main) pointers.
    bridgeCtx->inboundMutex = &inspectorState.mutex;
    bridgeCtx->inboundQueue = &inspectorState.inboundCommands;
    bridgeCtx->mainAsync = &inspectorState.asyncHandle;
    bridgeCtx->mainAsyncActive = &inspectorState.asyncActive;

    cdpAgent = facebook::hermes::cdp::CDPAgent::create(
        /*executionContextID=*/
        1,
        *cdpDebugAPI,
        /*enqueueRuntimeTask=*/
        [&inspectorState](facebook::hermes::debugger::RuntimeTask task) {
          {
            std::lock_guard<std::mutex> lock(inspectorState.mutex);
            inspectorState.runtimeTasks.push(std::move(task));
          }
          if (inspectorState.asyncActive.load(std::memory_order_acquire)) {
            uv_async_send(&inspectorState.asyncHandle);
          }
        },
        /*messageCallback=*/
        [bridgeCtx](const std::string &msg) {
          {
            std::lock_guard<std::mutex> lock(bridgeCtx->outboundMutex);
            bridgeCtx->outboundQueue.push(msg);
          }
          if (bridgeCtx->inspectorAsyncActive.load(std::memory_order_acquire)) {
            uv_async_send(&bridgeCtx->inspectorAsync);
          }
        });

    inspectorState.agent = cdpAgent.get();
    inspectorState.hermesRT = hermesRT.get();
    cdpAgent->enableRuntimeDomain();
    if (config.inspectBrk) {
      // Pre-enable the debugger domain so its event callback is registered
      // before user code. The task fires during the first JS execution
      // (primordials), setting pauseOnScriptLoad=true and registering the
      // coordinator's event callback. This is required for the
      // coordinator().pause() call in step 12b below to actually pause the VM.
      cdpAgent->enableDebuggerDomain();
    }

    // Set up the interrupt-based drain for inbound CDP commands. When the
    // runtime is paused at a breakpoint, the event loop is not running, so
    // uv_async_send alone cannot deliver commands. This function triggers a
    // runtime interrupt that drains the inbound queue and calls handleCommand.
    bridgeCtx->triggerInboundDrain = [&inspectorState, &cdpDebugAPI]() {
      cdpDebugAPI->asyncDebuggerAPI().triggerInterrupt_TS(
          [&inspectorState](facebook::hermes::HermesRuntime &) {
            std::queue<std::string> commands;
            {
              std::lock_guard<std::mutex> lock(inspectorState.mutex);
              commands.swap(inspectorState.inboundCommands);
            }
            while (!commands.empty()) {
              if (inspectorState.agent) {
                inspectorState.agent->handleCommand(commands.front());
              }
              commands.pop();
            }
          });
    };

    // Build inspector config and launch the inspector thread.
    HermesNodeConfig inspectorConfig;
    inspectorConfig.evalCode = "require('inspector-server');";
    inspectorConfig.argv = {"hermes-node-inspector"};
    inspectorConfig.inspectorBridgeContext = bridgeCtx;

    inspectorThread = std::thread([bridgeCtx, inspectorConfig]() {
      runHermesNode(inspectorConfig);
      // Ensure the main thread's readyCv wait doesn't hang if the inspector
      // failed to start (notifyReady was never called).
      {
        std::lock_guard<std::mutex> lock(bridgeCtx->readyMutex);
        bridgeCtx->ready = true;
      }
      bridgeCtx->readyCv.notify_one();
    });

    // Wait for the inspector server to signal it's listening.
    {
      std::unique_lock<std::mutex> lock(bridgeCtx->readyMutex);
      bridgeCtx->readyCv.wait(lock, [&] { return bridgeCtx->ready; });
    }

    if (bridgeCtx->actualPort > 0) {
      std::fprintf(
          stderr,
          "Debugger listening on ws://%s:%d/%s\n",
          bridgeCtx->host.c_str(),
          bridgeCtx->actualPort,
          bridgeCtx->sessionId.c_str());
      std::fprintf(
          stderr, "For help, see: https://nodejs.org/en/docs/inspector\n");
      char devtoolsUrl[1024];
      std::snprintf(
          devtoolsUrl,
          sizeof(devtoolsUrl),
          "http://%s:%d/devtools/inspector.html?ws=%s:%d/%s",
          bridgeCtx->host.c_str(),
          bridgeCtx->actualPort,
          bridgeCtx->host.c_str(),
          bridgeCtx->actualPort,
          bridgeCtx->sessionId.c_str());
      std::fprintf(stderr, "Open DevTools: %s\n", devtoolsUrl);
      if (config.inspectOpen) {
        openDevToolsUrl(devtoolsUrl);
      }
    } else {
      std::fprintf(stderr, "Warning: inspector failed to start\n");
    }
  }
#endif

  napi_handle_scope scope;
  napi_open_handle_scope(env, &scope);

  int exitCode = 0;

  // 4. Install console object.
  if (installConsole(env) != napi_ok) {
    std::fprintf(stderr, "Error: failed to install console\n");
    exitCode = 1;
  }

  // 5. Register native bindings.
  BindingRegistry registry;
  registry.registerBinding("async_context_frame", initAsyncContextFrameBinding);
  registry.registerBinding("async_wrap", initAsyncWrapBinding);
  registry.registerBinding("buffer", initBufferBinding);
  registry.registerBinding("cares_wrap", initCaresWrapBinding);
  registry.registerBinding("config", initConfigBinding);
  registry.registerBinding("constants", initConstantsBinding);
  registry.registerBinding("contextify", initContextifyBinding);
  registry.registerBinding("credentials", initCredentialsBinding);
  registry.registerBinding("crypto", initCryptoBinding);
  registry.registerBinding("encoding_binding", initEncodingBinding);
  registry.registerBinding("errors", initErrorsBinding);
  registry.registerBinding("fs", initFsBinding);
  registry.registerBinding("fs_dir", initFsDirBinding);
  registry.registerBinding("fs_event_wrap", initFsEventWrapBinding);
  registry.registerBinding("http_parser", initHttpParserBinding);
  registry.registerBinding("inspector_bridge", initInspectorBridgeBinding);
  registry.registerBinding("module_wrap", initModuleWrapBinding);
  registry.registerBinding("modules", initModulesBinding);
  registry.registerBinding("os", initOsBinding);
  registry.registerBinding("pipe_wrap", initPipeWrapBinding);
  registry.registerBinding("process_wrap", initProcessWrapBinding);
  registry.registerBinding("spawn_sync", initSpawnSyncBinding);
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
  registry.registerBinding("zlib", initZlibBinding);
  registry.attach(env);

  // 6. Load and execute primordials from embedded bytecode.
  if (exitCode == 0) {
    napi_value result;
    napi_status st = runEmbeddedModule(env, "primordials", &result);
    if (st != napi_ok) {
      std::fprintf(stderr, "Error: failed to execute primordials\n");
      printAndClearException(env);
      exitCode = 1;
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
    if (!config.argv.empty()) {
      nodeArgv = config.argv;
    } else {
      nodeArgv.push_back("hermes-node");
    }
    proc.setArgv(std::move(nodeArgv));
    if (!config.nodeVersion.empty())
      proc.setVersion(config.nodeVersion.c_str());

    char execBuf[4096];
    size_t execSize = sizeof(execBuf);
    if (uv_exepath(execBuf, &execSize) == 0) {
      proc.setExecPath(std::string(execBuf, execSize));
    } else {
      proc.setExecPath(config.argv.empty() ? "hermes-node" : config.argv[0]);
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

      // Add minimal process event emitter methods (on/emit/emitWarning).
      napi_value peResult;
      if (runEmbeddedModule(env, "process-events", &peResult) != napi_ok) {
        std::fprintf(stderr, "Error: failed to execute process-events\n");
        printAndClearException(env);
        exitCode = 1;
      }
    }
  }

  // 9. Initialize the module loader.
  ModuleLoader loader;

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
        napi_value nextTickFn;
        napi_get_named_property(env, setupResult, "nextTick", &nextTickFn);

        napi_value runNextTicksFn;
        napi_get_named_property(
            env, setupResult, "runNextTicks", &runNextTicksFn);

        napi_value processObj;
        napi_get_named_property(env, global, "process", &processObj);
        napi_set_named_property(env, processObj, "nextTick", nextTickFn);
        napi_set_named_property(
            env, processObj, "_tickCallback", runNextTicksFn);

        napi_create_reference(env, runNextTicksFn, 1, &tickCallbackRef);
      }
    }
  }

  // 11. Set up timers (setTimeout, setInterval, setImmediate).
  if (exitCode == 0 && tickCallbackRef) {
    napi_value internalTimersModule;
    if (loader.require(env, "internal/timers", &internalTimersModule) !=
        napi_ok) {
      std::fprintf(stderr, "Error: failed to load internal/timers\n");
      printAndClearException(env);
      exitCode = 1;
    } else {
      napi_value runNextTicksFn;
      napi_get_reference_value(env, tickCallbackRef, &runNextTicksFn);

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

  // 11b. Initialize debuglog.
  if (exitCode == 0) {
    napi_value debuglogModule;
    if (loader.require(env, "internal/util/debuglog", &debuglogModule) ==
        napi_ok) {
      napi_value initDebugEnvFn;
      napi_get_named_property(
          env, debuglogModule, "initializeDebugEnv", &initDebugEnvFn);

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

  // 11c. Set up process.stdin, process.stdout, process.stderr.
  if (exitCode == 0) {
    napi_value result;
    napi_status st = runEmbeddedModule(env, "setup-stdio", &result);
    if (st != napi_ok) {
      std::fprintf(stderr, "Error: failed to execute setup-stdio\n");
      printAndClearException(env);
      exitCode = 1;
    }
  }

  // 11d. Load and install the real console module.
  if (exitCode == 0) {
    napi_value ciResult;
    if (runEmbeddedModule(env, "console-init", &ciResult) != napi_ok) {
      // Non-fatal: fall back to the minimal C++ console from step 4.
      printAndClearException(env);
    }
  }

  // 11e. Initialize Node's CJS module loader.
  if (exitCode == 0) {
    napi_value initCJSFn;
    napi_get_named_property(env, global, "__initCJS", &initCJSFn);
    napi_value initResult;
    if (napi_call_function(env, global, initCJSFn, 0, nullptr, &initResult) !=
        napi_ok) {
      // Non-fatal.
      printAndClearException(env);
    }
  }

  // 12. Set up event loop check and prepare handles for tick draining.
  uv_check_t checkHandle;
  uv_prepare_t prepareHandle;
  TickDrainData tickDrainData{env, vmRuntime, tickCallbackRef};
  bool tickHandlesActive = false;

  if (exitCode == 0 && tickCallbackRef) {
    uv_check_init(eventLoop.getLoop(), &checkHandle);
    checkHandle.data = &tickDrainData;
    uv_check_start(&checkHandle, onCheckDrainTicks);
    uv_unref(reinterpret_cast<uv_handle_t *>(&checkHandle));

    uv_prepare_init(eventLoop.getLoop(), &prepareHandle);
    prepareHandle.data = &tickDrainData;
    uv_prepare_start(&prepareHandle, onPrepareDrainTicks);
    uv_unref(reinterpret_cast<uv_handle_t *>(&prepareHandle));

    tickHandlesActive = true;
  }

  // 12b. For --inspect-brk, register a CDP breakpoint at the user file's
  // first user statement so the runtime pauses there once the script is
  // parsed and execution enters the wrapper function body.
  //
  // We rely on Debugger.setBreakpointByUrl rather than coordinator.pause()
  // or a synthetic `debugger;` statement because:
  //   - CDP's ScriptLoaded handler always auto-resumes (it never user-pauses).
  //   - coordinator.pause()'s ExplicitPause flag, set this early, gets
  //     swallowed by the legacy Debugger's runUntilValidPauseLocation when
  //     the bytecode it lands on isn't yet a valid pause point.
  //   - Setting the breakpoint here means it's already pending when
  //     compileFunctionForCJSLoader parses the user file; processScript
  //     resolves it against the matching URL and the Breakpoint event is
  //     delivered as a real user-visible pause.
  //
  // Column offset: the wrapper function expression starts at column 0 on
  // line 1, with the user's first character at column kCJSWrapperPrefixLen.
  // Setting the breakpoint at that column makes the resolver land inside
  // the wrapper body rather than on the script-level function-expression
  // statement (which evaluates without entering the body).
#ifdef HERMES_ENABLE_DEBUGGER
  if (exitCode == 0 && config.inspectBrk && cdpDebugAPI) {
    auto flushRuntimeTasks = [&]() {
      std::queue<facebook::hermes::debugger::RuntimeTask> tasks;
      {
        std::lock_guard<std::mutex> lock(inspectorState.mutex);
        tasks.swap(inspectorState.runtimeTasks);
      }
      while (!tasks.empty()) {
        tasks.front()(*hermesRT);
        tasks.pop();
      }
    };
    // Flush enableDebuggerDomain so the agent is wired up before we add
    // the breakpoint or run the synthetic debugger; statement.
    flushRuntimeTasks();

    if (!config.scriptPath.empty()) {
      // File mode: resolve the user script path to absolute form so it
      // matches what compileFunctionForCJSLoader records as the script's
      // sourceURL.
      char absScriptPath[PATH_MAX] = {0};
      const char *url = config.scriptPath.c_str();
      if (realpath(config.scriptPath.c_str(), absScriptPath)) {
        url = absScriptPath;
      }
      char cmd[PATH_MAX + 256];
      std::snprintf(
          cmd,
          sizeof(cmd),
          "{\"id\":1,\"method\":\"Debugger.setBreakpointByUrl\","
          "\"params\":{\"url\":\"%s\",\"lineNumber\":0,\"columnNumber\":%zu}}",
          url,
          kCJSWrapperPrefixLen);
      cdpAgent->handleCommand(cmd);
      // Without setBreakpointsActive(true) the Breakpoint event handler in
      // DebuggerDomainCoordinator auto-resumes instead of user-pausing.
      cdpAgent->handleCommand(
          "{\"id\":2,\"method\":\"Debugger.setBreakpointsActive\","
          "\"params\":{\"active\":true}}");
      flushRuntimeTasks();
    } else {
      // Eval / REPL mode: there is no user file URL to set a breakpoint
      // on, so fall back to running a synthetic `debugger;` statement
      // (the same primitive the binding used before commit 5d36ec8).
      // The legacy Debugger fires immediately and blocks on the
      // coordinator until a DevTools client connects and sends Resume.
      // The synthetic-frame DevTools UX caveat from that commit only
      // matters in file mode, where the user expects to land in their
      // own source.
      napi_value brkScript;
      napi_create_string_utf8(
          env,
          "debugger; //# sourceURL=hermes-node:inspect-brk",
          NAPI_AUTO_LENGTH,
          &brkScript);
      napi_value brkResult;
      napi_run_script(env, brkScript, &brkResult);
    }
  }
#endif

  // 13. Load and execute the user script, eval code, or start the REPL.
  if (exitCode == 0) {
    if (!config.scriptPath.empty()) {
      napi_value loadUserScriptFn;
      napi_get_named_property(
          env, global, "__loadUserScript", &loadUserScriptFn);

      napi_value scriptPathStr;
      napi_create_string_utf8(
          env, config.scriptPath.c_str(), NAPI_AUTO_LENGTH, &scriptPathStr);

      napi_value result;
      if (napi_call_function(
              env, global, loadUserScriptFn, 1, &scriptPathStr, &result) !=
          napi_ok) {
        printAndClearException(env);
        exitCode = 1;
      }
    } else if (!config.evalCode.empty()) {
      napi_value evalScript;
      napi_create_string_utf8(
          env, config.evalCode.c_str(), NAPI_AUTO_LENGTH, &evalScript);
      napi_value evalResult;
      if (napi_run_script(env, evalScript, &evalResult) != napi_ok) {
        printAndClearException(env);
        exitCode = 1;
      }
    } else if (config.enableRepl) {
      const char *replCode =
          "var cliRepl = require('internal/repl');\n"
          "cliRepl.createInternalRepl(process.env, function(err, repl) {\n"
          "  if (err) throw err;\n"
          "  repl.on('exit', function() {\n"
          "    if (repl.historyManager.isFlushing) {\n"
          "      repl.once('flushHistory', function() { process.exit(); });\n"
          "      return;\n"
          "    }\n"
          "    process.exit();\n"
          "  });\n"
          "});\n"
          "//# sourceURL=hermes-node:repl-start\n";
      napi_value replScript, replResult;
      napi_create_string_utf8(env, replCode, NAPI_AUTO_LENGTH, &replScript);
      if (napi_run_script(env, replScript, &replResult) != napi_ok) {
        printAndClearException(env);
        exitCode = 1;
      }
    }
  }

  // 14. Run event loop.
  if (exitCode == 0) {
    vmRuntime->drainJobs();

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

  // 15. Emit 'exit' event on process object.
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

  // 16. Cleanup (reverse order of creation).
  {
    napi_value processObj;
    napi_get_named_property(env, global, "process", &processObj);
    const char *stdioNames[] = {"stdin", "stdout", "stderr"};
    for (const char *name : stdioNames) {
      napi_value stream;
      napi_get_named_property(env, processObj, name, &stream);
      napi_valuetype stype;
      napi_typeof(env, stream, &stype);
      if (stype == napi_object) {
        napi_value handle;
        napi_get_named_property(env, stream, "_handle", &handle);
        napi_valuetype htype;
        napi_typeof(env, handle, &htype);
        if (htype == napi_object) {
          napi_value closeFn;
          napi_get_named_property(env, handle, "close", &closeFn);
          napi_valuetype ctype;
          napi_typeof(env, closeFn, &ctype);
          if (ctype == napi_function) {
            napi_value closeResult;
            napi_call_function(env, handle, closeFn, 0, nullptr, &closeResult);
            bool pending = false;
            napi_is_exception_pending(env, &pending);
            if (pending) {
              napi_value exc;
              napi_get_and_clear_last_exception(env, &exc);
            }
          }
        }
      }
    }
  }

  uv_run(eventLoop.getLoop(), UV_RUN_NOWAIT);

  closeTimersHandles(env);

  if (tickHandlesActive) {
    uv_check_stop(&checkHandle);
    uv_close(reinterpret_cast<uv_handle_t *>(&checkHandle), nullptr);
    uv_prepare_stop(&prepareHandle);
    uv_close(reinterpret_cast<uv_handle_t *>(&prepareHandle), nullptr);
  }

  // Shut down the inspector thread and close the inspector async handle.
#ifdef HERMES_ENABLE_DEBUGGER
  if (inspectorAsyncActive) {
    // Signal the inspector thread to stop its event loop.
    if (inspectorThread.joinable() && bridgeCtx) {
      std::lock_guard<std::mutex> lock(bridgeCtx->shutdownMutex);
      if (bridgeCtx->canSendShutdown) {
        uv_async_send(&bridgeCtx->shutdownAsync);
      }
    }

    // Prevent further uv_async_send calls from the inspector thread
    // (or from CDPAgent destruction).
    inspectorState.asyncActive.store(false, std::memory_order_release);
    inspectorState.agent = nullptr;

    // Wait for the inspector thread to complete its cleanup.
    if (inspectorThread.joinable()) {
      inspectorThread.join();
    }

    uv_close(
        reinterpret_cast<uv_handle_t *>(&inspectorState.asyncHandle), nullptr);
  }
#endif

  uv_run(eventLoop.getLoop(), UV_RUN_NOWAIT);

  caresWrapShutdown(env);

  // If this is the inspector runtime, mark async handles as no longer safe
  // before closing the event loop. This prevents the main thread from sending
  // to inspectorAsync or shutdownAsync after the loop is destroyed.
  if (config.inspectorBridgeContext) {
    auto *bc =
        static_cast<InspectorBridgeContext *>(config.inspectorBridgeContext);
    bc->inspectorAsyncActive.store(false, std::memory_order_release);
    std::lock_guard<std::mutex> lock(bc->shutdownMutex);
    bc->canSendShutdown = false;
  }

  eventLoop.close();

  runtimeState->loop = nullptr;

  if (tickCallbackRef) {
    napi_delete_reference(env, tickCallbackRef);
  }

  loader.detach(env);
  proc.detach(env);
  registry.detach(env);

  napi_close_handle_scope(env, scope);

  // Destroy CDP objects before env and runtime (reverse creation order).
  // CDPAgent destructor may enqueue runtime tasks; drain them manually since
  // the async handle is already closed.
#ifdef HERMES_ENABLE_DEBUGGER
  cdpAgent.reset();
  if (inspectorAsyncActive) {
    std::lock_guard<std::mutex> lock(inspectorState.mutex);
    while (!inspectorState.runtimeTasks.empty()) {
      inspectorState.runtimeTasks.front()(*hermesRT);
      inspectorState.runtimeTasks.pop();
    }
    inspectorState.hermesRT = nullptr;
    inspectorAsyncActive = false;
  }
  cdpDebugAPI.reset();
  delete bridgeCtx;
#endif

  hermes_napi_destroy_env(env);
  hermesRT.reset();
  delete runtimeState;

  return exitCode;
}

} // namespace node_compat
} // namespace hermes
