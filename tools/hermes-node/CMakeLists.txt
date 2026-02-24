# Copyright (c) Tzvetan Mikov.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

add_hermes_tool(hermes-node
  hermes-node.cpp
  LINK_LIBS
    hermesNodeRuntime
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

# Export NAPI symbols so dlopen'd addons can resolve them.
if(APPLE)
  target_link_options(hermes-node PRIVATE -Wl,-export_dynamic)
elseif(UNIX)
  target_link_options(hermes-node PRIVATE -rdynamic)
endif()
