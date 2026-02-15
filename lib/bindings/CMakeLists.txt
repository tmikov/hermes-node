# Copyright (c) Tzvetan Mikov.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

add_hermes_library(hermesNodeBindings STATIC
  node_config.cpp
  node_constants.cpp
  node_errors.cpp
  node_string_decoder.cpp
  node_symbols.cpp
  node_types.cpp
  node_util.cpp
)

target_include_directories(hermesNodeBindings
  PUBLIC
    ${CMAKE_SOURCE_DIR}/include
  PRIVATE
    ${CMAKE_SOURCE_DIR}/hermes/include/hermes/napi
)

target_link_libraries(hermesNodeBindings
  PUBLIC
    uv_a
)
