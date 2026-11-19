/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "sokol_log.h"

#include <uv.h>

#include <unistd.h>
#include <cstdio>

namespace hermes {
namespace node_compat {

void uiSokolLog(
    const char *instance,
    uint32_t logLevel,
    uint32_t logItemId,
    const char *message,
    uint32_t lineNr,
    const char *filename,
    const char *panicNote) {
  static const char *const kLevels[] = {"panic", "error", "warning", "info"};
  const char *level = logLevel < 4 ? kLevels[logLevel] : "?";
  std::fprintf(
      stderr,
      "[%s:%s] item %u%s%s (%s:%u)\n",
      instance ? instance : "sokol",
      level,
      logItemId,
      message ? ": " : "",
      message ? message : "",
      filename ? filename : "sokol",
      lineNr);
  std::fflush(stderr);

  if (logLevel != 0)
    return;

  if (panicNote != nullptr) {
    std::fprintf(stderr, "%s\n", panicNote);
    std::fflush(stderr);
  }
  uv_tty_reset_mode();
  _exit(1);
}

bool producerSokolValidates() {
  // sokol_gfx.h's rule, restated for a translation unit that sees only its
  // declarations: the implementation defines SOKOL_DEBUG unless NDEBUG is
  // set (sokol_gfx.h, "#ifndef SOKOL_DEBUG"), and this library compiles
  // that implementation with the same flags as this file.
#if defined(SOKOL_DEBUG) || !defined(NDEBUG)
  return true;
#else
  return false;
#endif
}

} // namespace node_compat
} // namespace hermes
