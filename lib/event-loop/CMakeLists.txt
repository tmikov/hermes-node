# Copyright (c) Tzvetan Mikov.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

add_hermes_library(hermesNodeEventLoop STATIC
  uv_event_loop.cpp
)

target_include_directories(hermesNodeEventLoop PUBLIC
  # Our own public headers.
  ${CMAKE_SOURCE_DIR}/include
  # Hermes public NAPI type headers (js_native_api_types.h for napi_status).
  ${CMAKE_SOURCE_DIR}/hermes/include/hermes/napi
)

target_link_libraries(hermesNodeEventLoop PUBLIC
  uv_a
)
