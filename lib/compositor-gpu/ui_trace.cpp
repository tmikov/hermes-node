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
#include <vector>

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

namespace {

/// Emit `prefix`, the formatted text and a newline as **one** fwrite, and
/// so -- stderr being unbuffered -- as one write(2).
///
/// Three threads trace (the JS thread, the platform thread and, on Metal,
/// the queue its completion handlers run on). A line built from several
/// stdio calls takes the stream lock once per call, so two threads could
/// produce "[ui] [ui] bodyAbodyB\n\n", and FileCheck lines that expect the
/// prefix beside its body then fail for a reason that has nothing to do
/// with what they check. One stdio call would already be enough against
/// other stdio users; one write(2) is what is needed against everything
/// else that owns fd 2 -- a child process, or code writing to the
/// descriptor directly -- and a single fprintf does not promise that:
/// glibc 2.39 formats an unbuffered stream through a 128-byte stage and
/// issues a write each time it fills.
void writeLineV(const char *prefix, const char *format, va_list args) {
  const size_t prefixLen = std::strlen(prefix);
  char stackBuf[512];
  std::vector<char> heapBuf;
  char *buf = stackBuf;
  size_t cap = sizeof(stackBuf);

  // Measure first. The line is never truncated: one that does not fit the
  // stack buffer is formatted again into a heap buffer of the right size.
  va_list measure;
  va_copy(measure, args);
  int bodyLen = prefixLen < cap
      ? std::vsnprintf(buf + prefixLen, cap - prefixLen, format, measure)
      : std::vsnprintf(nullptr, 0, format, measure);
  va_end(measure);
  if (bodyLen < 0) {
    // An encoding error in the format. Say so rather than print nothing:
    // the format string itself still identifies the call site.
    static const char kNote[] = "<unformattable trace line> ";
    heapBuf.assign(prefix, prefix + prefixLen);
    heapBuf.insert(heapBuf.end(), kNote, kNote + sizeof(kNote) - 1);
    heapBuf.insert(heapBuf.end(), format, format + std::strlen(format));
    heapBuf.push_back('\n');
    std::fwrite(heapBuf.data(), 1, heapBuf.size(), stderr);
    std::fflush(stderr);
    return;
  }
  // prefix + body + '\n', plus the NUL vsnprintf writes after the body.
  const size_t needed = prefixLen + static_cast<size_t>(bodyLen) + 2;
  if (needed > cap) {
    heapBuf.resize(needed);
    buf = heapBuf.data();
    cap = needed;
    std::vsnprintf(buf + prefixLen, cap - prefixLen, format, args);
  }
  std::memcpy(buf, prefix, prefixLen);
  buf[prefixLen + bodyLen] = '\n';
  std::fwrite(buf, 1, prefixLen + bodyLen + 1, stderr);
  // Flushed per line, deliberately. These lines are what the end-to-end
  // tests match on, and they interleave with the program's own stdout and
  // with lines from other threads; a buffered stderr would reorder them
  // against both, and lose the tail entirely on the _exit() paths this
  // runtime takes (CLAUDE.md, Bootstrap Sequence).
  std::fflush(stderr);
}

} // namespace

void uiTrace(const char *format, ...) {
  if (!uiTracingEnabled())
    return;
  va_list args;
  va_start(args, format);
  writeLineV("[ui] ", format, args);
  va_end(args);
}

void uiStderrLine(const char *format, ...) {
  va_list args;
  va_start(args, format);
  writeLineV("", format, args);
  va_end(args);
}

} // namespace node_compat
} // namespace hermes
