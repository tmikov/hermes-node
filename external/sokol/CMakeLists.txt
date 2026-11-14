# Copyright (c) Tzvetan Mikov.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# Sokol is header-only: there is nothing to compile here. The single-file
# headers carry their own implementation, guarded by SOKOL_*_IMPL, and the
# compositor defines those in translation units of its own
# (lib/compositor-gpu/, tools/hermes-node-ui/) -- deliberately, because
# sokol_gfx.h's implementation is compiled TWICE there under different
# symbol prefixes, which a library target here could not express.
#
# So this target carries the include path and nothing else.
add_library(sokol INTERFACE)

# SYSTEM, unlike the other vendored wrappers here. Hermes builds our targets
# with its own warning set, and sokol_gfx.h's implementation is ~20k lines of
# unmodified upstream C that was never written against it; without SYSTEM the
# first real build of lib/compositor-gpu buries its own diagnostics under
# upstream's. The `external/` convention says this source is not ours to fix,
# and suppressing warnings we are forbidden to act on is the consistent
# reading of that.
target_include_directories(sokol SYSTEM INTERFACE
  ${CMAKE_CURRENT_SOURCE_DIR}/sokol
)

# The vendored commit, read out of hnui.h rather than typed a second time.
# hnui.h must carry a value of its own, because an addon is compiled outside
# this build and includes it alone; so that file is where the commit is
# written, and this variable is derived from it. Combined with
# test/sokol-pin.js (which checks the variable against the commit
# external/sokol/README.md records), a bump is one edit in hnui.h plus the
# README line, and forgetting either fails a test rather than shipping.
# Editing the header must re-run configure, or the variable (and the lit
# param built from it) keeps the old commit after a correct bump.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  ${PROJECT_SOURCE_DIR}/include/hermes/node-compat/ui/hnui.h)
file(STRINGS ${PROJECT_SOURCE_DIR}/include/hermes/node-compat/ui/hnui.h
  _hnui_pinned_line
  REGEX "^#define HNUI_SOKOL_COMMIT_PINNED \"[0-9a-f]+\"$")
if(NOT _hnui_pinned_line MATCHES "\"([0-9a-f]+)\"")
  message(FATAL_ERROR
    "Could not find HNUI_SOKOL_COMMIT_PINNED in hnui.h; see "
    "external/sokol/CMakeLists.txt")
endif()
set(HNUI_SOKOL_COMMIT "${CMAKE_MATCH_1}" CACHE INTERNAL
  "The Sokol commit vendored at external/sokol/sokol (from hnui.h)")
