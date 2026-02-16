// Copyright (c) Tzvetan Mikov.
// RUN: %hermes-node %s | %FileCheck %s
// CHECK: PASS
// Test the symbols binding.
'use strict';

var s = internalBinding('symbols');

// Helper
function assert(cond, msg) {
  if (!cond) throw new Error('Assertion failed: ' + msg);
}

// All expected symbol property names from PER_ISOLATE_SYMBOL_PROPERTIES.
var expectedSymbols = [
  'fs_use_promises_symbol',
  'async_id_symbol',
  'constructor_key_symbol',
  'handle_onclose_symbol',
  'no_message_symbol',
  'messaging_deserialize_symbol',
  'imported_cjs_symbol',
  'messaging_transfer_symbol',
  'messaging_clone_symbol',
  'messaging_transfer_list_symbol',
  'oninit_symbol',
  'owner_symbol',
  'onpskexchange_symbol',
  'resource_symbol',
  'trigger_async_id_symbol',
  'source_text_module_default_hdo',
  'vm_context_no_contextify',
  'vm_dynamic_import_default_internal',
  'vm_dynamic_import_main_context_default',
  'vm_dynamic_import_missing_flag',
  'vm_dynamic_import_no_callback',
];

// Verify each symbol exists and is typeof 'symbol'.
for (var i = 0; i < expectedSymbols.length; i++) {
  var name = expectedSymbols[i];
  assert(typeof s[name] === 'symbol', name + ' should be a symbol, got ' + typeof s[name]);
}

// Verify all symbols are unique.
for (var i = 0; i < expectedSymbols.length; i++) {
  for (var j = i + 1; j < expectedSymbols.length; j++) {
    assert(
      s[expectedSymbols[i]] !== s[expectedSymbols[j]],
      expectedSymbols[i] + ' should differ from ' + expectedSymbols[j]
    );
  }
}

// Verify symbols have descriptive toString.
assert(
  String(s.owner_symbol).indexOf('owner_symbol') !== -1,
  'owner_symbol description should contain "owner_symbol"'
);
assert(
  String(s.async_id_symbol).indexOf('async_id_symbol') !== -1,
  'async_id_symbol description should contain "async_id_symbol"'
);

// Verify symbols can be used as property keys.
var obj = {};
obj[s.owner_symbol] = 'test_value';
assert(obj[s.owner_symbol] === 'test_value', 'symbol usable as property key');

console.log('PASS');
