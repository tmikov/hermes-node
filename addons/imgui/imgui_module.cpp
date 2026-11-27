/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// The addon's N-API surface and its tracing. Loaded by hermes-node-ui as
// `hermes-node:imgui` (tools/hermes-node-ui/named_addons.cpp), from
// hermes-node-imgui.node beside the binary.

#include "imgui_addon.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace hnimgui {

namespace {

/// The host's rule for UI tracing (lib/compositor-gpu/ui_trace.cpp,
/// uiTracingEnabled), copied rather than shared: the addon includes no host
/// header. Read once; several threads may trace.
bool tracingEnabled() {
  static const bool enabled = [] {
    const char *dbg = std::getenv("HERMES_NODE_DEBUG_NATIVE");
    return dbg != nullptr && std::strstr(dbg, "UI") != nullptr;
  }();
  return enabled;
}

} // namespace

void trace(const char *fmt, ...) {
  if (!tracingEnabled())
    return;
  static const char kPrefix[] = "[ui] imgui: ";
  const size_t prefixLen = sizeof(kPrefix) - 1;

  va_list args;
  va_start(args, fmt);
  va_list measure;
  va_copy(measure, args);
  const int bodyLen = std::vsnprintf(nullptr, 0, fmt, measure);
  va_end(measure);
  if (bodyLen < 0) {
    va_end(args);
    std::fprintf(stderr, "%s<unformattable trace line> %s\n", kPrefix, fmt);
    std::fflush(stderr);
    return;
  }
  // The whole line in one buffer and one fwrite, so lines from different
  // threads cannot interleave (the host's ui_trace.cpp explains why one
  // write matters). On the stack unless the line is long.
  char stackBuf[512];
  const size_t needed = prefixLen + static_cast<size_t>(bodyLen) + 2;
  char *line = needed <= sizeof(stackBuf)
      ? stackBuf
      : static_cast<char *>(std::malloc(needed));
  if (line == nullptr) {
    va_end(args);
    return;
  }
  std::memcpy(line, kPrefix, prefixLen);
  std::vsnprintf(line + prefixLen, bodyLen + 1, fmt, args);
  va_end(args);
  line[prefixLen + bodyLen] = '\n';
  std::fwrite(line, 1, prefixLen + bodyLen + 1, stderr);
  std::fflush(stderr);
  if (line != stackBuf)
    std::free(line);
}

bool testHooksEnabled() {
  // The host's rule (lib/compositor-gpu/test_hooks.cpp,
  // uiTestHooksEnabledFor): unset, empty or exactly "0" is off, anything
  // else on. Read once.
  static const bool enabled = [] {
    const char *value = std::getenv("HERMES_NODE_UI_TEST_HOOKS");
    return value != nullptr && value[0] != '\0' &&
        std::strcmp(value, "0") != 0;
  }();
  return enabled;
}

} // namespace hnimgui

NAPI_MODULE_INIT() {
  // First, before anything touches the host: an addon built against a
  // different hnui ABI or Sokol must fail to load rather than run.
  HNUI_CHECK_VERSION(env);

  napi_value version = nullptr;
  if (napi_create_string_utf8(env, IMGUI_VERSION, NAPI_AUTO_LENGTH, &version) !=
          napi_ok ||
      napi_set_named_property(env, exports, "version", version) != napi_ok)
    return nullptr;

  hnimgui::trace("addon loaded (imgui %s)", IMGUI_VERSION);
  return exports;
}
