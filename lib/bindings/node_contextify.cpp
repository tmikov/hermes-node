/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <hermes/node-compat/bindings/node_contextify.h>
#include <napi/hermes_napi.h>
#include <napi/hermes_napi_compile.h>
#include <node_api.h>

#include <atomic>
#include <csignal>
#include <cstring>
#include <string>

namespace hermes {
namespace node_compat {

#define NAPI_CALL(call)                                             \
  do {                                                              \
    napi_status status_ = (call);                                   \
    if (status_ != napi_ok) {                                       \
      napi_throw_error(env, nullptr, "NAPI call failed in " #call); \
      return nullptr;                                               \
    }                                                               \
  } while (0)

/// Cached reference to the contextify_context_private_symbol from
/// internalBinding('util').privateSymbols. Lazily initialized on first
/// makeContext call.
static napi_ref s_contextPrivateSymbolRef = nullptr;

// ---------------------------------------------------------------------------
// SIGINT watchdog state
// ---------------------------------------------------------------------------

/// Callback to trigger an async break in the JS engine.
static TriggerAsyncBreakFn s_triggerAsyncBreak = nullptr;
static void *s_triggerAsyncBreakData = nullptr;

/// Whether the SIGINT watchdog is currently active.
static std::atomic<bool> s_sigintWatching{false};

/// Whether a SIGINT was received while the watchdog was active.
static std::atomic<bool> s_sigintReceived{false};

/// Previous SIGINT handler, restored when the watchdog stops.
static struct sigaction s_previousSigaction;

/// SIGINT signal handler. Called from the signal handler context.
/// Must be async-signal-safe: only sets atomic flags and calls
/// the async break callback (which is documented as safe from any thread
/// or signal handler).
static void sigintHandler(int /*signo*/) {
  s_sigintReceived.store(true, std::memory_order_relaxed);
  if (s_triggerAsyncBreak) {
    s_triggerAsyncBreak(s_triggerAsyncBreakData);
  }
}

void setContextifyAsyncBreak(TriggerAsyncBreakFn fn, void *data) {
  s_triggerAsyncBreak = fn;
  s_triggerAsyncBreakData = data;
}

// ---------------------------------------------------------------------------
// Helper: get a std::string from a napi_value string argument.
// Returns empty string if the value is not a string.
// ---------------------------------------------------------------------------
static std::string napiGetString(napi_env env, napi_value value) {
  napi_valuetype type;
  napi_typeof(env, value, &type);
  if (type != napi_string)
    return {};

  size_t len = 0;
  napi_get_value_string_utf8(env, value, nullptr, 0, &len);
  std::string result(len, '\0');
  napi_get_value_string_utf8(env, value, &result[0], len + 1, nullptr);
  return result;
}

// ---------------------------------------------------------------------------
// ContextifyScript -- compiles source to bytecode in constructor, caches it,
// and executes via hermes_run_bytecode in runInContext.
//
// Compiling in the constructor throws SyntaxError immediately on parse
// failure, which the REPL depends on for multi-line input detection.
// ---------------------------------------------------------------------------

/// Cached compiled bytecode, attached to the JS object via
/// napi_create_external.
struct CompiledScript {
  uint8_t *bytecode;
  size_t bytecodeSize;
  ~CompiledScript() {
    hermes_free_bytecode(bytecode);
  }
};

static void
compiledScriptDestroy(napi_env /*env*/, void *data, void * /*hint*/) {
  delete static_cast<CompiledScript *>(data);
}

/// ContextifyScript constructor callback.
///
/// Signature (from vm.js):
///   new ContextifyScript(code, filename, lineOffset, columnOffset,
///                        cachedData, produceCachedData, parsingContext,
///                        hostDefinedOptionId)
///
/// Compiles source to bytecode and caches it. Throws SyntaxError on
/// parse failure.
static napi_value contextifyScriptNew(napi_env env, napi_callback_info info) {
  napi_handle_scope scope;
  napi_open_handle_scope(env, &scope);

  size_t argc = 8;
  napi_value argv[8];
  napi_value thisObj;
  napi_get_cb_info(env, info, &argc, argv, &thisObj, nullptr);

  // argv[0] = code (string)
  // argv[1] = filename (string)
  // argv[2..7] ignored

  std::string code;
  if (argc > 0)
    code = napiGetString(env, argv[0]);

  std::string filename;
  if (argc > 1)
    filename = napiGetString(env, argv[1]);

  // Append sourceURL directive for stack traces.
  std::string fullSource = code;
  if (!filename.empty()) {
    fullSource += "\n//# sourceURL=";
    fullSource += filename;
    fullSource += "\n";
  }

  // Compile to bytecode. Throws SyntaxError on parse failure.
  hermes_compile_flags cflags{};
  cflags.struct_size = sizeof(cflags);
  cflags.strict = false;
  cflags.emit_async_break_check = true;
  uint8_t *bytecodeData = nullptr;
  size_t bytecodeSize = 0;
  napi_status compileStatus = hermes_compile_to_bytecode(
      env,
      reinterpret_cast<const uint8_t *>(fullSource.c_str()),
      fullSource.size(),
      filename.empty() ? nullptr : filename.c_str(),
      &cflags,
      &bytecodeData,
      &bytecodeSize);
  if (compileStatus != napi_ok) {
    napi_close_handle_scope(env, scope);
    return nullptr;
  }

  // Store compiled bytecode on the script object.
  auto *compiled = new CompiledScript{bytecodeData, bytecodeSize};
  napi_value external;
  if (napi_create_external(
          env, compiled, compiledScriptDestroy, nullptr, &external) !=
      napi_ok) {
    delete compiled;
    napi_close_handle_scope(env, scope);
    return nullptr;
  }
  napi_set_named_property(env, thisObj, "__compiledBytecode", external);

  napi_close_handle_scope(env, scope);
  return thisObj;
}

/// ContextifyScript.prototype.runInContext(sandbox, timeout, displayErrors,
///                                         breakOnSigint, breakOnFirstLine)
///
/// When sandbox is null/undefined, evaluates in the current (global) context
/// (the runInThisContext path). When sandbox is an object, we also evaluate
/// in the global context since we don't support real sandboxing.
///
/// If the SIGINT watchdog is active and a timeout exception occurs due to
/// SIGINT, this function converts the uncatchable timeout error into a
/// catchable Error so the REPL can recover.
static napi_value contextifyScriptRunInContext(
    napi_env env,
    napi_callback_info info) {
  napi_handle_scope scope;
  napi_open_handle_scope(env, &scope);

  size_t argc = 5;
  napi_value argv[5];
  napi_value thisObj;
  napi_get_cb_info(env, info, &argc, argv, &thisObj, nullptr);

  // Retrieve the cached compiled bytecode.
  napi_value external;
  napi_get_named_property(env, thisObj, "__compiledBytecode", &external);

  napi_valuetype extType;
  napi_typeof(env, external, &extType);
  if (extType != napi_external) {
    napi_throw_error(
        env, nullptr, "Script has no bytecode (was it constructed properly?)");
    napi_close_handle_scope(env, scope);
    return nullptr;
  }

  void *data;
  napi_get_value_external(env, external, &data);
  auto *compiled = static_cast<CompiledScript *>(data);

  // Execute the cached bytecode.
  napi_value result;
  napi_status st = hermes_run_bytecode(
      env,
      compiled->bytecode,
      compiled->bytecodeSize,
      nullptr, // finalize_cb: we own the buffer
      nullptr, // finalize_hint
      nullptr, // source_url: already in bytecode
      nullptr, // flags: defaults
      &result);

  if (st != napi_ok) {
    // Check if this failure was caused by a SIGINT-triggered async break.
    if (s_sigintReceived.load(std::memory_order_relaxed)) {
      napi_value exc;
      napi_get_and_clear_last_exception(env, &exc);
      napi_throw_error(
          env,
          "ERR_SCRIPT_EXECUTION_INTERRUPTED",
          "Script execution was interrupted by `SIGINT`");
    }
    napi_close_handle_scope(env, scope);
    return nullptr;
  }

  napi_close_handle_scope(env, scope);
  return result;
}

/// createCachedData() -> undefined (stub)
/// We don't support V8 code caching.
static napi_value contextifyScriptCreateCachedData(
    napi_env env,
    napi_callback_info /*info*/) {
  napi_value undef;
  napi_get_undefined(env, &undef);
  return undef;
}

// ---------------------------------------------------------------------------
// Standalone functions exported on the binding
// ---------------------------------------------------------------------------

/// Get the cached contextify_context_private_symbol.
/// On first call, retrieves it from
/// globalThis.internalBinding('util').privateSymbols and caches it.
static napi_value getContextPrivateSymbol(napi_env env) {
  if (s_contextPrivateSymbolRef) {
    napi_value sym;
    napi_get_reference_value(env, s_contextPrivateSymbolRef, &sym);
    return sym;
  }

  // Get globalThis.internalBinding('util').privateSymbols
  //                                .contextify_context_private_symbol
  napi_value global;
  napi_get_global(env, &global);

  napi_value internalBindingFn;
  napi_get_named_property(env, global, "internalBinding", &internalBindingFn);

  napi_value utilStr;
  napi_create_string_utf8(env, "util", NAPI_AUTO_LENGTH, &utilStr);

  napi_value utilBinding;
  napi_status st = napi_call_function(
      env, global, internalBindingFn, 1, &utilStr, &utilBinding);
  if (st != napi_ok)
    return nullptr;

  napi_value privateSymbols;
  napi_get_named_property(env, utilBinding, "privateSymbols", &privateSymbols);

  napi_value sym;
  napi_get_named_property(
      env, privateSymbols, "contextify_context_private_symbol", &sym);

  // Cache it as a strong reference.
  napi_create_reference(env, sym, 1, &s_contextPrivateSymbolRef);
  return sym;
}

/// makeContext(sandbox, name, origin, strings, wasm, microtaskQueue,
///             hostDefinedOptionId)
///
/// Marks the sandbox object as a "context" by setting the private symbol
/// so that isContext() returns true. Does not create real V8 sandboxing.
static napi_value makeContextCb(napi_env env, napi_callback_info info) {
  size_t argc = 7;
  napi_value argv[7];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  if (argc == 0) {
    napi_value undef;
    napi_get_undefined(env, &undef);
    return undef;
  }

  napi_value sandbox = argv[0];

  // Set the contextify_context_private_symbol on the sandbox so
  // isContext() returns true.
  napi_value sym = getContextPrivateSymbol(env);
  if (sym) {
    napi_value trueVal;
    napi_get_boolean(env, true, &trueVal);
    napi_set_property(env, sandbox, sym, trueVal);
  }

  return sandbox;
}

/// measureMemory() -> undefined (stub)
static napi_value measureMemoryCb(napi_env env, napi_callback_info /*info*/) {
  napi_value undef;
  napi_get_undefined(env, &undef);
  return undef;
}

/// startSigintWatchdog() -> bool
///
/// Installs a SIGINT signal handler that triggers an async break in
/// the Hermes runtime when Ctrl+C is pressed. Returns true on success.
static napi_value startSigintWatchdogCb(
    napi_env env,
    napi_callback_info /*info*/) {
  bool success = false;

  if (s_triggerAsyncBreak &&
      !s_sigintWatching.load(std::memory_order_relaxed)) {
    s_sigintReceived.store(false, std::memory_order_relaxed);

    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigintHandler;
    sa.sa_flags = 0;
    sigemptyset(&sa.sa_mask);

    if (sigaction(SIGINT, &sa, &s_previousSigaction) == 0) {
      s_sigintWatching.store(true, std::memory_order_relaxed);
      success = true;
    }
  }

  napi_value result;
  napi_get_boolean(env, success, &result);
  return result;
}

/// stopSigintWatchdog() -> bool
///
/// Restores the previous SIGINT handler and returns true if a SIGINT
/// was received since startSigintWatchdog() was called.
static napi_value stopSigintWatchdogCb(
    napi_env env,
    napi_callback_info /*info*/) {
  bool hadPendingSigint = false;

  if (s_sigintWatching.load(std::memory_order_relaxed)) {
    // Restore the previous handler.
    sigaction(SIGINT, &s_previousSigaction, nullptr);
    s_sigintWatching.store(false, std::memory_order_relaxed);

    hadPendingSigint = s_sigintReceived.load(std::memory_order_relaxed);
    s_sigintReceived.store(false, std::memory_order_relaxed);
  }

  napi_value result;
  napi_get_boolean(env, hadPendingSigint, &result);
  return result;
}

/// watchdogHasPendingSigint() -> bool
///
/// Returns true if a SIGINT was received since startSigintWatchdog().
static napi_value watchdogHasPendingSigintCb(
    napi_env env,
    napi_callback_info /*info*/) {
  bool pending = s_sigintReceived.load(std::memory_order_relaxed);
  napi_value result;
  napi_get_boolean(env, pending, &result);
  return result;
}

/// compileFunction(code, filename, lineOffset, columnOffset, cachedData,
///                 produceCachedData, parsingContext, contextExtensions,
///                 params, hostDefinedOptionId)
/// -> { function: fn, cachedDataProduced: false }
///
/// Creates a function from code with the given parameter names.
/// No real sandboxing -- parsingContext and contextExtensions are ignored.
static napi_value compileFunctionCb(napi_env env, napi_callback_info info) {
  napi_handle_scope scope;
  napi_open_handle_scope(env, &scope);

  size_t argc = 10;
  napi_value argv[10];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  // argv[0] = code (string)
  // argv[8] = params (array of strings)
  // Others ignored for now.

  std::string code;
  if (argc > 0)
    code = napiGetString(env, argv[0]);

  std::string filename;
  if (argc > 1)
    filename = napiGetString(env, argv[1]);

  // Build parameter list for new Function().
  // params is argv[8] -- an array of strings.
  std::string paramList;
  if (argc > 8) {
    napi_valuetype paramType;
    napi_typeof(env, argv[8], &paramType);
    if (paramType == napi_object) {
      uint32_t paramCount = 0;
      napi_get_array_length(env, argv[8], &paramCount);
      for (uint32_t i = 0; i < paramCount; ++i) {
        napi_value paramVal;
        napi_get_element(env, argv[8], i, &paramVal);
        std::string paramStr = napiGetString(env, paramVal);
        if (i > 0)
          paramList += ",";
        paramList += paramStr;
      }
    }
  }

  // Append sourceURL for stack traces.
  std::string fullBody = code;
  if (!filename.empty()) {
    fullBody += "\n//# sourceURL=";
    fullBody += filename;
    fullBody += "\n";
  }

  // Build: new Function(param1, param2, ..., body)
  // We do this via napi_run_script with a wrapper expression.
  // Escape the body for embedding in a string template.
  // Instead of complex escaping, use an indirect approach:
  // 1. Store body and params as JS values
  // 2. Use Function constructor via napi

  // Create the function using an eval-based approach:
  // (function(paramList) { body })
  // This is simpler and avoids complex string escaping issues.
  std::string wrapperCode = "(function(";
  wrapperCode += paramList;
  wrapperCode += ") {\n";
  wrapperCode += code;
  wrapperCode += "\n})";
  if (!filename.empty()) {
    wrapperCode += "\n//# sourceURL=";
    wrapperCode += filename;
    wrapperCode += "\n";
  }

  napi_value wrapperStr;
  napi_create_string_utf8(
      env, wrapperCode.c_str(), wrapperCode.size(), &wrapperStr);

  napi_value fn;
  napi_status st = napi_run_script(env, wrapperStr, &fn);
  if (st != napi_ok) {
    napi_close_handle_scope(env, scope);
    return nullptr;
  }

  // Build result object: { function: fn, cachedDataProduced: false }
  napi_value result;
  napi_create_object(env, &result);
  napi_set_named_property(env, result, "function", fn);

  napi_value falseVal;
  napi_get_boolean(env, false, &falseVal);
  napi_set_named_property(env, result, "cachedDataProduced", falseVal);

  napi_close_handle_scope(env, scope);
  return result;
}

/// compileFunctionForCJSLoader(content, filename, is_sea_main,
///                              should_detect_module)
/// -> { function, sourceMapURL, sourceURL, cachedDataRejected, canParseAsESM }
///
/// Wraps the source in a function with CJS parameters
/// (exports, require, module, __filename, __dirname), compiles it to
/// bytecode, and returns a callable wrapper function. This mirrors V8's
/// ScriptCompiler::CompileFunction with CJS parameters.
static napi_value compileFunctionForCJSLoaderCb(
    napi_env env,
    napi_callback_info info) {
  napi_handle_scope scope;
  napi_open_handle_scope(env, &scope);

  size_t argc = 4;
  napi_value argv[4];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  // argv[0] = content (string) -- raw unwrapped source
  // argv[1] = filename (string)
  // argv[2] = is_sea_main (boolean) -- always false for us
  // argv[3] = should_detect_module (boolean) -- we always return false

  std::string content;
  if (argc > 0)
    content = napiGetString(env, argv[0]);

  // Strip shebang line — Hermes doesn't handle #! like V8 does.
  if (content.size() >= 2 && content[0] == '#' && content[1] == '!') {
    auto nl = content.find('\n');
    if (nl != std::string::npos)
      content.erase(0, nl); // keep the \n to preserve line numbers
    else
      content.clear();
  }

  std::string filename;
  if (argc > 1)
    filename = napiGetString(env, argv[1]);

  // Wrap the source in a function with CJS parameters, matching Node's
  // GetCJSParameters: exports, require, module, __filename, __dirname.
  std::string wrappedSource;
  wrappedSource.reserve(content.size() + filename.size() + 128);
  wrappedSource +=
      "(function(exports, require, module, __filename, __dirname) {\n";
  wrappedSource += content;
  wrappedSource += "\n})";
  if (!filename.empty()) {
    wrappedSource += "\n//# sourceURL=";
    wrappedSource += filename;
    wrappedSource += "\n";
  }

  // Evaluate the wrapper expression to get the function.
  // Using napi_run_script (which handles buffer lifetime internally)
  // rather than hermes_compile_to_bytecode + hermes_run_bytecode,
  // since the returned function continues to reference the bytecode.
  napi_value sourceStr;
  napi_create_string_utf8(
      env, wrappedSource.c_str(), wrappedSource.size(), &sourceStr);

  napi_value fn;
  napi_status runStatus = napi_run_script(env, sourceStr, &fn);
  if (runStatus != napi_ok) {
    // Compilation failed (SyntaxError). The exception is already pending.
    napi_close_handle_scope(env, scope);
    return nullptr;
  }

  // Build result: { function, sourceMapURL, sourceURL,
  //                 cachedDataRejected, canParseAsESM }
  napi_value result;
  napi_create_object(env, &result);

  napi_set_named_property(env, result, "function", fn);

  napi_value undef;
  napi_get_undefined(env, &undef);
  napi_set_named_property(env, result, "sourceMapURL", undef);
  napi_set_named_property(env, result, "sourceURL", undef);

  napi_value falseVal;
  napi_get_boolean(env, false, &falseVal);
  napi_set_named_property(env, result, "cachedDataRejected", falseVal);
  napi_set_named_property(env, result, "canParseAsESM", falseVal);

  napi_close_handle_scope(env, scope);
  return result;
}

/// containsModuleSyntax(...) -> false (stub)
static napi_value containsModuleSyntaxCb(
    napi_env env,
    napi_callback_info /*info*/) {
  napi_value result;
  napi_get_boolean(env, false, &result);
  return result;
}

// ---------------------------------------------------------------------------
// initContextifyBinding
// ---------------------------------------------------------------------------

napi_value initContextifyBinding(napi_env env, napi_value exports) {
  napi_handle_scope scope;
  NAPI_CALL(napi_open_handle_scope(env, &scope));

  // --- ContextifyScript constructor ---
  napi_property_descriptor protoMethods[] = {
      {"runInContext",
       nullptr,
       contextifyScriptRunInContext,
       nullptr,
       nullptr,
       nullptr,
       napi_default,
       nullptr},
      {"createCachedData",
       nullptr,
       contextifyScriptCreateCachedData,
       nullptr,
       nullptr,
       nullptr,
       napi_default,
       nullptr},
  };

  napi_value scriptCtor;
  NAPI_CALL(napi_define_class(
      env,
      "ContextifyScript",
      NAPI_AUTO_LENGTH,
      contextifyScriptNew,
      nullptr,
      sizeof(protoMethods) / sizeof(protoMethods[0]),
      protoMethods,
      &scriptCtor));

  NAPI_CALL(
      napi_set_named_property(env, exports, "ContextifyScript", scriptCtor));

  // --- Standalone functions ---
  struct {
    const char *name;
    napi_callback cb;
  } funcs[] = {
      {"makeContext", makeContextCb},
      {"measureMemory", measureMemoryCb},
      {"startSigintWatchdog", startSigintWatchdogCb},
      {"stopSigintWatchdog", stopSigintWatchdogCb},
      {"watchdogHasPendingSigint", watchdogHasPendingSigintCb},
      {"compileFunction", compileFunctionCb},
      {"compileFunctionForCJSLoader", compileFunctionForCJSLoaderCb},
      {"containsModuleSyntax", containsModuleSyntaxCb},
  };

  for (const auto &f : funcs) {
    napi_value fn;
    NAPI_CALL(napi_create_function(
        env, f.name, NAPI_AUTO_LENGTH, f.cb, nullptr, &fn));
    NAPI_CALL(napi_set_named_property(env, exports, f.name, fn));
  }

  // --- constants object ---
  // constants.measureMemory.mode.SUMMARY = 0, DETAILED = 1
  // constants.measureMemory.execution.DEFAULT = 0, EAGER = 1
  {
    napi_value constants;
    NAPI_CALL(napi_create_object(env, &constants));

    napi_value measureMemoryConst;
    NAPI_CALL(napi_create_object(env, &measureMemoryConst));

    napi_value modeObj;
    NAPI_CALL(napi_create_object(env, &modeObj));
    napi_value v0, v1;
    NAPI_CALL(napi_create_int32(env, 0, &v0));
    NAPI_CALL(napi_create_int32(env, 1, &v1));
    NAPI_CALL(napi_set_named_property(env, modeObj, "SUMMARY", v0));
    NAPI_CALL(napi_set_named_property(env, modeObj, "DETAILED", v1));

    napi_value execObj;
    NAPI_CALL(napi_create_object(env, &execObj));
    NAPI_CALL(napi_create_int32(env, 0, &v0));
    NAPI_CALL(napi_create_int32(env, 1, &v1));
    NAPI_CALL(napi_set_named_property(env, execObj, "DEFAULT", v0));
    NAPI_CALL(napi_set_named_property(env, execObj, "EAGER", v1));

    NAPI_CALL(
        napi_set_named_property(env, measureMemoryConst, "mode", modeObj));
    NAPI_CALL(
        napi_set_named_property(env, measureMemoryConst, "execution", execObj));
    NAPI_CALL(napi_set_named_property(
        env, constants, "measureMemory", measureMemoryConst));

    NAPI_CALL(napi_set_named_property(env, exports, "constants", constants));
  }

  NAPI_CALL(napi_close_handle_scope(env, scope));
  return exports;
}

#undef NAPI_CALL

} // namespace node_compat
} // namespace hermes
