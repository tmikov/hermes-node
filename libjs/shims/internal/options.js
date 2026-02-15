// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// Shim for internal/options.
//
// Replaces Node's internal/options module which reads from the C++ options
// parser (internalBinding('options')). We provide a static map of default
// values for all options queried by lib/*.js during bootstrap.

'use strict';

// Default values for CLI options queried by Node's internal modules.
// Boolean flags default to false, string flags to '', array flags to [].
// Values here reflect Node v24 defaults for the options we care about.
var optionsMap = {
  // Deprecation control
  '--pending-deprecation': false,
  '--no-deprecation': false,

  // Error handling
  '--abort-on-uncaught-exception': false,
  '--unhandled-rejections': 'throw-or-warn',

  // Module system
  '--experimental-require-module': true,
  '--experimental-detect-module': true,
  '--experimental-vm-modules': false,
  '--experimental-loader': [],
  '--experimental-addon-modules': false,
  '--experimental-print-required-tla': false,
  '--conditions': [],
  '--no-addons': false,
  '--preserve-symlinks': false,
  '--preserve-symlinks-main': false,
  '--input-type': '',
  '--entry-url': false,
  '--trace-require-module': '',

  // TypeScript
  '--strip-types': false,
  '--experimental-transform-types': false,

  // Warnings and diagnostics
  '--warnings': true,
  '--diagnostic-dir': '',
  '--redirect-warnings': '',
  '--disable-warning': [],
  '--enable-source-maps': false,
  '--trace-sigint': false,
  '--trace-tls': false,

  // Inspector
  '--inspect-brk': false,
  '--experimental-network-inspection': false,

  // Security
  '--permission': '',
  '--frozen-intrinsics': false,
  '--expose-internals': false,

  // HTTP
  '--max-http-header-size': 16384,
  '--insecure-http-parser': false,
  '--use-env-proxy': false,

  // Networking
  '--network-family-autoselection': true,
  '--network-family-autoselection-attempt-timeout': 250,
  '--dns-result-order': 'verbatim',

  // TLS (all no-ops without OpenSSL)
  '--tls-cipher-list': '',
  '--tls-keylog': '',
  '--tls-min-v1.0': false,
  '--tls-min-v1.1': false,
  '--tls-min-v1.2': false,
  '--tls-min-v1.3': false,
  '--tls-max-v1.2': false,
  '--tls-max-v1.3': false,
  '--use-openssl-ca': false,
  '--use-system-ca': false,
  '--force-fips': false,
  '--secure-heap': 0,
  '--secure-heap-min': 0,

  // Experimental features (mostly disabled)
  '--no-experimental-websocket': false,
  '--experimental-eventsource': false,
  '--no-experimental-global-navigator': false,
  '--no-experimental-sqlite': true,
  '--experimental-quic': false,
  '--experimental-webstorage': false,
  '--experimental-config-file': '',
  '--experimental-default-config-file': false,
  '--experimental-test-coverage': false,
  '--experimental-test-module-mocks': false,
  '--experimental-import-meta-resolve': false,
  '--experimental-inspector-network-resource': false,

  // Preload
  '--require': [],
  '--import': [],

  // Reporting
  '--report-on-signal': false,
  '--heapsnapshot-signal': '',
  '--heapsnapshot-near-heap-limit': 0,

  // REPL
  '--experimental-repl-await': true,

  // V8
  '--stack-trace-limit': 10,

  // Watch mode
  '--watch': false,
  '--watch-path': [],
  '--watch-preserve-output': false,
  '--watch-kill-signal': 'SIGTERM',

  // Test runner
  '--test': false,
  '--test-only': false,
  '--test-force-exit': false,
  '--test-update-snapshots': false,
  '--test-timeout': 0,
  '--test-rerun-failures': '',
  '--test-reporter': [],
  '--test-reporter-destination': [],
  '--test-global-setup': '',
  '--test-isolation': 'process',
  '--test-concurrency': 0,
  '--test-shard': '',
  '--test-name-pattern': [],
  '--test-skip-pattern': [],
  '--test-coverage-exclude': [],
  '--test-coverage-include': [],
  '--test-coverage-branches': 0,
  '--test-coverage-lines': 0,
  '--test-coverage-functions': 0,

  // Env files
  '--env-file': [],
  '--env-file-if-exists': [],

  // Misc
  '--eval': '',
  '--print': false,
  '--async-context-frame': false,
  '--localstorage-file': '',
  '[has_eval_string]': false,
};

function getOptionValue(optionName) {
  var val = optionsMap[optionName];
  return val !== undefined ? val : undefined;
}

function refreshOptions() {
  // No-op: we don't have a C++ options parser to re-read from.
}

function getEmbedderOptions() {
  return {
    noBrowserGlobals: false,
    hasEmbedderPreload: false,
    noGlobalSearchPaths: false,
  };
}

function getCLIOptionsInfo() {
  // Return empty options/aliases — only used by print_help and per_thread.
  return { options: new Map(), aliases: new Map() };
}

function getOptionsAsFlagsFromBinding() {
  // Returns the current CLI options as a flags string.
  // Only used by test_runner and watch_mode.
  return '';
}

function getAllowUnauthorized() {
  return false;
}

function generateConfigJsonSchema() {
  return {
    $schema: 'https://json-schema.org/draft/2020-12/schema',
    type: 'object',
    properties: {},
  };
}

module.exports = {
  getCLIOptionsInfo: getCLIOptionsInfo,
  getOptionValue: getOptionValue,
  getOptionsAsFlagsFromBinding: getOptionsAsFlagsFromBinding,
  getAllowUnauthorized: getAllowUnauthorized,
  getEmbedderOptions: getEmbedderOptions,
  generateConfigJsonSchema: generateConfigJsonSchema,
  refreshOptions: refreshOptions,
};
