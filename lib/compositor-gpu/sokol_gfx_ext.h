/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_NODE_COMPAT_COMPOSITOR_GPU_SOKOL_GFX_EXT_H
#define HERMES_NODE_COMPAT_COMPOSITOR_GPU_SOKOL_GFX_EXT_H

/// Accessors for sokol_gfx state that Sokol keeps private, one set per
/// instance: `hnui_sg_*` read the PRODUCING instance (the JS thread's,
/// `sokol_gfx_impl.cpp`), `hnui_sgp_*` the PRESENTING one (the platform
/// thread's, `sokol_gfx_present_impl.cpp`).
///
/// **No patch to vendored Sokol.** `_sg` is a file-static in whichever unit
/// defines `SOKOL_GFX_IMPL`, so code written after that unit's
/// `#include <sokol_gfx.h>` sees it directly. The definitions are
/// `sokol_gfx_ext.inc`, included by both implementation units with
/// `HNUI_SG_EXT(name)` defined to paste the instance's prefix. A Sokol bump
/// that renames a field these read stops compiling, the same loud way a
/// patch that no longer applied would.
///
/// **Metal only.** On GL the `.inc` compiles to nothing and these are
/// declared but never defined, so a call from GL code is a link error.
///
/// Each function must be called **only from the thread owning its
/// instance** -- the same rule as every `sg_*` / `sgp_*` call. The family a
/// caller uses is checked: `utils/check-sgp-isolation.py` fails if a
/// producing-side backend object references any `hnui_sgp_*`, or a
/// presenting-side one any `hnui_sg_*` (test/sokol-isolation.js).
///
/// No Sokol types here, deliberately, so a backend header can include this
/// without inheriting `sokol_gfx.h` or Objective-C.

#ifdef __cplusplus
extern "C" {
#endif

/// Sokol's frame command buffer, `_sg.mtl.cmd_buffer`, as a `__bridge`
/// (not retained) pointer: non-null only between the frame's first
/// `sg_begin_pass` and `sg_commit`, which commits and nils it. A completion
/// handler must be added to it before that commit.
const void *hnui_sg_mtl_command_buffer(void);
const void *hnui_sgp_mtl_command_buffer(void);

/// Free slots in the instance's Metal id-pool,
/// `_sg.mtl.idpool.free_queue_top`.
int hnui_sg_mtl_idpool_free(void);
int hnui_sgp_mtl_idpool_free(void);

/// Releases queued and not yet collected: `(release_queue_front -
/// release_queue_back)` modulo the pool's slot count. Sokol collects only
/// inside `sg_commit()`, and only releases whose frame index has passed.
int hnui_sg_mtl_idpool_pending(void);
int hnui_sgp_mtl_idpool_pending(void);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // HERMES_NODE_COMPAT_COMPOSITOR_GPU_SOKOL_GFX_EXT_H
