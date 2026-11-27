# Copyright (c) Tzvetan Mikov.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# Dear ImGui is not built here. The addon (addons/imgui/) compiles the five
# sources below into its own shared object with hidden visibility, so its
# ImGui symbols are its own and a second addon carrying another copy cannot
# collide with it. This directory hands that build the include path and the
# source list, and nothing else.
add_library(imgui_headers INTERFACE)

# SYSTEM, for the reason external/sokol/CMakeLists.txt gives: this is
# unmodified upstream source, and warnings we are forbidden to act on only
# bury our own.
target_include_directories(imgui_headers SYSTEM INTERFACE
  ${CMAKE_CURRENT_SOURCE_DIR}/imgui
)

set(IMGUI_SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/imgui/imgui.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/imgui/imgui_demo.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/imgui/imgui_draw.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/imgui/imgui_tables.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/imgui/imgui_widgets.cpp
  CACHE INTERNAL "The vendored Dear ImGui translation units")

# The pinned version, read out of the README the way external/sokol reads its
# commit, so the tag is written once. test/imgui-pin.js checks it against
# IMGUI_VERSION in the vendored header. Editing the README must re-run
# configure, or the variable keeps the old version after a correct bump.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  ${CMAKE_CURRENT_SOURCE_DIR}/README.md)
file(STRINGS ${CMAKE_CURRENT_SOURCE_DIR}/README.md _imgui_pinned_line
  REGEX "^- \\*\\*Pinned at tag `v[0-9.]+-docking`\\*\\*")
if(NOT _imgui_pinned_line MATCHES "`v([0-9.]+)-docking`")
  message(FATAL_ERROR
    "Could not find the pinned tag in external/imgui/README.md")
endif()
set(IMGUI_VERSION_PINNED "${CMAKE_MATCH_1}" CACHE INTERNAL
  "The Dear ImGui version vendored at external/imgui/imgui (from the README)")
