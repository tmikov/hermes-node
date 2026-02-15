/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// hermes_napi.h must come before uv_event_loop.h to avoid double definition
// of hermes_napi_event_loop struct.
#include "hermes_napi.h"

#include "hermes/Public/RuntimeConfig.h"
#include "hermes/VM/Runtime.h"

#include <hermes/node-compat/binding-registry/binding_registry.h>
#include <hermes/node-compat/bindings/node_config.h>
#include <hermes/node-compat/bindings/node_constants.h>
#include <hermes/node-compat/bindings/node_errors.h>
#include <hermes/node-compat/bindings/node_string_decoder.h>
#include <hermes/node-compat/bindings/node_types.h>
#include <hermes/node-compat/bindings/node_util.h>
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
  napi_env env = hermes_napi_create_env(*runtime, eventLoop.getEventLoop());

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
  registry.registerBinding("config", initConfigBinding);
  registry.registerBinding("constants", initConstantsBinding);
  registry.registerBinding("errors", initErrorsBinding);
  registry.registerBinding("string_decoder", initStringDecoderBinding);
  registry.registerBinding("types", initTypesBinding);
  registry.registerBinding("util", initUtilBinding);
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

  // 10. Load and execute the user script.
  if (exitCode == 0) {
    std::string scriptSource = readFile(scriptPath);
    if (scriptSource.empty()) {
      std::fprintf(
          stderr, "Error: failed to read script '%s'\n", scriptPath);
      exitCode = 1;
    } else {
      scriptSource += "\n//# sourceURL=" + std::string(scriptPath) + "\n";

      napi_value scriptStr;
      napi_create_string_utf8(
          env, scriptSource.c_str(), scriptSource.size(), &scriptStr);
      napi_value result;
      if (napi_run_script(env, scriptStr, &result) != napi_ok) {
        printAndClearException(env);
        exitCode = 1;
      }
    }
  }

  // 11. Run event loop.
  if (exitCode == 0) {
    runtime->drainJobs();
    eventLoop.run();
  }

  // 12. Cleanup (reverse order of creation).
  loader.detach(env);
  proc.detach(env);
  registry.detach(env);

  napi_close_handle_scope(env, scope);
  hermes_napi_destroy_env(env);
  runtime.reset();
  eventLoop.close();

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
