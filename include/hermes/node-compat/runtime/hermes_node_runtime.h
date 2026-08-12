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

  /// Modules to require before the script, eval code, or REPL runs, in the
  /// order given. Resolved like require() from the current directory.
  std::vector<std::string> requireModules;

  /// Override process.version. Empty = use default.
  std::string nodeVersion;

  /// Start the REPL when no scriptPath and no evalCode are provided.
  bool enableRepl = false;

  /// Enable the inspector (CDP debugger).
  bool inspect = false;

  /// Pause before executing the first user statement (implies inspect).
  bool inspectBrk = false;

  /// Inspector host address.
  std::string inspectHost = "127.0.0.1";

  /// Inspector port (0 = OS-assigned).
  int inspectPort = 9229;

  /// Open the DevTools URL in the system browser once the inspector is
  /// listening (implies inspect).
  bool inspectOpen = false;

  /// Compile cache root directory. Empty = use the default XDG location.
  std::string compileCacheDir;

  /// Disable the on-disk compile cache entirely.
  bool disableCompileCache = false;

  /// Opaque pointer to the inspector bridge context for cross-thread CDP
  /// messaging. Null for the user runtime (normal operation). Set when this
  /// runtime is the inspector runtime running on the IO thread.
  void *inspectorBridgeContext = nullptr;
};

/// Run a complete hermes-node instance. Blocks until the event loop exits.
/// Thread-safe: can be called from any thread; each invocation is fully
/// independent (own runtime, event loop, bindings state).
/// Returns the process exit code.
int runHermesNode(const HermesNodeConfig &config);

} // namespace node_compat
} // namespace hermes
