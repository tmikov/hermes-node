/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_NODE_COMPAT_NODE_PROCESS_H
#define HERMES_NODE_COMPAT_NODE_PROCESS_H

#include <node_api_types.h>

#include <string>
#include <vector>

namespace hermes {
namespace node_compat {

/// Creates the `process` global object with basic (non-I/O) properties and
/// methods. The resulting object can be set as a global or passed to the
/// module loader.
///
/// Properties set:
///   pid, ppid, platform, arch, version, versions, argv, execPath, title, env
///
/// Methods set:
///   cwd(), chdir(), hrtime(), hrtime.bigint(), cpuUsage(), memoryUsage(),
///   uptime(), exit(), abort(), umask()
///
/// Properties/methods NOT set here (need event loop / streams):
///   nextTick, stdout, stderr, stdin, signal handling, on('exit')
class NodeProcess {
 public:
  NodeProcess();
  ~NodeProcess();

  NodeProcess(const NodeProcess &) = delete;
  NodeProcess &operator=(const NodeProcess &) = delete;

  /// Set command-line arguments. Must be called before create().
  void setArgv(std::vector<std::string> argv);

  /// Set the executable path. Must be called before create().
  void setExecPath(std::string execPath);

  /// Create the process object and return it.
  /// The object is also cached internally and can be retrieved via get().
  napi_status create(napi_env env, napi_value *result);

  /// Detach from the napi_env, releasing cached references.
  /// Must be called before destroying the env.
  void detach(napi_env env);

  /// Returns the process start time in nanoseconds (from uv_hrtime epoch).
  uint64_t getStartTime() const {
    return startTime_;
  }

 private:
  std::vector<std::string> argv_;
  std::string execPath_;
  napi_ref processRef_ = nullptr;
  uint64_t startTime_ = 0; // nanoseconds, for uptime()
};

} // namespace node_compat
} // namespace hermes

#endif // HERMES_NODE_COMPAT_NODE_PROCESS_H
