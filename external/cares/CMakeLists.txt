# Copyright (c) Tzvetan Mikov.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# Wrapper CMakeLists.txt for vendored c-ares.
# We only need the static library, no shared lib, tools, or tests.

set(CARES_STATIC ON CACHE BOOL "" FORCE)
set(CARES_SHARED OFF CACHE BOOL "" FORCE)
set(CARES_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(CARES_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(CARES_INSTALL OFF CACHE BOOL "" FORCE)

add_subdirectory(cares)

# Convenience alias matching our naming convention (like uv_a).
# When CARES_SHARED=OFF, the static target is named "c-ares" (no _static suffix).
add_library(cares_a ALIAS c-ares)
