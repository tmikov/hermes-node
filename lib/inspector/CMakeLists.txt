# Copyright (c) Tzvetan Mikov.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

add_hermes_library(hermesNodeInspector STATIC
  inspector_bridge.cpp
)

target_include_directories(hermesNodeInspector
  PUBLIC
    ${PROJECT_SOURCE_DIR}/include
  PRIVATE
    ${PROJECT_SOURCE_DIR}/hermes/include/hermes/napi
)

target_link_libraries(hermesNodeInspector
  PUBLIC
    uv_a
)
