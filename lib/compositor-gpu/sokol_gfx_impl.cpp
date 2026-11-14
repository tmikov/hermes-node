/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/// The PRODUCING instance of sokol_gfx: the one the JS thread owns and every
/// addon's `sg_*` call lands in. Public symbols keep their upstream names,
/// because those are what an addon compiled against `sokol_gfx.h` links to
/// (the embedder exports them -- see the design's "The native C API").
///
/// Its twin is sokol_gfx_present_impl.cpp, which compiles this same header a
/// second time behind `sgp_rename.h`. `sokol_gfx` keeps all of its state in
/// one process-global (`static _sg_state_t _sg`) and every call must come
/// from the thread owning that instance's GPU context, so two threads need
/// two instances. Nothing but the rename separates the two translation
/// units; if this file grows anything besides the include below, the twin
/// needs it too.
///
/// `utils/check-sgp-isolation.py` proves the separation actually held, by
/// reading the two objects' symbol tables rather than by trusting this
/// comment.

#include "sokol_config.h"

#define SOKOL_GFX_IMPL
#include <sokol_gfx.h>
