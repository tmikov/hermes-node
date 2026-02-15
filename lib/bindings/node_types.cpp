/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <hermes/node-compat/bindings/node_types.h>
#include <node_api.h>

#include <cstring>

namespace hermes {
namespace node_compat {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/// Get a named global constructor as napi_value.
static napi_status
getGlobalCtor(napi_env env, const char *name, napi_value *result) {
  napi_value global;
  napi_status st = napi_get_global(env, &global);
  if (st != napi_ok)
    return st;
  return napi_get_named_property(env, global, name, result);
}

/// Check if `value` is an instance of the named global constructor.
static napi_status
isInstanceOfGlobal(napi_env env, napi_value value, const char *name, bool *out) {
  *out = false;

  // Non-objects can't be instances of anything.
  napi_valuetype vtype;
  napi_status st = napi_typeof(env, value, &vtype);
  if (st != napi_ok)
    return st;
  if (vtype != napi_object && vtype != napi_function)
    return napi_ok;

  napi_value ctor;
  st = getGlobalCtor(env, name, &ctor);
  if (st != napi_ok)
    return st;

  // If the constructor doesn't exist (e.g. SharedArrayBuffer), return false.
  napi_valuetype ctorType;
  st = napi_typeof(env, ctor, &ctorType);
  if (st != napi_ok)
    return st;
  if (ctorType != napi_function)
    return napi_ok;

  return napi_instanceof(env, value, ctor, out);
}

/// Helper: extract the first argument from callback info.
static napi_value getArg0(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv;
  napi_get_cb_info(env, info, &argc, &argv, nullptr, nullptr);
  if (argc == 0) {
    napi_value undef;
    napi_get_undefined(env, &undef);
    return undef;
  }
  return argv;
}

/// Helper: return a boolean result.
static napi_value returnBool(napi_env env, bool val) {
  napi_value result;
  napi_get_boolean(env, val, &result);
  return result;
}

// ---------------------------------------------------------------------------
// Type check functions using direct NAPI calls
// ---------------------------------------------------------------------------

static napi_value isArrayBuffer(napi_env env, napi_callback_info info) {
  napi_value arg = getArg0(env, info);
  bool result = false;
  napi_is_arraybuffer(env, arg, &result);
  return returnBool(env, result);
}

static napi_value isDataView(napi_env env, napi_callback_info info) {
  napi_value arg = getArg0(env, info);
  bool result = false;
  napi_is_dataview(env, arg, &result);
  return returnBool(env, result);
}

static napi_value isDate(napi_env env, napi_callback_info info) {
  napi_value arg = getArg0(env, info);
  bool result = false;
  napi_is_date(env, arg, &result);
  return returnBool(env, result);
}

static napi_value isPromise(napi_env env, napi_callback_info info) {
  napi_value arg = getArg0(env, info);
  bool result = false;
  napi_is_promise(env, arg, &result);
  return returnBool(env, result);
}

static napi_value isTypedArray(napi_env env, napi_callback_info info) {
  napi_value arg = getArg0(env, info);
  bool result = false;
  napi_is_typedarray(env, arg, &result);
  return returnBool(env, result);
}

static napi_value isExternal(napi_env env, napi_callback_info info) {
  napi_value arg = getArg0(env, info);
  napi_valuetype vtype;
  napi_typeof(env, arg, &vtype);
  return returnBool(env, vtype == napi_external);
}

static napi_value isNativeError(napi_env env, napi_callback_info info) {
  napi_value arg = getArg0(env, info);
  bool result = false;
  napi_is_error(env, arg, &result);
  return returnBool(env, result);
}

// ---------------------------------------------------------------------------
// Type check functions using instanceof
// ---------------------------------------------------------------------------

static napi_value isMap(napi_env env, napi_callback_info info) {
  napi_value arg = getArg0(env, info);
  bool result = false;
  isInstanceOfGlobal(env, arg, "Map", &result);
  return returnBool(env, result);
}

static napi_value isSet(napi_env env, napi_callback_info info) {
  napi_value arg = getArg0(env, info);
  bool result = false;
  isInstanceOfGlobal(env, arg, "Set", &result);
  return returnBool(env, result);
}

static napi_value isWeakMap(napi_env env, napi_callback_info info) {
  napi_value arg = getArg0(env, info);
  bool result = false;
  isInstanceOfGlobal(env, arg, "WeakMap", &result);
  return returnBool(env, result);
}

static napi_value isWeakSet(napi_env env, napi_callback_info info) {
  napi_value arg = getArg0(env, info);
  bool result = false;
  isInstanceOfGlobal(env, arg, "WeakSet", &result);
  return returnBool(env, result);
}

static napi_value isRegExp(napi_env env, napi_callback_info info) {
  napi_value arg = getArg0(env, info);
  bool result = false;
  isInstanceOfGlobal(env, arg, "RegExp", &result);
  return returnBool(env, result);
}

static napi_value isSharedArrayBuffer(napi_env env, napi_callback_info info) {
  napi_value arg = getArg0(env, info);
  bool result = false;
  isInstanceOfGlobal(env, arg, "SharedArrayBuffer", &result);
  return returnBool(env, result);
}

// ---------------------------------------------------------------------------
// Composite type checks
// ---------------------------------------------------------------------------

static napi_value isAnyArrayBuffer(napi_env env, napi_callback_info info) {
  napi_value arg = getArg0(env, info);
  bool isAB = false;
  napi_is_arraybuffer(env, arg, &isAB);
  if (isAB)
    return returnBool(env, true);
  bool isSAB = false;
  isInstanceOfGlobal(env, arg, "SharedArrayBuffer", &isSAB);
  return returnBool(env, isSAB);
}

static napi_value isArrayBufferView(napi_env env, napi_callback_info info) {
  napi_value arg = getArg0(env, info);
  bool isTA = false;
  napi_is_typedarray(env, arg, &isTA);
  if (isTA)
    return returnBool(env, true);
  bool isDV = false;
  napi_is_dataview(env, arg, &isDV);
  return returnBool(env, isDV);
}

// ---------------------------------------------------------------------------
// Boxed primitive checks via Object.prototype.toString
// ---------------------------------------------------------------------------

/// Check if value is a boxed primitive of a given type by checking
/// Object.prototype.toString.call(value) === '[object <Tag>]'.
static bool isObjectWithTag(napi_env env, napi_value value, const char *tag) {
  napi_valuetype vtype;
  napi_typeof(env, value, &vtype);
  if (vtype != napi_object)
    return false;

  // Get Object.prototype.toString
  napi_value global, objectCtor, objectProto, toStringFn;
  napi_get_global(env, &global);
  napi_get_named_property(env, global, "Object", &objectCtor);
  napi_get_named_property(env, objectCtor, "prototype", &objectProto);
  napi_get_named_property(env, objectProto, "toString", &toStringFn);

  // Call toString.call(value)
  napi_value result;
  napi_call_function(env, value, toStringFn, 1, &value, &result);

  // Get the string result
  char buf[64];
  size_t len = 0;
  napi_get_value_string_utf8(env, result, buf, sizeof(buf), &len);

  return std::strcmp(buf, tag) == 0;
}

static napi_value isNumberObject(napi_env env, napi_callback_info info) {
  napi_value arg = getArg0(env, info);
  return returnBool(env, isObjectWithTag(env, arg, "[object Number]"));
}

static napi_value isStringObject(napi_env env, napi_callback_info info) {
  napi_value arg = getArg0(env, info);
  return returnBool(env, isObjectWithTag(env, arg, "[object String]"));
}

static napi_value isBooleanObject(napi_env env, napi_callback_info info) {
  napi_value arg = getArg0(env, info);
  return returnBool(env, isObjectWithTag(env, arg, "[object Boolean]"));
}

static napi_value isBigIntObject(napi_env env, napi_callback_info info) {
  napi_value arg = getArg0(env, info);
  return returnBool(env, isObjectWithTag(env, arg, "[object BigInt]"));
}

static napi_value isSymbolObject(napi_env env, napi_callback_info info) {
  napi_value arg = getArg0(env, info);
  return returnBool(env, isObjectWithTag(env, arg, "[object Symbol]"));
}

static napi_value isBoxedPrimitive(napi_env env, napi_callback_info info) {
  napi_value arg = getArg0(env, info);
  bool result = isObjectWithTag(env, arg, "[object Number]") ||
      isObjectWithTag(env, arg, "[object String]") ||
      isObjectWithTag(env, arg, "[object Boolean]") ||
      isObjectWithTag(env, arg, "[object BigInt]") ||
      isObjectWithTag(env, arg, "[object Symbol]");
  return returnBool(env, result);
}

// ---------------------------------------------------------------------------
// Function subtype checks (AsyncFunction, GeneratorFunction)
// ---------------------------------------------------------------------------

/// Check if a function's constructor name matches the expected name.
/// Used for isAsyncFunction, isGeneratorFunction.
static bool isFunctionWithCtorName(
    napi_env env,
    napi_value value,
    const char *ctorName) {
  napi_valuetype vtype;
  napi_typeof(env, value, &vtype);
  if (vtype != napi_function)
    return false;

  // Check Object.prototype.toString.call(value) for the tag.
  // AsyncFunction -> "[object AsyncFunction]"
  // GeneratorFunction -> "[object GeneratorFunction]"
  napi_value global, objectCtor, objectProto, toStringFn;
  napi_get_global(env, &global);
  napi_get_named_property(env, global, "Object", &objectCtor);
  napi_get_named_property(env, objectCtor, "prototype", &objectProto);
  napi_get_named_property(env, objectProto, "toString", &toStringFn);

  napi_value result;
  napi_call_function(env, value, toStringFn, 1, &value, &result);

  char buf[64];
  size_t len = 0;
  napi_get_value_string_utf8(env, result, buf, sizeof(buf), &len);

  return std::strcmp(buf, ctorName) == 0;
}

static napi_value isAsyncFunction(napi_env env, napi_callback_info info) {
  napi_value arg = getArg0(env, info);
  return returnBool(
      env, isFunctionWithCtorName(env, arg, "[object AsyncFunction]"));
}

static napi_value isGeneratorFunction(napi_env env, napi_callback_info info) {
  napi_value arg = getArg0(env, info);
  return returnBool(
      env, isFunctionWithCtorName(env, arg, "[object GeneratorFunction]"));
}

// ---------------------------------------------------------------------------
// Generator object check
// ---------------------------------------------------------------------------

static napi_value isGeneratorObject(napi_env env, napi_callback_info info) {
  napi_value arg = getArg0(env, info);
  return returnBool(
      env, isObjectWithTag(env, arg, "[object Generator]"));
}

// ---------------------------------------------------------------------------
// Iterator checks (MapIterator, SetIterator)
// ---------------------------------------------------------------------------

static napi_value isMapIterator(napi_env env, napi_callback_info info) {
  napi_value arg = getArg0(env, info);
  return returnBool(
      env, isObjectWithTag(env, arg, "[object Map Iterator]"));
}

static napi_value isSetIterator(napi_env env, napi_callback_info info) {
  napi_value arg = getArg0(env, info);
  return returnBool(
      env, isObjectWithTag(env, arg, "[object Set Iterator]"));
}

// ---------------------------------------------------------------------------
// Stubs for V8-specific checks
// ---------------------------------------------------------------------------

static napi_value isProxy(napi_env env, napi_callback_info info) {
  // No NAPI way to detect proxies. Always returns false.
  (void)info;
  return returnBool(env, false);
}

static napi_value isModuleNamespaceObject(
    napi_env env,
    napi_callback_info info) {
  // V8-specific. Always returns false.
  (void)info;
  return returnBool(env, false);
}

static napi_value isArgumentsObject(napi_env env, napi_callback_info info) {
  napi_value arg = getArg0(env, info);
  return returnBool(
      env, isObjectWithTag(env, arg, "[object Arguments]"));
}

// ---------------------------------------------------------------------------
// Uint8Array check (used by internal/util/types.js override, but also
// sometimes referenced from the native binding)
// ---------------------------------------------------------------------------

static napi_value isUint8Array(napi_env env, napi_callback_info info) {
  napi_value arg = getArg0(env, info);
  bool isTA = false;
  napi_is_typedarray(env, arg, &isTA);
  if (!isTA)
    return returnBool(env, false);
  napi_typedarray_type taType;
  size_t len;
  void *data;
  napi_value abuf;
  size_t offset;
  napi_get_typedarray_info(env, arg, &taType, &len, &data, &abuf, &offset);
  return returnBool(env, taType == napi_uint8_array);
}

// ---------------------------------------------------------------------------
// Init function
// ---------------------------------------------------------------------------

napi_value initTypesBinding(napi_env env, napi_value exports) {
  // clang-format off
  napi_property_descriptor props[] = {
    {"isArrayBuffer",          nullptr, isArrayBuffer,          nullptr, nullptr, nullptr, napi_enumerable, nullptr},
    {"isArrayBufferView",      nullptr, isArrayBufferView,      nullptr, nullptr, nullptr, napi_enumerable, nullptr},
    {"isAsyncFunction",        nullptr, isAsyncFunction,        nullptr, nullptr, nullptr, napi_enumerable, nullptr},
    {"isDataView",             nullptr, isDataView,             nullptr, nullptr, nullptr, napi_enumerable, nullptr},
    {"isDate",                 nullptr, isDate,                 nullptr, nullptr, nullptr, napi_enumerable, nullptr},
    {"isExternal",             nullptr, isExternal,             nullptr, nullptr, nullptr, napi_enumerable, nullptr},
    {"isMap",                  nullptr, isMap,                  nullptr, nullptr, nullptr, napi_enumerable, nullptr},
    {"isMapIterator",          nullptr, isMapIterator,          nullptr, nullptr, nullptr, napi_enumerable, nullptr},
    {"isModuleNamespaceObject",nullptr, isModuleNamespaceObject,nullptr, nullptr, nullptr, napi_enumerable, nullptr},
    {"isNativeError",          nullptr, isNativeError,          nullptr, nullptr, nullptr, napi_enumerable, nullptr},
    {"isPromise",              nullptr, isPromise,              nullptr, nullptr, nullptr, napi_enumerable, nullptr},
    {"isRegExp",               nullptr, isRegExp,               nullptr, nullptr, nullptr, napi_enumerable, nullptr},
    {"isSet",                  nullptr, isSet,                  nullptr, nullptr, nullptr, napi_enumerable, nullptr},
    {"isSetIterator",          nullptr, isSetIterator,          nullptr, nullptr, nullptr, napi_enumerable, nullptr},
    {"isSharedArrayBuffer",    nullptr, isSharedArrayBuffer,    nullptr, nullptr, nullptr, napi_enumerable, nullptr},
    {"isTypedArray",           nullptr, isTypedArray,           nullptr, nullptr, nullptr, napi_enumerable, nullptr},
    {"isWeakMap",              nullptr, isWeakMap,              nullptr, nullptr, nullptr, napi_enumerable, nullptr},
    {"isWeakSet",              nullptr, isWeakSet,              nullptr, nullptr, nullptr, napi_enumerable, nullptr},
    {"isGeneratorFunction",    nullptr, isGeneratorFunction,    nullptr, nullptr, nullptr, napi_enumerable, nullptr},
    {"isGeneratorObject",      nullptr, isGeneratorObject,      nullptr, nullptr, nullptr, napi_enumerable, nullptr},
    {"isArgumentsObject",      nullptr, isArgumentsObject,      nullptr, nullptr, nullptr, napi_enumerable, nullptr},
    {"isNumberObject",         nullptr, isNumberObject,         nullptr, nullptr, nullptr, napi_enumerable, nullptr},
    {"isStringObject",         nullptr, isStringObject,         nullptr, nullptr, nullptr, napi_enumerable, nullptr},
    {"isBooleanObject",        nullptr, isBooleanObject,        nullptr, nullptr, nullptr, napi_enumerable, nullptr},
    {"isBigIntObject",         nullptr, isBigIntObject,         nullptr, nullptr, nullptr, napi_enumerable, nullptr},
    {"isSymbolObject",         nullptr, isSymbolObject,         nullptr, nullptr, nullptr, napi_enumerable, nullptr},
    {"isBoxedPrimitive",       nullptr, isBoxedPrimitive,       nullptr, nullptr, nullptr, napi_enumerable, nullptr},
    {"isProxy",                nullptr, isProxy,                nullptr, nullptr, nullptr, napi_enumerable, nullptr},
    {"isAnyArrayBuffer",       nullptr, isAnyArrayBuffer,       nullptr, nullptr, nullptr, napi_enumerable, nullptr},
    {"isUint8Array",           nullptr, isUint8Array,           nullptr, nullptr, nullptr, napi_enumerable, nullptr},
  };
  // clang-format on

  napi_define_properties(
      env, exports, sizeof(props) / sizeof(props[0]), props);
  return exports;
}

} // namespace node_compat
} // namespace hermes
