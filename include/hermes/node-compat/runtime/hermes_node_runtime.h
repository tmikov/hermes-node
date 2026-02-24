/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <string>
#include <vector>

namespace hermes {
namespace node_compat {

/// Configuration for a hermes-node runtime instance.
struct HermesNodeConfig {
  /// Script file to execute. Empty = no script file.
  std::string scriptPath;

  /// Inline JS code to eval after bootstrap, before event loop.
  /// Useful for programmatic use (e.g. inspector runtime).
  std::string evalCode;

  /// process.argv values. First element should be the binary name.
  std::vector<std::string> argv;

  /// Override process.version. Empty = use default.
  std::string nodeVersion;

  /// Start the REPL when no scriptPath and no evalCode are provided.
  bool enableRepl = false;
};

/// Run a complete hermes-node instance. Blocks until the event loop exits.
/// Thread-safe: can be called from any thread; each invocation is fully
/// independent (own runtime, event loop, bindings state).
/// Returns the process exit code.
int runHermesNode(const HermesNodeConfig &config);

} // namespace node_compat
} // namespace hermes
