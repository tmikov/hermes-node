/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/// The PRESENTING instance of sokol_gfx: the one the platform thread owns
/// and blits with. Identical to sokol_gfx_impl.cpp except that every public
/// function is renamed `sg_x` -> `sgp_x` by the generated header below.
///
/// **The rename must precede sokol_gfx.h's DECLARATIONS, not merely its
/// implementation**, and that ordering is the single most breakable thing
/// here. `sokol_gfx.h` also defines C++ inline overloads
/// (`inline void sg_setup(const sg_desc&)`); included after the
/// declarations, the rename would leave those bound to the *other* copy,
/// producing an ODR violation that links cleanly and then calls into the
/// wrong instance's state from the wrong thread. Measured during the Linux
/// spike (docs/notes/2026-09-19-linux-spike-results.md, section 3), which is
/// why it is stated as fact rather than caution. `sgp_rename.h` itself
/// `#error`s if `SOKOL_GFX_INCLUDED` is already defined when it is reached,
/// so getting this backwards is a compile error rather than a run-time
/// mystery.
///
/// Types are NOT renamed -- see utils/gen-sokol-rename.py for why. The two
/// instances exchange `sg_image`/`sg_view` values, so those must stay one
/// type.

#include "sokol_config.h"

#include "sgp_rename.h"

#define SOKOL_GFX_IMPL
#include <sokol_gfx.h>
