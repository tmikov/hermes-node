/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <hermes/node-compat/bindings/node_contextify.h>
#include <node_api.h>

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
// ContextifyScript -- wraps compiled script source for evaluation.
//
// In Node, this compiles the script into V8's internal representation and
// caches it. Since Hermes does not expose a compile-without-execute API
// through NAPI, we store the source string and evaluate it on each
// runInContext call via napi_run_script.
// ---------------------------------------------------------------------------

/// ContextifyScript constructor callback.
///
/// Signature (from vm.js):
///   new ContextifyScript(code, filename, lineOffset, columnOffset,
///                        cachedData, produceCachedData, parsingContext,
///                        hostDefinedOptionId)
///
/// For our implementation we only use code and filename.
/// We store them as properties on the JS object using internal keys.
static napi_value contextifyScriptNew(
    napi_env env,
    napi_callback_info info) {
  napi_handle_scope scope;
  napi_open_handle_scope(env, &scope);

  size_t argc = 8;
  napi_value argv[8];
  napi_value thisObj;
  napi_get_cb_info(env, info, &argc, argv, &thisObj, nullptr);

  // argv[0] = code (string)
  // argv[1] = filename (string)
  // argv[2] = lineOffset (int32) -- ignored
  // argv[3] = columnOffset (int32) -- ignored
  // argv[4] = cachedData (ArrayBufferView or undefined) -- ignored
  // argv[5] = produceCachedData (bool) -- ignored
  // argv[6] = parsingContext (object or undefined) -- ignored
  // argv[7] = hostDefinedOptionId (symbol) -- ignored

  // Get the code string.
  std::string code;
  if (argc > 0) {
    code = napiGetString(env, argv[0]);
  }

  // Get the filename string (for sourceURL in stack traces).
  std::string filename;
  if (argc > 1) {
    filename = napiGetString(env, argv[1]);
  }

  // Build the full source: append sourceURL directive for stack traces.
  std::string fullSource = code;
  if (!filename.empty()) {
    fullSource += "\n//# sourceURL=";
    fullSource += filename;
    fullSource += "\n";
  }

  // Store the full source on the object as a non-enumerable property
  // using a unique key that JS code won't collide with.
  napi_value sourceKey;
  napi_create_string_utf8(
      env, "__contextifySource", NAPI_AUTO_LENGTH, &sourceKey);
  napi_value sourceVal;
  napi_create_string_utf8(
      env, fullSource.c_str(), fullSource.size(), &sourceVal);

  napi_property_descriptor sourceProp = {
      nullptr,     // utf8name (we use name)
      sourceKey,   // name
      nullptr,     // method
      nullptr,     // getter
      nullptr,     // setter
      sourceVal,   // value
      napi_default, // attributes (non-enumerable, non-configurable)
      nullptr,     // data
  };
  napi_define_properties(env, thisObj, 1, &sourceProp);

  napi_close_handle_scope(env, scope);
  return thisObj;
}

/// ContextifyScript.prototype.runInContext(sandbox, timeout, displayErrors,
///                                         breakOnSigint, breakOnFirstLine)
///
/// When sandbox is null/undefined, evaluates in the current (global) context
/// (the runInThisContext path). When sandbox is an object, we also evaluate
/// in the global context since we don't support real sandboxing.
static napi_value contextifyScriptRunInContext(
    napi_env env,
    napi_callback_info info) {
  napi_handle_scope scope;
  napi_open_handle_scope(env, &scope);

  size_t argc = 5;
  napi_value argv[5];
  napi_value thisObj;
  napi_get_cb_info(env, info, &argc, argv, &thisObj, nullptr);

  // Retrieve the stored source code.
  napi_value sourceKey;
  napi_create_string_utf8(
      env, "__contextifySource", NAPI_AUTO_LENGTH, &sourceKey);
  napi_value sourceVal;
  bool hasSource = false;
  napi_has_property(env, thisObj, sourceKey, &hasSource);
  if (!hasSource) {
    napi_throw_error(
        env, nullptr, "Script has no source (was it constructed properly?)");
    napi_close_handle_scope(env, scope);
    return nullptr;
  }
  napi_get_property(env, thisObj, sourceKey, &sourceVal);

  // Evaluate the script via napi_run_script (global context, non-strict).
  napi_value result;
  napi_status st = napi_run_script(env, sourceVal, &result);

  napi_close_handle_scope(env, scope);

  if (st != napi_ok) {
    // napi_run_script already threw a JS exception on failure.
    return nullptr;
  }
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

/// makeContext(sandbox, ...) -- stub: just returns the sandbox.
/// Real implementation in R9.
static napi_value makeContextCb(napi_env env, napi_callback_info info) {
  size_t argc = 7;
  napi_value argv[7];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  // Return the sandbox object as-is.
  if (argc > 0)
    return argv[0];

  napi_value undef;
  napi_get_undefined(env, &undef);
  return undef;
}

/// measureMemory() -> undefined (stub)
static napi_value measureMemoryCb(napi_env env, napi_callback_info /*info*/) {
  napi_value undef;
  napi_get_undefined(env, &undef);
  return undef;
}

/// startSigintWatchdog() -> true (stub)
/// Real implementation in R19.
static napi_value
startSigintWatchdogCb(napi_env env, napi_callback_info /*info*/) {
  napi_value result;
  napi_get_boolean(env, true, &result);
  return result;
}

/// stopSigintWatchdog() -> false (stub, no pending SIGINT)
/// Real implementation in R19.
static napi_value
stopSigintWatchdogCb(napi_env env, napi_callback_info /*info*/) {
  napi_value result;
  napi_get_boolean(env, false, &result);
  return result;
}

/// watchdogHasPendingSigint() -> false (stub)
static napi_value
watchdogHasPendingSigintCb(napi_env env, napi_callback_info /*info*/) {
  napi_value result;
  napi_get_boolean(env, false, &result);
  return result;
}

/// compileFunction(code, filename, lineOffset, columnOffset, cachedData,
///                 produceCachedData, parsingContext, contextExtensions,
///                 params, hostDefinedOptionId)
/// -> { function: fn, cachedDataProduced: false }
///
/// Stub: creates a function from code with the given parameter names.
/// Real implementation in R9.
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

/// compileFunctionForCJSLoader(...) -> undefined (stub)
static napi_value
compileFunctionForCJSLoaderCb(napi_env env, napi_callback_info /*info*/) {
  napi_value undef;
  napi_get_undefined(env, &undef);
  return undef;
}

/// containsModuleSyntax(...) -> false (stub)
static napi_value
containsModuleSyntaxCb(napi_env env, napi_callback_info /*info*/) {
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
