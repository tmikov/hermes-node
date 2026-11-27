# Copyright (c) Tzvetan Mikov.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# hermes-node-imgui.node: the Dear ImGui addon, loaded by hermes-node-ui as
# `hermes-node:imgui` from the binary's own directory
# (tools/hermes-node-ui/named_addons.cpp). Built into that directory for that
# reason. Added from the top-level CMakeLists.txt only under
# HERMES_NODE_ENABLE_UI.

add_library(hermes_node_imgui SHARED
  imgui_module.cpp
  imgui_platform.cpp
  imgui_renderer.cpp
  imgui_test_hooks.cpp
  bind/bind_util.cpp
  bind/bind_guards.cpp
  bind/bind_constants.cpp
  bind/bind_frame.cpp
  bind/bind_windows.cpp
  bind/bind_layout.cpp
  bind/bind_text.cpp
  bind/bind_widgets.cpp
  bind/bind_trees_tabs.cpp
  bind/bind_menus.cpp
  bind/bind_tables.cpp
  bind/bind_popups.cpp
  bind/bind_queries.cpp
  bind/bind_style.cpp
  bind/bind_plots.cpp
  bind/bind_drawlist.cpp
  bind/bind_clipper.cpp
  bind/bind_docking.cpp
  ${IMGUI_SOURCES}
)

# Unmodified upstream source: warnings we are forbidden to act on would only
# bury our own (external/imgui/CMakeLists.txt makes the headers SYSTEM for
# the same reason).
set_source_files_properties(${IMGUI_SOURCES} PROPERTIES COMPILE_OPTIONS "-w")

# The include paths hnui_demo_addon uses (top-level CMakeLists.txt), for the
# reason given there: an addon writes `#include <hnui.h>` flat, and hnui.h
# includes hnui_abi.h by its in-tree path, so both directories are needed.
# addons/imgui itself is on the path for IMGUI_USER_CONFIG.
target_include_directories(hermes_node_imgui PRIVATE
  ${PROJECT_SOURCE_DIR}/hermes/include/hermes/napi
  ${PROJECT_SOURCE_DIR}/include/hermes/node-compat/ui
  ${PROJECT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}
)
# sokol for sokol_gfx.h's declarations only; the sg_* symbols resolve
# against hermes-node-ui at dlopen time.
target_link_libraries(hermes_node_imgui PRIVATE sokol imgui_headers)
# The host's backend define (SOKOL_GLCORE or SOKOL_METAL), read from the
# compositor's own PUBLIC compile definitions as hnui_demo_addon does, so the
# addon cannot be built for a backend the host was not. Read through the
# property alone: linking hermesNodeCompositorGpu would drag the compositor
# into a module that must resolve every sg_* symbol against the host.
target_compile_definitions(hermes_node_imgui PRIVATE
  $<TARGET_PROPERTY:hermesNodeCompositorGpu,INTERFACE_COMPILE_DEFINITIONS>
  IMGUI_USER_CONFIG="imgui_config.h"
)

set_target_properties(hermes_node_imgui PROPERTIES
  PREFIX ""
  OUTPUT_NAME hermes-node-imgui
  SUFFIX ".node"
  LIBRARY_OUTPUT_DIRECTORY ${HERMES_TOOLS_OUTPUT_DIR}
  # ImGui is compiled into this object and its symbols stay its own, so a
  # second addon carrying another copy cannot collide with it. Hermes's
  # -fvisibility=hidden is scoped to its own subdirectory and does not reach
  # here. The N-API init stays exported through NAPI_MODULE_INIT's own
  # visibility attribute; the export list below is the second layer, and
  # test/imgui-symbols.js checks the result.
  CXX_VISIBILITY_PRESET hidden
  C_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON
)
# And an explicit export list on top of it. Hidden visibility does not reach
# std:: template instantiations: libstdc++ declares namespace std with
# default visibility, so a std::vector used anywhere in the addon is exported
# as a weak symbol. The list names the two NAPI_MODULE_INIT() functions and
# nothing else. macOS has a file of its own, in ld64's exported-symbols
# format with the Mach-O (underscore-prefixed) names; keep the two in step.
# The macOS link was made and its export table read on 2026-11-27.
if(APPLE)
  set(_imgui_exports ${CMAKE_CURRENT_SOURCE_DIR}/exports.darwin.txt)
  target_link_options(hermes_node_imgui PRIVATE
    -undefined dynamic_lookup
    -Wl,-exported_symbols_list,${_imgui_exports})
else()
  set(_imgui_exports ${CMAKE_CURRENT_SOURCE_DIR}/exports.map)
  target_link_options(hermes_node_imgui PRIVATE
    -Wl,--version-script=${_imgui_exports})
endif()
set_target_properties(hermes_node_imgui PROPERTIES
  LINK_DEPENDS ${_imgui_exports})
