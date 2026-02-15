# Copyright (c) Tzvetan Mikov.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

add_hermes_library(hermesNodeProcess STATIC
  node_process.cpp
)

target_include_directories(hermesNodeProcess PUBLIC
  # Our own public headers.
  ${CMAKE_SOURCE_DIR}/include
  # Hermes public NAPI headers (js_native_api.h, node_api_types.h, etc.).
  ${CMAKE_SOURCE_DIR}/hermes/include/hermes/napi
)

target_link_libraries(hermesNodeProcess PUBLIC
  uv_a
)
