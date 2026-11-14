// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// The two sokol_gfx instances must not be able to reach each other's state.
//
// sokol_gfx keeps everything in one process-global (`static _sg_state_t
// _sg`) and every call must come from the thread owning that instance's GPU
// context. The compositor compiles the implementation twice -- plainly for
// the JS thread that draws, and behind sgp_rename.h for the platform thread
// that presents -- and the whole separation rests on that rename having
// reached every public function.
//
// Nothing about that is checked by the compiler. A function the rename list
// missed compiles, links, and then routes one instance's call into the
// other's `_sg`. So utils/check-sgp-isolation.py reads the two objects'
// symbol tables and asserts it directly; this runs that check inside the
// ordinary suite rather than leaving it to the `check-sgp-isolation` target,
// which would only ever run when somebody remembered to ask for it.
//
// This is also the independent check on the generator itself.
// gen-sokol-rename.py verifies its own list by comparing sokol_gfx.h's
// declarations against its definitions -- but both halves of that are
// regexes over one file and would miss the same unusual spelling. "The
// presenting object exports zero sg_* symbols" is the same property
// measured from the compiler's output, sharing no machinery with it.
//
// Gated on `ui-gpu` rather than `ui-enabled`: the two objects this reads
// exist only where a GPU backend was built, which is not every platform
// the option can be turned on for. See test/sokol-rename.js and
// test/lit.cfg.
//
// REQUIRES: ui-gpu

// RUN: %python %source_dir/utils/check-sgp-isolation.py --producing %sgp_objdir/sokol_gfx_impl.cpp.o --presenting %sgp_objdir/sokol_gfx_present_impl.cpp.o | %FileCheck %s

// The counts are checked, not just the verdict. A checker that read no
// symbols at all would satisfy every "must not contain" assertion it makes,
// so the script refuses to report success on an empty read and prints what
// it found; this pins that the numbers reach stdout, which is what lets a
// reader tell "isolated" from "saw nothing".
//
// CHECK: sgp isolation OK: producing defines {{[0-9]+}} sg_*, presenting defines {{[0-9]+}} sgp_*, no cross-references
