/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <hermes/node-compat/bindings/node_errors.h>

#include <node_api.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace hermes {
namespace node_compat {

// ---------------------------------------------------------------------------
// Reporting an error that nothing handled
// ---------------------------------------------------------------------------

/// Write \p error to stderr the way a crashing program should report itself:
/// the stack when there is one, since that is what names the line that
/// threw, and the coerced value otherwise -- `throw 'a string'` is legal and
/// still has to print something.
static void reportError(napi_env env, napi_value error) {
  napi_value stack;
  napi_status st = napi_get_named_property(env, error, "stack", &stack);
  napi_valuetype stackType = napi_undefined;
  if (st == napi_ok)
    napi_typeof(env, stack, &stackType);

  napi_value msg;
  if (stackType == napi_string) {
    msg = stack;
  } else {
    // Clear any pending exception from property access on non-object.
    bool pending = false;
    napi_is_exception_pending(env, &pending);
    if (pending)
      napi_get_and_clear_last_exception(env, &msg);
    napi_coerce_to_string(env, error, &msg);
  }

  char buf[8192];
  size_t len = 0;
  napi_get_value_string_utf8(env, msg, buf, sizeof(buf), &len);
  std::fprintf(stderr, "%.*s\n", static_cast<int>(len), buf);
}

bool triggerUncaughtException(napi_env env, napi_value error) {
  // Ask JavaScript first. process._fatalException runs the
  // 'uncaughtException' listeners and answers whether one of them took
  // responsibility; when none did, it has already set process.exitCode and
  // emitted 'exit' before answering, so nothing is left here but to report
  // and go. That division is Node's, and keeping it means a listener sees
  // an exception from a timer exactly as it sees one from the main script.
  bool handled = false;

  napi_value global;
  napi_value processObj;
  napi_valuetype processType = napi_undefined;
  if (napi_get_global(env, &global) == napi_ok &&
      napi_get_named_property(env, global, "process", &processObj) == napi_ok &&
      napi_typeof(env, processObj, &processType) == napi_ok &&
      processType == napi_object) {
    napi_value fatalFn;
    napi_valuetype fnType = napi_undefined;
    if (napi_get_named_property(env, processObj, "_fatalException", &fatalFn) ==
            napi_ok &&
        napi_typeof(env, fatalFn, &fnType) == napi_ok &&
        fnType == napi_function) {
      napi_value args[1] = {error};
      napi_value result;
      if (napi_call_function(env, processObj, fatalFn, 1, args, &result) ==
          napi_ok) {
        napi_get_value_bool(env, result, &handled);
      } else {
        // The handler itself threw. Node treats that as fatal rather than
        // recursing, and so do we: report the original error, which is the
        // one the program was actually about.
        bool pending = false;
        napi_is_exception_pending(env, &pending);
        if (pending) {
          napi_value ignored;
          napi_get_and_clear_last_exception(env, &ignored);
        }
        handled = false;
      }
    }
  }

  if (handled)
    return true;

  reportError(env, error);
  // 'exit' handlers have already run in JavaScript and may have written
  // something; flush before leaving, or the explanation goes missing along
  // with the program.
  std::fflush(nullptr);
  std::exit(1);
}

// ---------------------------------------------------------------------------
// triggerUncaughtException(error, fromPromise)
// ---------------------------------------------------------------------------

static napi_value triggerUncaughtExceptionCallback(
    napi_env env,
    napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  if (argc < 1) {
    napi_throw_error(
        env, nullptr, "triggerUncaughtException requires at least 1 argument");
    return nullptr;
  }

  reportError(env, argv[0]);

  std::exit(1);
  return nullptr;
}

// ---------------------------------------------------------------------------
// noSideEffectsToString(value)
// ---------------------------------------------------------------------------

static napi_value noSideEffectsToString(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  if (argc < 1) {
    napi_value undef;
    napi_get_undefined(env, &undef);
    return undef;
  }

  // Use napi_coerce_to_string which is the simplest available approach.
  // This isn't truly side-effect-free (toString() can be overridden),
  // but it's sufficient for our use case in error formatting.
  napi_value result;
  napi_status st = napi_coerce_to_string(env, argv[0], &result);
  if (st != napi_ok) {
    // If coercion fails, return "[object Object]" as a fallback.
    napi_create_string_utf8(env, "[object Object]", NAPI_AUTO_LENGTH, &result);
  }
  return result;
}

// ---------------------------------------------------------------------------
// Stubs for source map / stack trace callbacks
// ---------------------------------------------------------------------------

static napi_value noopStub(napi_env env, napi_callback_info /*info*/) {
  napi_value undef;
  napi_get_undefined(env, &undef);
  return undef;
}

// ---------------------------------------------------------------------------
// Exit codes
// ---------------------------------------------------------------------------

/// Set an integer property on an object.
static void
setExitCode(napi_env env, napi_value obj, const char *name, int value) {
  napi_value val;
  napi_create_int32(env, value, &val);
  napi_set_named_property(env, obj, name, val);
}

// ---------------------------------------------------------------------------
// initErrorsBinding
// ---------------------------------------------------------------------------

napi_value initErrorsBinding(napi_env env, napi_value exports) {
  // triggerUncaughtException
  {
    napi_value fn;
    napi_create_function(
        env,
        "triggerUncaughtException",
        NAPI_AUTO_LENGTH,
        triggerUncaughtExceptionCallback,
        nullptr,
        &fn);
    napi_set_named_property(env, exports, "triggerUncaughtException", fn);
  }

  // noSideEffectsToString
  {
    napi_value fn;
    napi_create_function(
        env,
        "noSideEffectsToString",
        NAPI_AUTO_LENGTH,
        noSideEffectsToString,
        nullptr,
        &fn);
    napi_set_named_property(env, exports, "noSideEffectsToString", fn);
  }

  // Stubs: setPrepareStackTraceCallback, setGetSourceMapErrorSource,
  // setSourceMapsEnabled, setMaybeCacheGeneratedSourceMap,
  // setEnhanceStackForFatalException, getErrorSourcePositions
  const char *stubs[] = {
      "setPrepareStackTraceCallback",
      "setGetSourceMapErrorSource",
      "setSourceMapsEnabled",
      "setMaybeCacheGeneratedSourceMap",
      "setEnhanceStackForFatalException",
      "getErrorSourcePositions",
  };
  for (const char *name : stubs) {
    napi_value fn;
    napi_create_function(env, name, NAPI_AUTO_LENGTH, noopStub, nullptr, &fn);
    napi_set_named_property(env, exports, name, fn);
  }

  // exitCodes object — matches Node's ExitCode enum from node_exit_code.h
  {
    napi_value exitCodes;
    napi_create_object(env, &exitCodes);

    setExitCode(env, exitCodes, "kNoFailure", 0);
    setExitCode(env, exitCodes, "kGenericUserError", 1);
    setExitCode(env, exitCodes, "kInternalJSParseError", 3);
    setExitCode(env, exitCodes, "kInternalJSEvaluationFailure", 4);
    setExitCode(env, exitCodes, "kV8FatalError", 5);
    setExitCode(env, exitCodes, "kInvalidFatalExceptionMonkeyPatching", 6);
    setExitCode(env, exitCodes, "kExceptionInFatalExceptionHandler", 7);
    setExitCode(env, exitCodes, "kInvalidCommandLineArgument", 9);
    setExitCode(env, exitCodes, "kBootstrapFailure", 10);
    setExitCode(env, exitCodes, "kInvalidCommandLineArgument2", 12);
    setExitCode(env, exitCodes, "kUnsettledTopLevelAwait", 13);
    setExitCode(env, exitCodes, "kStartupSnapshotFailure", 14);
    setExitCode(env, exitCodes, "kAbort", 134);

    napi_set_named_property(env, exports, "exitCodes", exitCodes);
  }

  return exports;
}

} // namespace node_compat
} // namespace hermes
