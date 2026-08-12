# Copyright (c) Tzvetan Mikov.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

add_hermes_library(hermesNodeCompileCache STATIC
  compile_cache.cpp
)

target_include_directories(hermesNodeCompileCache
  PUBLIC
    ${PROJECT_SOURCE_DIR}/include
)

# This library talks to the filesystem through raw POSIX calls, not libuv,
# and never includes a Hermes header. zlib, for CRC32, is its only
# dependency -- keep it that way.
target_link_libraries(hermesNodeCompileCache
  PRIVATE
    zlib_a
)
