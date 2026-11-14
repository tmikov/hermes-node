/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ui_trace.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace hermes {
namespace node_compat {

bool uiTracingEnabled() {
  // Function-local static: initialised once, thread-safely, on first use.
  // Several threads reach the trace calls (the JS producer, the platform
  // thread, and whichever thread a GPU completion lands on), so a plain
  // global initialised lazily by hand would be a data race.
  static const bool enabled = [] {
    const char *dbg = ::getenv("HERMES_NODE_DEBUG_NATIVE");
    return dbg != nullptr && std::strstr(dbg, "UI") != nullptr;
  }();
  return enabled;
}

void uiTrace(const char *format, ...) {
  if (!uiTracingEnabled())
    return;
  std::fputs("[ui] ", stderr);
  va_list args;
  va_start(args, format);
  std::vfprintf(stderr, format, args);
  va_end(args);
  std::fputc('\n', stderr);
  // Flushed per line, deliberately. These lines are what the end-to-end
  // tests match on, and they interleave with the program's own stdout and
  // with lines from other threads; a buffered stderr would reorder them
  // against both, and lose the tail entirely on the _exit() paths this
  // runtime takes (CLAUDE.md, Bootstrap Sequence).
  std::fflush(stderr);
}

} // namespace node_compat
} // namespace hermes
