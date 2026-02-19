# Copyright (c) Tzvetan Mikov.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

add_hermes_library(hermesNodeModuleLoader STATIC
  module_loader.cpp
)

target_include_directories(hermesNodeModuleLoader PUBLIC
  # Our own public headers.
  ${PROJECT_SOURCE_DIR}/include
  # Hermes public NAPI headers (js_native_api.h, node_api_types.h, etc.).
  ${PROJECT_SOURCE_DIR}/hermes/include/hermes/napi
  PRIVATE
  # Hermes API directory (for napi/hermes_napi.h - hermes_run_script).
  ${PROJECT_SOURCE_DIR}/hermes/API
  # Hermes include root (hermes_napi.h includes "hermes/napi/node_api.h").
  ${PROJECT_SOURCE_DIR}/hermes/include
)
