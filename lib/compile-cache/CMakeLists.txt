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
  PRIVATE
    ${PROJECT_SOURCE_DIR}/hermes/include/hermes/napi
    ${PROJECT_SOURCE_DIR}/hermes/include
    ${PROJECT_SOURCE_DIR}/hermes/API
    ${CMAKE_BINARY_DIR}/generated
)

target_link_libraries(hermesNodeCompileCache
  PUBLIC
    uv_a
  PRIVATE
    zlib_a
)
