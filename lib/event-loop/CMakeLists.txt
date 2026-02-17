# Copyright (c) Tzvetan Mikov.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

add_hermes_library(hermesNodeEventLoop STATIC
  uv_event_loop.cpp
)

target_include_directories(hermesNodeEventLoop PUBLIC
  # Our own public headers.
  ${PROJECT_SOURCE_DIR}/include
  # Hermes public NAPI headers: napi/hermes_napi.h -> hermes/napi/node_api.h.
  ${PROJECT_SOURCE_DIR}/hermes/API
  ${PROJECT_SOURCE_DIR}/hermes/include
)

target_link_libraries(hermesNodeEventLoop PUBLIC
  uv_a
)
