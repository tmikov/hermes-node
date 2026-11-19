/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/// The one and only `sokol_app` implementation in this process.
///
/// A translation unit of its own, rather than a `#define SOKOL_APP_IMPL` at
/// the top of `ui_platform.cpp`, for the reason that split is usually worth
/// making: `ui_platform.cpp` includes `sokol_app.h` for its declarations, and
/// a file that both defines a 15,000-line implementation and writes code
/// against it gives every compile error in either half the other half's
/// line numbers.
///
/// Unlike `sokol_gfx`, this is compiled **once**. Only `sokol_gfx` needs two
/// instances, because only it keeps per-thread GPU state; `sokol_app` runs
/// entirely on the platform thread and has nothing to separate.
///
/// The backend define (`SOKOL_GLCORE`) arrives from
/// `hermesNodeCompositorGpu`'s PUBLIC compile definitions, so this and the
/// two `sokol_gfx` instances cannot be configured differently -- see
/// `lib/compositor-gpu/sokol_config.h` for why that matters.

#include "../../lib/compositor-gpu/sokol_config.h"

/// **Required, not optional.** By default `sokol_app.h`'s implementation
/// supplies its own `main()`, which calls a `sokol_main()` the application
/// is expected to write and which returns the `sapp_desc` to run. That
/// shape cannot work here: this process's `main()` parses a command line,
/// spawns the JS thread and only reaches a window if a script asks for one,
/// so the desc does not exist until long after startup. `SOKOL_NO_ENTRY`
/// is how Sokol supports exactly that -- the application keeps its own
/// entry point and calls `sapp_run()` when it is ready, which
/// `uiRunPlatformLoop()` does.
///
/// Without it the link fails with two definitions of `main`, which is at
/// least loud. It is spelled out here because the failure names
/// `sokol_app.h:14334` and gives no hint that a one-line define is the
/// answer.
#define SOKOL_NO_ENTRY

#define SOKOL_APP_IMPL
#include <sokol_app.h>
