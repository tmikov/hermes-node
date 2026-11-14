# Copyright (c) Tzvetan Mikov.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

add_hermes_library(hermesNodeCompositor STATIC
  surface_table.cpp
  owner_dispatch.cpp
  tick_machine.cpp
  channels.cpp
  hnui_event_convert.cpp
)
target_include_directories(hermesNodeCompositor PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(hermesNodeCompositor PUBLIC
  uv_a
)
