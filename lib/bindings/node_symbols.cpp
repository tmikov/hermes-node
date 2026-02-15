/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <hermes/node-compat/bindings/node_symbols.h>
#include <node_api.h>

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

/// Create a symbol with the given description and set it as a property on obj.
static void setSymbolProp(
    napi_env env,
    napi_value obj,
    const char *name,
    const char *description) {
  napi_value descStr;
  napi_create_string_utf8(env, description, NAPI_AUTO_LENGTH, &descStr);
  napi_value sym;
  napi_create_symbol(env, descStr, &sym);
  napi_set_named_property(env, obj, name, sym);
}

// List of symbols from Node.js PER_ISOLATE_SYMBOL_PROPERTIES
// (src/env_properties.h). Each entry is (property_name, description_string).
#define SYMBOL_PROPERTIES(V)                                                  \
  V(fs_use_promises_symbol, "fs_use_promises_symbol")                         \
  V(async_id_symbol, "async_id_symbol")                                       \
  V(constructor_key_symbol, "constructor_key_symbol")                         \
  V(handle_onclose_symbol, "handle_onclose")                                  \
  V(no_message_symbol, "no_message_symbol")                                   \
  V(messaging_deserialize_symbol, "messaging_deserialize_symbol")             \
  V(imported_cjs_symbol, "imported_cjs_symbol")                               \
  V(messaging_transfer_symbol, "messaging_transfer_symbol")                   \
  V(messaging_clone_symbol, "messaging_clone_symbol")                         \
  V(messaging_transfer_list_symbol, "messaging_transfer_list_symbol")         \
  V(oninit_symbol, "oninit")                                                  \
  V(owner_symbol, "owner_symbol")                                             \
  V(onpskexchange_symbol, "onpskexchange")                                    \
  V(resource_symbol, "resource_symbol")                                       \
  V(trigger_async_id_symbol, "trigger_async_id_symbol")                       \
  V(source_text_module_default_hdo, "source_text_module_default_hdo")         \
  V(vm_context_no_contextify, "vm_context_no_contextify")                     \
  V(vm_dynamic_import_default_internal, "vm_dynamic_import_default_internal") \
  V(vm_dynamic_import_main_context_default,                                   \
    "vm_dynamic_import_main_context_default")                                 \
  V(vm_dynamic_import_missing_flag, "vm_dynamic_import_missing_flag")         \
  V(vm_dynamic_import_no_callback, "vm_dynamic_import_no_callback")

napi_value initSymbolsBinding(napi_env env, napi_value exports) {
#define SET_SYMBOL(name, desc) setSymbolProp(env, exports, #name, desc);
  SYMBOL_PROPERTIES(SET_SYMBOL)
#undef SET_SYMBOL

  return exports;
}

#undef NAPI_CALL

} // namespace node_compat
} // namespace hermes
