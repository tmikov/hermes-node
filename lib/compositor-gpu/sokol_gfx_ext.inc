/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/// The definitions behind `sokol_gfx_ext.h`, included **after**
/// `#include <sokol_gfx.h>` with `SOKOL_GFX_IMPL` defined -- that is, by
/// `sokol_gfx_impl.cpp` and `sokol_gfx_present_impl.cpp` and nothing else --
/// with `HNUI_SG_EXT(name)` defined by the includer to paste its instance's
/// prefix (`hnui_sg_##name` or `hnui_sgp_##name`). Each unit has its own
/// file-static `_sg`, so the same text reads a different instance in each.
///
/// The body is Metal-only. On GL this file compiles to nothing, and the two
/// objects gain no symbol: `test/sokol-isolation.js` and
/// `test/sokol-rename.js` see exactly what they saw before it existed.

#ifndef HNUI_SG_EXT
#error "Define HNUI_SG_EXT(name) before including sokol_gfx_ext.inc."
#endif
#ifndef SOKOL_GFX_IMPL
#error \
    "sokol_gfx_ext.inc reads _sg: include it after sokol_gfx.h's implementation."
#endif

#include "sokol_gfx_ext.h"

#if defined(SOKOL_METAL)
extern "C" {

const void *HNUI_SG_EXT(mtl_command_buffer)(void) {
  return (__bridge const void *)_sg.mtl.cmd_buffer;
}

int HNUI_SG_EXT(mtl_idpool_free)(void) {
  return _sg.mtl.idpool.free_queue_top;
}

int HNUI_SG_EXT(mtl_idpool_pending)(void) {
  const int slots = _sg.mtl.idpool.num_slots;
  if (slots <= 0)
    return 0;
  const int span =
      _sg.mtl.idpool.release_queue_front - _sg.mtl.idpool.release_queue_back;
  return ((span % slots) + slots) % slots;
}

} // extern "C"
#endif // SOKOL_METAL
