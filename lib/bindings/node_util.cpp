/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <hermes/node-compat/bindings/node_util.h>
#include <node_api.h>
#include <uv.h>

#include <cstring>

namespace hermes {
namespace node_compat {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/// Set a named int32 property on an object.
static napi_status setInt32Property(
    napi_env env,
    napi_value obj,
    const char *name,
    int32_t value) {
  napi_value val;
  napi_status st = napi_create_int32(env, value, &val);
  if (st != napi_ok)
    return st;
  return napi_set_named_property(env, obj, name, val);
}

// ---------------------------------------------------------------------------
// getOwnNonIndexProperties(object, filter) -> string[]
// ---------------------------------------------------------------------------

/// V8 PropertyFilter values (must match V8's enum).
enum PropertyFilter {
  ALL_PROPERTIES = 0,
  ONLY_WRITABLE = 1,
  ONLY_ENUMERABLE = 2,
  ONLY_CONFIGURABLE = 4,
  SKIP_STRINGS = 8,
  SKIP_SYMBOLS = 16,
};

/// Check if a string is a valid array index (non-negative integer < 2^32 - 1).
static bool isArrayIndex(const char *buf, size_t len) {
  if (len == 0)
    return false;
  // Leading zeros are not valid indices (except "0" itself).
  if (buf[0] == '0' && len > 1)
    return false;
  for (size_t j = 0; j < len; ++j) {
    if (buf[j] < '0' || buf[j] > '9')
      return false;
  }
  // Max array index is 4294967294 (0xFFFFFFFE), which is 10 digits.
  if (len > 10)
    return false;
  if (len == 10 && std::strcmp(buf, "4294967294") > 0)
    return false;
  return true;
}

static napi_value
getOwnNonIndexProperties(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  if (argc < 2) {
    napi_throw_error(env, nullptr, "getOwnNonIndexProperties requires 2 args");
    return nullptr;
  }

  // Get the filter flags.
  uint32_t filter = 0;
  napi_get_value_uint32(env, argv[1], &filter);

  napi_value result;
  napi_create_array(env, &result);
  uint32_t outIdx = 0;

  // Build the key filter from V8 PropertyFilter flags.
  napi_key_filter keyFilter = napi_key_all_properties;
  if (filter & ONLY_WRITABLE)
    keyFilter = static_cast<napi_key_filter>(keyFilter | napi_key_writable);
  if (filter & ONLY_ENUMERABLE)
    keyFilter = static_cast<napi_key_filter>(keyFilter | napi_key_enumerable);
  if (filter & ONLY_CONFIGURABLE)
    keyFilter =
        static_cast<napi_key_filter>(keyFilter | napi_key_configurable);
  if (filter & SKIP_STRINGS)
    keyFilter =
        static_cast<napi_key_filter>(keyFilter | napi_key_skip_strings);
  if (filter & SKIP_SYMBOLS)
    keyFilter =
        static_cast<napi_key_filter>(keyFilter | napi_key_skip_symbols);

  napi_value names;
  napi_status st = napi_get_all_property_names(
      env,
      argv[0],
      napi_key_own_only,
      keyFilter,
      napi_key_numbers_to_strings,
      &names);
  if (st != napi_ok)
    return nullptr;

  uint32_t len = 0;
  napi_get_array_length(env, names, &len);

  for (uint32_t i = 0; i < len; ++i) {
    napi_value key;
    napi_get_element(env, names, i, &key);

    // Skip array indices (only applies to string keys).
    napi_valuetype keyType;
    napi_typeof(env, key, &keyType);
    if (keyType == napi_string) {
      char buf[16];
      size_t bufLen = 0;
      napi_get_value_string_utf8(env, key, buf, sizeof(buf), &bufLen);
      if (isArrayIndex(buf, bufLen))
        continue;
    }

    napi_set_element(env, result, outIdx++, key);
  }

  return result;
}

// ---------------------------------------------------------------------------
// getConstructorName(object) -> string
// ---------------------------------------------------------------------------

static napi_value getConstructorName(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv;
  napi_get_cb_info(env, info, &argc, &argv, nullptr, nullptr);

  if (argc == 0) {
    napi_value undef;
    napi_get_undefined(env, &undef);
    return undef;
  }

  // Create the "constructor" key string once.
  napi_value ctorKeyStr;
  napi_create_string_utf8(env, "constructor", NAPI_AUTO_LENGTH, &ctorKeyStr);

  // Walk up the prototype chain looking for constructor.name.
  napi_value current = argv;
  while (true) {
    napi_valuetype currentType;
    napi_typeof(env, current, &currentType);
    if (currentType != napi_object && currentType != napi_function)
      break;

    bool hasCtor = false;
    napi_has_own_property(env, current, ctorKeyStr, &hasCtor);

    if (hasCtor) {
      napi_value ctor;
      napi_get_named_property(env, current, "constructor", &ctor);
      napi_valuetype ctorType;
      napi_typeof(env, ctor, &ctorType);
      if (ctorType == napi_function) {
        napi_value name;
        napi_get_named_property(env, ctor, "name", &name);
        return name;
      }
    }

    napi_value nextProto;
    napi_status st = napi_get_prototype(env, current, &nextProto);
    if (st != napi_ok)
      break;

    // Check if we've reached the end of the prototype chain.
    napi_valuetype nextType;
    napi_typeof(env, nextProto, &nextType);
    if (nextType == napi_null || nextType == napi_undefined)
      break;

    current = nextProto;
  }

  napi_value empty;
  napi_create_string_utf8(env, "", 0, &empty);
  return empty;
}

// ---------------------------------------------------------------------------
// getPromiseDetails(promise) -> [state, result] | undefined
// Stub: returns undefined (V8-specific API).
// ---------------------------------------------------------------------------

static napi_value getPromiseDetails(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv;
  napi_get_cb_info(env, info, &argc, &argv, nullptr, nullptr);

  if (argc == 0) {
    napi_value undef;
    napi_get_undefined(env, &undef);
    return undef;
  }

  // Check if the argument is a promise.
  bool isPromise = false;
  napi_is_promise(env, argv, &isPromise);
  if (!isPromise) {
    napi_value undef;
    napi_get_undefined(env, &undef);
    return undef;
  }

  // Return undefined — we can't inspect promise internals via NAPI.
  napi_value undef;
  napi_get_undefined(env, &undef);
  return undef;
}

// ---------------------------------------------------------------------------
// getProxyDetails(proxy) -> [target, handler] | undefined
// Stub: returns undefined (V8-specific API).
// ---------------------------------------------------------------------------

static napi_value getProxyDetails(napi_env env, napi_callback_info info) {
  napi_value undef;
  napi_get_undefined(env, &undef);
  return undef;
}

// ---------------------------------------------------------------------------
// getCallerLocation() -> [line, column, filename] | undefined
// Stub: returns undefined (V8-specific stack introspection).
// ---------------------------------------------------------------------------

static napi_value getCallerLocation(napi_env env, napi_callback_info info) {
  napi_value undef;
  napi_get_undefined(env, &undef);
  return undef;
}

// ---------------------------------------------------------------------------
// previewEntries(obj) -> entries | [entries, isKeyValue]
// Stub: returns undefined (V8-specific API).
// ---------------------------------------------------------------------------

static napi_value previewEntries(napi_env env, napi_callback_info info) {
  napi_value undef;
  napi_get_undefined(env, &undef);
  return undef;
}

// ---------------------------------------------------------------------------
// getCallSites(frames) -> array of callsite objects
// Stub: returns empty array (V8-specific stack introspection).
// ---------------------------------------------------------------------------

static napi_value getCallSites(napi_env env, napi_callback_info info) {
  napi_value result;
  napi_create_array_with_length(env, 0, &result);
  return result;
}

// ---------------------------------------------------------------------------
// sleep(msec)
// ---------------------------------------------------------------------------

static napi_value utilSleep(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv;
  napi_get_cb_info(env, info, &argc, &argv, nullptr, nullptr);

  uint32_t msec = 0;
  if (argc > 0)
    napi_get_value_uint32(env, argv, &msec);

  uv_sleep(msec);

  napi_value undef;
  napi_get_undefined(env, &undef);
  return undef;
}

// ---------------------------------------------------------------------------
// guessHandleType(fd) -> integer code
// Returns an integer index matching Node's handle type array:
//   0=TCP, 1=TTY, 2=UDP, 3=FILE, 4=PIPE, 5=UNKNOWN
// ---------------------------------------------------------------------------

static napi_value guessHandleType(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv;
  napi_get_cb_info(env, info, &argc, &argv, nullptr, nullptr);

  int32_t fd = 0;
  if (argc > 0)
    napi_get_value_int32(env, argv, &fd);

  if (fd < 0) {
    napi_throw_range_error(env, nullptr, "fd must be non-negative");
    return nullptr;
  }

  uv_handle_type t = uv_guess_handle(fd);
  uint32_t code;
  switch (t) {
    case UV_TCP:
      code = 0;
      break;
    case UV_TTY:
      code = 1;
      break;
    case UV_UDP:
      code = 2;
      break;
    case UV_FILE:
      code = 3;
      break;
    case UV_NAMED_PIPE:
      code = 4;
      break;
    default:
      code = 5;
      break;
  }

  napi_value result;
  napi_create_uint32(env, code, &result);
  return result;
}

// ---------------------------------------------------------------------------
// getExternalValue(external) -> BigInt
// Stub: returns 0n (External values are V8-specific).
// ---------------------------------------------------------------------------

static napi_value getExternalValue(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv;
  napi_get_cb_info(env, info, &argc, &argv, nullptr, nullptr);

  void *ptr = nullptr;
  if (argc > 0)
    napi_get_value_external(env, argv, &ptr);

  uint64_t value = reinterpret_cast<uint64_t>(ptr);
  napi_value result;
  napi_create_bigint_uint64(env, value, &result);
  return result;
}

// ---------------------------------------------------------------------------
// arrayBufferViewHasBuffer(view) -> boolean
// ---------------------------------------------------------------------------

static napi_value
arrayBufferViewHasBuffer(napi_env env, napi_callback_info info) {
  // In Hermes, typed arrays always have a backing buffer.
  napi_value result;
  napi_get_boolean(env, true, &result);
  return result;
}

// ---------------------------------------------------------------------------
// isInsideNodeModules(frameLimit, defaultValue) -> boolean
// Stub: always returns the default value (second arg or false).
// ---------------------------------------------------------------------------

static napi_value isInsideNodeModules(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  // Return the default value (second argument) if provided, else false.
  if (argc >= 2) {
    return argv[1];
  }

  napi_value result;
  napi_get_boolean(env, false, &result);
  return result;
}

// ---------------------------------------------------------------------------
// defineLazyProperties(target, id, keys[, enumerable])
// Defines lazy getter properties on target that require(id)[key] on access.
// ---------------------------------------------------------------------------

/// Data stored for each lazy property getter.
struct LazyPropData {
  napi_ref requireRef; // Reference to the global require function
  char *moduleId;      // The module ID string (owned)
  char *propKey;       // The property key string (owned)

  ~LazyPropData() {
    delete[] moduleId;
    delete[] propKey;
  }
};

/// Release the reference and free the data when the lazy property object is
/// garbage collected.
static void lazyPropDataCleanup(napi_env env, void *data, void * /*hint*/) {
  auto *lpd = static_cast<LazyPropData *>(data);
  if (lpd->requireRef)
    napi_delete_reference(env, lpd->requireRef);
  delete lpd;
}

/// Getter callback for lazy properties.
static napi_value lazyPropGetter(napi_env env, napi_callback_info info) {
  void *data;
  napi_value thisArg;
  napi_get_cb_info(env, info, nullptr, nullptr, &thisArg, &data);
  auto *lpd = static_cast<LazyPropData *>(data);

  // Get the require function from global scope.
  napi_value global;
  napi_get_global(env, &global);

  // Use globalThis.require or the module loader's require if available.
  // In our setup, require is exposed via the module loader on globalThis.
  napi_value requireFn;
  if (lpd->requireRef) {
    napi_get_reference_value(env, lpd->requireRef, &requireFn);
  } else {
    // Fallback: look for require on globalThis.
    napi_get_named_property(env, global, "require", &requireFn);
  }

  // Call require(moduleId).
  napi_value moduleIdStr;
  napi_create_string_utf8(
      env, lpd->moduleId, NAPI_AUTO_LENGTH, &moduleIdStr);

  napi_value modExports;
  napi_status st = napi_call_function(
      env, global, requireFn, 1, &moduleIdStr, &modExports);
  if (st != napi_ok)
    return nullptr;

  // Get the property from the module exports.
  napi_value propKeyStr;
  napi_create_string_utf8(env, lpd->propKey, NAPI_AUTO_LENGTH, &propKeyStr);

  napi_value result;
  napi_get_property(env, modExports, propKeyStr, &result);
  return result;
}

static napi_value defineLazyProperties(napi_env env, napi_callback_info info) {
  size_t argc = 4;
  napi_value argv[4];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  if (argc < 3) {
    napi_throw_error(env, nullptr, "defineLazyProperties requires 3+ args");
    return nullptr;
  }

  napi_value target = argv[0];
  napi_value id = argv[1];
  napi_value keys = argv[2];

  // Get the module ID string.
  char moduleId[256];
  size_t moduleIdLen = 0;
  napi_get_value_string_utf8(env, id, moduleId, sizeof(moduleId), &moduleIdLen);

  // Check enumerable flag (default true).
  bool enumerable = true;
  if (argc >= 4) {
    napi_valuetype t;
    napi_typeof(env, argv[3], &t);
    if (t == napi_boolean)
      napi_get_value_bool(env, argv[3], &enumerable);
  }

  // Get the keys array length.
  uint32_t keysLen = 0;
  napi_get_array_length(env, keys, &keysLen);

  // Get the global require function from globalThis.
  napi_value global;
  napi_get_global(env, &global);
  napi_value requireFn;
  napi_get_named_property(env, global, "require", &requireFn);

  napi_ref requireRef = nullptr;
  napi_valuetype reqType;
  napi_typeof(env, requireFn, &reqType);
  if (reqType == napi_function) {
    napi_create_reference(env, requireFn, 1, &requireRef);
  }

  for (uint32_t i = 0; i < keysLen; ++i) {
    napi_value keyVal;
    napi_get_element(env, keys, i, &keyVal);

    char keyBuf[256];
    size_t keyLen = 0;
    napi_get_value_string_utf8(env, keyVal, keyBuf, sizeof(keyBuf), &keyLen);

    // Create data for this lazy property.
    auto *lpd = new LazyPropData();
    lpd->moduleId = new char[moduleIdLen + 1];
    std::memcpy(lpd->moduleId, moduleId, moduleIdLen + 1);
    lpd->propKey = new char[keyLen + 1];
    std::memcpy(lpd->propKey, keyBuf, keyLen + 1);

    if (requireRef) {
      napi_ref ref;
      napi_value refVal;
      napi_get_reference_value(env, requireRef, &refVal);
      napi_create_reference(env, refVal, 1, &ref);
      lpd->requireRef = ref;
    } else {
      lpd->requireRef = nullptr;
    }

    // Attach lpd cleanup to the target object (not a separate getter function).
    // The target outlives any accessor getters defined on it, so the data
    // won't be freed while the getter is still reachable.
    napi_add_finalizer(
        env, target, lpd, lazyPropDataCleanup, nullptr, nullptr);

    // Define the lazy property as a configurable getter.
    napi_property_descriptor desc = {};
    desc.utf8name = keyBuf;
    desc.getter = lazyPropGetter;
    desc.data = lpd;
    desc.attributes = napi_configurable;
    if (enumerable)
      desc.attributes =
          static_cast<napi_property_attributes>(desc.attributes | napi_enumerable);

    napi_define_properties(env, target, 1, &desc);
  }

  // Release our own copy of the require reference.
  if (requireRef)
    napi_delete_reference(env, requireRef);

  napi_value undef;
  napi_get_undefined(env, &undef);
  return undef;
}

// ---------------------------------------------------------------------------
// constructSharedArrayBuffer(length) -> SharedArrayBuffer
// Stub: throws since Hermes may not support SABs.
// ---------------------------------------------------------------------------

static napi_value
constructSharedArrayBuffer(napi_env env, napi_callback_info info) {
  // Try to construct via JS.
  size_t argc = 1;
  napi_value argv;
  napi_get_cb_info(env, info, &argc, &argv, nullptr, nullptr);

  napi_value global;
  napi_get_global(env, &global);

  napi_value sabCtor;
  napi_get_named_property(env, global, "SharedArrayBuffer", &sabCtor);

  napi_valuetype ctorType;
  napi_typeof(env, sabCtor, &ctorType);
  if (ctorType != napi_function) {
    napi_throw_error(
        env, nullptr, "SharedArrayBuffer is not available in this environment");
    return nullptr;
  }

  napi_value result;
  napi_status st = napi_new_instance(env, sabCtor, argc > 0 ? 1 : 0,
                                     argc > 0 ? &argv : nullptr, &result);
  if (st != napi_ok)
    return nullptr;

  return result;
}

// ---------------------------------------------------------------------------
// parseEnv(content) -> object
// Stub: returns empty object (requires dotenv parser).
// ---------------------------------------------------------------------------

static napi_value parseEnv(napi_env env, napi_callback_info info) {
  napi_value result;
  napi_create_object(env, &result);
  return result;
}

// ---------------------------------------------------------------------------
// privateSymbols
// ---------------------------------------------------------------------------

/// Create the privateSymbols object with all private symbols used by Node
/// internals.
static napi_status
createPrivateSymbols(napi_env env, napi_value *result) {
  napi_status st = napi_create_object(env, result);
  if (st != napi_ok)
    return st;

  // Each private symbol is created as a unique Symbol with a descriptive name.
  // Node uses V8 Private symbols which are not accessible from JS. We use
  // regular Symbols which are accessible but serve the same purpose in our
  // controlled environment.
  struct SymbolDef {
    const char *name;
    const char *description;
  };

  static const SymbolDef symbols[] = {
      {"arrow_message_private_symbol", "node:arrowMessage"},
      {"contextify_context_private_symbol", "node:contextify:context"},
      {"decorated_private_symbol", "node:decorated"},
      {"transfer_mode_private_symbol", "node:transfer_mode"},
      {"host_defined_option_symbol", "node:host_defined_option_symbol"},
      {"js_transferable_wrapper_private_symbol",
       "node:js_transferable_wrapper"},
      {"entry_point_module_private_symbol", "node:entry_point_module"},
      {"entry_point_promise_private_symbol", "node:entry_point_promise"},
      {"module_source_private_symbol", "node:module_source"},
      {"module_export_names_private_symbol", "node:module_export_names"},
      {"module_circular_visited_private_symbol",
       "node:module_circular_visited"},
      {"module_export_private_symbol", "node:module_export"},
      {"module_first_parent_private_symbol", "node:module_first_parent"},
      {"module_last_parent_private_symbol", "node:module_last_parent"},
      {"napi_type_tag", "node:napi:type_tag"},
      {"napi_wrapper", "node:napi:wrapper"},
      {"untransferable_object_private_symbol", "node:untransferableObject"},
      {"exit_info_private_symbol", "node:exit_info_private_symbol"},
      {"promise_trace_id", "node:promise_trace_id"},
      {"source_map_data_private_symbol",
       "node:source_map_data_private_symbol"},
  };

  for (const auto &sym : symbols) {
    napi_value desc;
    st = napi_create_string_utf8(
        env, sym.description, NAPI_AUTO_LENGTH, &desc);
    if (st != napi_ok)
      return st;

    napi_value symbol;
    st = napi_create_symbol(env, desc, &symbol);
    if (st != napi_ok)
      return st;

    st = napi_set_named_property(env, *result, sym.name, symbol);
    if (st != napi_ok)
      return st;
  }

  return napi_ok;
}

// ---------------------------------------------------------------------------
// constants
// ---------------------------------------------------------------------------

/// Create the constants sub-object for the util binding.
static napi_status createConstants(napi_env env, napi_value *result) {
  napi_status st = napi_create_object(env, result);
  if (st != napi_ok)
    return st;

  // Promise state constants.
  setInt32Property(env, *result, "kPending", 0);
  setInt32Property(env, *result, "kFulfilled", 1);
  setInt32Property(env, *result, "kRejected", 2);

  // ExitInfo field constants.
  setInt32Property(env, *result, "kExiting", 0);
  setInt32Property(env, *result, "kExitCode", 1);
  setInt32Property(env, *result, "kHasExitCode", 2);

  // PropertyFilter constants (match V8's enum values).
  setInt32Property(env, *result, "ALL_PROPERTIES", ALL_PROPERTIES);
  setInt32Property(env, *result, "ONLY_WRITABLE", ONLY_WRITABLE);
  setInt32Property(env, *result, "ONLY_ENUMERABLE", ONLY_ENUMERABLE);
  setInt32Property(env, *result, "ONLY_CONFIGURABLE", ONLY_CONFIGURABLE);
  setInt32Property(env, *result, "SKIP_STRINGS", SKIP_STRINGS);
  setInt32Property(env, *result, "SKIP_SYMBOLS", SKIP_SYMBOLS);

  // TransferMode constants.
  setInt32Property(env, *result, "kDisallowCloneAndTransfer", 0);
  setInt32Property(env, *result, "kTransferable", 1);
  setInt32Property(env, *result, "kCloneable", 2);

  return napi_ok;
}

// ---------------------------------------------------------------------------
// shouldAbortOnUncaughtToggle
// A Uint32Array(1) that JS reads to decide abort-on-uncaught behavior.
// ---------------------------------------------------------------------------

static napi_status
createShouldAbortOnUncaughtToggle(napi_env env, napi_value *result) {
  // Create a Uint32Array of length 1, initialized to 0 (don't abort).
  napi_value ab;
  void *data;
  napi_status st =
      napi_create_arraybuffer(env, sizeof(uint32_t), &data, &ab);
  if (st != napi_ok)
    return st;

  // Initialize to 0.
  *static_cast<uint32_t *>(data) = 0;

  return napi_create_typedarray(env, napi_uint32_array, 1, ab, 0, result);
}

// ---------------------------------------------------------------------------
// initUtilBinding
// ---------------------------------------------------------------------------

napi_value initUtilBinding(napi_env env, napi_value exports) {
  // Functions.
  napi_property_descriptor props[] = {
      {"getOwnNonIndexProperties",
       nullptr,
       getOwnNonIndexProperties,
       nullptr,
       nullptr,
       nullptr,
       napi_default,
       nullptr},
      {"getConstructorName",
       nullptr,
       getConstructorName,
       nullptr,
       nullptr,
       nullptr,
       napi_default,
       nullptr},
      {"getPromiseDetails",
       nullptr,
       getPromiseDetails,
       nullptr,
       nullptr,
       nullptr,
       napi_default,
       nullptr},
      {"getProxyDetails",
       nullptr,
       getProxyDetails,
       nullptr,
       nullptr,
       nullptr,
       napi_default,
       nullptr},
      {"getCallerLocation",
       nullptr,
       getCallerLocation,
       nullptr,
       nullptr,
       nullptr,
       napi_default,
       nullptr},
      {"previewEntries",
       nullptr,
       previewEntries,
       nullptr,
       nullptr,
       nullptr,
       napi_default,
       nullptr},
      {"getCallSites",
       nullptr,
       getCallSites,
       nullptr,
       nullptr,
       nullptr,
       napi_default,
       nullptr},
      {"getExternalValue",
       nullptr,
       getExternalValue,
       nullptr,
       nullptr,
       nullptr,
       napi_default,
       nullptr},
      {"sleep",
       nullptr,
       utilSleep,
       nullptr,
       nullptr,
       nullptr,
       napi_default,
       nullptr},
      {"guessHandleType",
       nullptr,
       guessHandleType,
       nullptr,
       nullptr,
       nullptr,
       napi_default,
       nullptr},
      {"arrayBufferViewHasBuffer",
       nullptr,
       arrayBufferViewHasBuffer,
       nullptr,
       nullptr,
       nullptr,
       napi_default,
       nullptr},
      {"isInsideNodeModules",
       nullptr,
       isInsideNodeModules,
       nullptr,
       nullptr,
       nullptr,
       napi_default,
       nullptr},
      {"defineLazyProperties",
       nullptr,
       defineLazyProperties,
       nullptr,
       nullptr,
       nullptr,
       napi_default,
       nullptr},
      {"constructSharedArrayBuffer",
       nullptr,
       constructSharedArrayBuffer,
       nullptr,
       nullptr,
       nullptr,
       napi_default,
       nullptr},
      {"parseEnv",
       nullptr,
       parseEnv,
       nullptr,
       nullptr,
       nullptr,
       napi_default,
       nullptr},
  };

  napi_define_properties(
      env, exports, sizeof(props) / sizeof(props[0]), props);

  // privateSymbols sub-object.
  napi_value privateSymbols;
  createPrivateSymbols(env, &privateSymbols);
  napi_set_named_property(env, exports, "privateSymbols", privateSymbols);

  // constants sub-object.
  napi_value constants;
  createConstants(env, &constants);
  napi_set_named_property(env, exports, "constants", constants);

  // shouldAbortOnUncaughtToggle typed array.
  napi_value toggle;
  createShouldAbortOnUncaughtToggle(env, &toggle);
  napi_set_named_property(
      env, exports, "shouldAbortOnUncaughtToggle", toggle);

  return exports;
}

} // namespace node_compat
} // namespace hermes
