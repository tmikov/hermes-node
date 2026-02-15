# Copyright (c) Tzvetan Mikov.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

add_hermes_library(hermesNodeBindingRegistry STATIC
  binding_registry.cpp
)

target_include_directories(hermesNodeBindingRegistry PUBLIC
  # Our own public headers.
  ${CMAKE_SOURCE_DIR}/include
  # Hermes public NAPI headers (js_native_api.h, node_api_types.h, etc.).
  ${CMAKE_SOURCE_DIR}/hermes/include/hermes/napi
)
