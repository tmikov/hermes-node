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
