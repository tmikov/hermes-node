# Copyright (c) Tzvetan Mikov.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

add_hermes_tool(hermes-node
  hermes-node.cpp
  LINK_LIBS
    hermesNodeRuntime
    # The read-only verbs are dispatched from main(), before any runtime
    # exists, so the tools layer is linked here rather than reached through
    # hermesNodeRuntime.
    hermesNodeBundleTools
    # --dump-bytecode, the one verb that needs the Hermes disassembler. Its
    # own target so that hermesNodeBundleTools stays VM-free; see
    # lib/bytecode-dump/CMakeLists.txt.
    hermesNodeBytecodeDump
)

target_include_directories(hermes-node PRIVATE
  ${CMAKE_BINARY_DIR}/generated
)

# version.h is re-derived on every build (see cmake/version.cmake); this orders
# that ahead of the compile that includes it.
add_dependencies(hermes-node hermes-node-version)

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

# Export NAPI symbols so dlopen'd addons can resolve them.
if(APPLE)
  target_link_options(hermes-node PRIVATE -Wl,-export_dynamic)
elseif(UNIX)
  target_link_options(hermes-node PRIVATE -rdynamic)
endif()
