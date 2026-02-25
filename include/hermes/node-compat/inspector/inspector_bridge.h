/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_NODE_COMPAT_INSPECTOR_INSPECTOR_BRIDGE_H
#define HERMES_NODE_COMPAT_INSPECTOR_INSPECTOR_BRIDGE_H

#include <node_api.h>
#include <uv.h>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <string>

namespace hermes {
namespace node_compat {

/// Cross-thread context for CDP message passing between the main runtime and
/// the inspector runtime. Allocated by the main runtime (Step 10), passed into
/// the inspector runtime via HermesNodeConfig::inspectorBridgeContext.
struct InspectorBridgeContext {
  // --- Main -> Inspector (outbound CDP messages) ---
  std::mutex outboundMutex;
  std::queue<std::string> outboundQueue;
  /// Async handle in the inspector's event loop. Set by the inspector binding
  /// during init. Used to wake the inspector loop when outbound messages arrive.
  uv_async_t *inspectorAsync = nullptr;

  // --- Inspector -> Main (inbound CDP commands) ---
  /// Points to InspectorState::mutex on the main thread.
  std::mutex *inboundMutex = nullptr;
  /// Points to InspectorState::inboundCommands on the main thread.
  std::queue<std::string> *inboundQueue = nullptr;
  /// Async handle in the main event loop. Used to wake the main loop.
  uv_async_t *mainAsync = nullptr;
  /// Guard for mainAsync: only send if the main async handle is active.
  std::atomic<bool> *mainAsyncActive = nullptr;

  // --- Config ---
  std::string host;
  int port = 9229;
  std::string scriptName;
  std::string sessionId;

  // --- Startup synchronization ---
  std::mutex readyMutex;
  std::condition_variable readyCv;
  bool ready = false;
  /// Actual port after bind (useful when port 0 was requested).
  int actualPort = 0;

  // --- Inspector-side JS callback state (set during binding init) ---
  napi_ref messageCallbackRef = nullptr;
  napi_env inspectorEnv = nullptr;
};

/// Init function for the 'inspector_bridge' binding.
/// When inspectorBridgeContext is null (normal runtime), exports an empty
/// object. When set (inspector runtime), exposes sendToMain,
/// setMessageCallback, getConfig, notifyReady.
napi_value initInspectorBridgeBinding(napi_env env, napi_value exports);

} // namespace node_compat
} // namespace hermes

#endif // HERMES_NODE_COMPAT_INSPECTOR_INSPECTOR_BRIDGE_H
