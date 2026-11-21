// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// sgp_rename.h -- the generated header that gives the presenting sokol_gfx
// instance its own copy of every public function, so the two instances
// cannot reach each other's state (`static _sg_state_t _sg`).
//
// This asserts against the header the BUILD actually produced, not against
// a fresh run of the generator: %sgp_rename_h names the real artifact under
// the build tree, and check-hermes-node-js DEPENDS on the target that
// generates it. Re-running the generator here would test the generator
// while saying nothing about what got compiled.
//
// Gated on `ui-gpu`, NOT on `ui-enabled`. `ui-enabled` only says the option
// was compiled into plain hermes-node; `ui-gpu` adds the platform half,
// mirroring the guard in lib/compositor-gpu/CMakeLists.txt, which is the
// directory that generates this header. (The two used to differ on macOS,
// which had the option and no backend; that guard is a configure error
// now, so they agree wherever the option configured.) See test/lit.cfg for
// why it is a platform predicate rather than a check that the file exists.
//
// Still deliberately not `display` or `ui`: nothing here needs a window or
// the hermes-node-ui binary.
//
// REQUIRES: ui-gpu

// --- The renames themselves. ------------------------------------------
//
// CHECK-DAG, not ordered CHECKs, and that is a correction rather than a
// style preference (finding 11): the generator sorts its output, so an
// ordered sequence of CHECKs would encode the sort order into the test and
// contradict the very property the next RUN line pins. Two representative
// names, one from each end of the alphabet, so the check is not satisfied
// by a header that emitted only its first few lines.
//
// RUN: %FileCheck --check-prefix=RENAME %s < %sgp_rename_h
//
// RENAME-DAG: #define sg_setup sgp_setup
// RENAME-DAG: #define sg_commit sgp_commit
// RENAME-DAG: #define sg_make_image sgp_make_image
//
// Types must NOT be renamed. The two instances pass sg_image and sg_view
// values to each other -- the presenting side wraps textures the producing
// side made -- so a renamed type would give them structurally identical but
// differently-named types and break every signature that crosses. sg_view
// is the sharpest case, because `hnui_gpu_view` mirrors it across the
// public C ABI (see the static_asserts in lib/compositor-gpu/
// gl_backend.cpp). This CHECK-NOT runs over a file that is 156 lines of
// #defines, so it is a real assertion over populated input rather than one
// over a stream that is normally empty.
//
// RENAME-NOT: #define sg_view sgp_view
// RENAME-NOT: #define sg_image sgp_image
// RENAME-NOT: #define sg_desc sgp_desc

// --- How many. --------------------------------------------------------
//
// `>= 156`, not `== 156`. 156 is what the design measured with `nm` (spec:
// "Two copies of sokol_gfx") and what this pin of sokol_gfx.h produces, so
// it is a real floor rather than a guess -- but Task 16 adds the Metal
// command-buffer accessor, which is a 157th public function and must not
// break this. A floor catches the failure that matters (a generator that
// silently stops emitting) while leaving room for the growth that is
// already planned.
//
// RUN: test "$(grep -c '^#define sg_' %sgp_rename_h)" -ge 156

// --- Sorted. ----------------------------------------------------------
//
// Sorting is what keeps a Sokol bump's diff proportional to what actually
// changed, instead of reshuffled by wherever upstream moved a definition.
// `sort -c` exits non-zero on the first line out of order, so an unsorted
// generator fails this RUN line -- which is the only thing here that pins
// the property, since CHECK-DAG above is by construction indifferent to it.
//
// RUN: grep '^#define sg_' %sgp_rename_h | awk '{print $2}' | sort -c

// --- The ordering guard. ----------------------------------------------
//
// The header must be included before sokol_gfx.h's DECLARATIONS, not merely
// before its implementation: sokol_gfx.h defines C++ inline overloads
// (`inline void sg_setup(const sg_desc&)`) which bind to whichever copy was
// declared first. Getting that backwards links cleanly and then calls into
// the wrong instance from the wrong thread, so the generated header carries
// an #error against it.
//
// RUN: %FileCheck --check-prefix=GUARD %s < %sgp_rename_h
//
// GUARD: #ifdef SOKOL_GFX_INCLUDED
// GUARD-NEXT: #error
