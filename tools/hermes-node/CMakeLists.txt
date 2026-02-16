# Copyright (c) Tzvetan Mikov.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

add_hermes_tool(hermes-node
  hermes-node.cpp
  LINK_LIBS
    hermesNodeEventLoop
    hermesNodeBindingRegistry
    hermesNodeBindings
    hermesNodeModuleLoader
    hermesNodeProcess
    hermesvm_a
)

# hermesNapi was removed from hermesvm_a upstream (54c50a06a).
# We must whole-archive link it so all NAPI symbols are available.
if(APPLE)
  target_link_libraries(hermes-node -force_load hermesNapi)
elseif(MSVC)
  target_link_libraries(hermes-node hermesNapi)
  target_link_options(hermes-node PRIVATE /WHOLEARCHIVE:hermesNapi)
else()
  target_link_libraries(hermes-node
    -Wl,--whole-archive
    hermesNapi
    -Wl,--no-whole-archive
  )
endif()

# hermes_napi.h (internal), generated config, and LLVH headers.
target_include_directories(hermes-node PRIVATE
  ${CMAKE_SOURCE_DIR}/hermes/API/napi
  ${CMAKE_BINARY_DIR}/hermes/lib/config
  ${CMAKE_SOURCE_DIR}/hermes/external/llvh/include
  ${CMAKE_SOURCE_DIR}/hermes/external/llvh/gen/include
  ${CMAKE_BINARY_DIR}/hermes/external/llvh/include
)

# GC define needed for Hermes VM headers included via hermes_napi.h.
target_compile_definitions(hermes-node PRIVATE
  HERMESVM_GC_${HERMESVM_GCKIND}
)
