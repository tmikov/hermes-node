/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_NODE_COMPAT_COMPOSITOR_GPU_SOKOL_LOG_H
#define HERMES_NODE_COMPAT_COMPOSITOR_GPU_SOKOL_LOG_H

#include <cstdint>

namespace hermes {
namespace node_compat {

/// The body of every Sokol logger in hermes-node-ui -- sokol_app's
/// (ui_platform.cpp) and both sokol_gfx instances' (gl_backend.cpp,
/// gl_present.cpp). Each of those is a thin `slog_func`-shaped wrapper that
/// names its instance, so a message says which one produced it.
///
/// Prints `[<instance>:<level>] item <id>: <message> (<file>:<line>)` to
/// stderr. **At level 0, a panic, it does not return**: it prints
/// \p panicNote (when non-null) as a second line, restores the terminal and
/// `_exit(1)`s. That is what Sokol's reference logger does (`slog_func`
/// aborts, sokol_log.h), and Sokol's code after a `_SAPP_PANIC` /
/// `_SG_PANIC` is written assuming the call does not come back -- a
/// sokol_gfx validation failure, for one, would otherwise carry on and
/// publish a frame it never drew.
///
/// The terminal restore is the reason this is one function rather than
/// three copies: every `_exit()` path in this tree calls
/// `uv_tty_reset_mode()` first (CLAUDE.md, Bootstrap Sequence -- raw mode
/// is a libuv global, and a shell left in it no longer echoes). Nothing
/// here flushes the JS thread's queued stdio the way `processExit()` does:
/// a panic can be raised on either thread, and from the platform thread
/// pumping the JS thread's loop would be a data race rather than a fix.
void uiSokolLog(
    const char *instance,
    uint32_t logLevel,
    uint32_t logItemId,
    const char *message,
    uint32_t lineNr,
    const char *filename,
    const char *panicNote);

/// Whether the producing sokol_gfx instance was compiled with its
/// validation layer -- sokol_gfx.h's own rule, `SOKOL_DEBUG`, which the
/// header defines itself unless `NDEBUG` is. Validation is what turns an
/// addon's malformed pass into a panic rather than undefined GL behaviour,
/// so a test that provokes one can only run where it is compiled in.
bool producerSokolValidates();

} // namespace node_compat
} // namespace hermes

#endif // HERMES_NODE_COMPAT_COMPOSITOR_GPU_SOKOL_LOG_H
