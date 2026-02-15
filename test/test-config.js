// Copyright (c) Tzvetan Mikov.
// Test the config binding.
'use strict';

var cfg = internalBinding('config');

// Helper
function assert(cond, msg) {
  if (!cond) throw new Error('Assertion failed: ' + msg);
}
function assertEqual(a, b, msg) {
  if (a !== b) throw new Error('Assertion failed: ' + msg + ' (' + a + ' !== ' + b + ')');
}

// --- Boolean feature flags ---
assertEqual(typeof cfg.hasOpenSSL, 'boolean', 'hasOpenSSL is boolean');
assertEqual(cfg.hasOpenSSL, false, 'hasOpenSSL is false');

assertEqual(typeof cfg.openSSLIsBoringSSL, 'boolean', 'openSSLIsBoringSSL is boolean');
assertEqual(cfg.openSSLIsBoringSSL, false, 'openSSLIsBoringSSL is false');

assertEqual(typeof cfg.fipsMode, 'boolean', 'fipsMode is boolean');
assertEqual(cfg.fipsMode, false, 'fipsMode is false');

assertEqual(typeof cfg.hasIntl, 'boolean', 'hasIntl is boolean');
assertEqual(cfg.hasIntl, false, 'hasIntl is false');

assertEqual(typeof cfg.hasSmallICU, 'boolean', 'hasSmallICU is boolean');
assertEqual(cfg.hasSmallICU, false, 'hasSmallICU is false');

assertEqual(typeof cfg.hasTracing, 'boolean', 'hasTracing is boolean');
assertEqual(cfg.hasTracing, false, 'hasTracing is false');

assertEqual(typeof cfg.hasNodeOptions, 'boolean', 'hasNodeOptions is boolean');
assertEqual(cfg.hasNodeOptions, true, 'hasNodeOptions is true');

assertEqual(typeof cfg.hasInspector, 'boolean', 'hasInspector is boolean');
assertEqual(cfg.hasInspector, false, 'hasInspector is false');

assertEqual(typeof cfg.noBrowserGlobals, 'boolean', 'noBrowserGlobals is boolean');
assertEqual(cfg.noBrowserGlobals, false, 'noBrowserGlobals is false');

assertEqual(typeof cfg.isDebugBuild, 'boolean', 'isDebugBuild is boolean');

// --- bits ---
assertEqual(typeof cfg.bits, 'number', 'bits is number');
assert(cfg.bits === 32 || cfg.bits === 64, 'bits is 32 or 64');

// --- getDefaultLocale ---
assertEqual(typeof cfg.getDefaultLocale, 'function', 'getDefaultLocale is function');
var locale = cfg.getDefaultLocale();
assertEqual(typeof locale, 'string', 'getDefaultLocale returns string');
assert(locale.length > 0, 'getDefaultLocale returns non-empty string');

console.log('PASS');
