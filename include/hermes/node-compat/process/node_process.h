/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_NODE_COMPAT_NODE_PROCESS_H
#define HERMES_NODE_COMPAT_NODE_PROCESS_H

#include <node_api_types.h>
#include <uv.h>

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

  /// Override the reported process.version string. Must be called before
  /// create(). If not set, defaults to the bundled Node.js version.
  /// process.versions.node follows it with any leading "v" removed.
  void setVersion(std::string version);

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
  std::string version_;
  napi_ref processRef_ = nullptr;
  uint64_t startTime_ = 0; // nanoseconds, for uptime()
};

/// Records that an exit is under way, so the 'exit' event fires exactly
/// once however the process ends. The runtime's own end-of-life path emits
/// that event itself; calling this first stops process.exit(), reached from
/// an 'exit' handler, from emitting it a second time. Node fires it once,
/// and code that restores a terminal there is wrong if it runs twice.
void markProcessExiting();

/// Hands the process binding the event loop, so process.exit() can flush
/// queued stdio writes before it goes. stdout on a TTY or a pipe is a libuv
/// stream and its writes are queued, not synchronous, so _exit() discards
/// whatever has not reached the fd -- which is most of it. Without this,
/// eight console.log calls followed by process.exit() printed one line, and
/// a terminal UI restoring the screen from an 'exit' handler got a fraction
/// of the escape sequence out.
void setProcessExitLoop(uv_loop_t *loop);

/// Runs the loop until the queued writes are gone, bounded. Called on both
/// exit paths; see the note in node_process.cpp about what else this can
/// run.
void flushPendingWrites(uv_loop_t *loop);

} // namespace node_compat
} // namespace hermes

#endif // HERMES_NODE_COMPAT_NODE_PROCESS_H
