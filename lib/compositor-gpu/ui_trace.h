/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_NODE_COMPAT_COMPOSITOR_GPU_UI_TRACE_H
#define HERMES_NODE_COMPAT_COMPOSITOR_GPU_UI_TRACE_H

namespace hermes {
namespace node_compat {

/// Whether HERMES_NODE_DEBUG_NATIVE names UI.
///
/// The plan's Global Constraints forbid timing-based assertions -- the suite
/// runs 16-way parallel -- and name this tracing as what the end-to-end
/// tests assert from instead. So these lines are test surface, not debug
/// leftovers: a message removed here can silently un-test something.
///
/// Read **once**, into a function-local static, for the same reason
/// libjs/bundle-loader.js reads its own gate once (CLAUDE.md, AOT Bundles):
/// this sits on per-frame paths, and a process that could change its own
/// tracing mid-run would produce a log nobody can read start to finish.
bool uiTracingEnabled();

/// printf-style, to **stderr**, prefixed "[ui] ". Nothing when tracing is
/// off, and the arguments are still evaluated -- so no call site may hide a
/// side effect in one.
void uiTrace(const char *format, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;

} // namespace node_compat
} // namespace hermes

#endif // HERMES_NODE_COMPAT_COMPOSITOR_GPU_UI_TRACE_H
