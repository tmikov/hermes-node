# Copyright (c) Tzvetan Mikov.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

add_hermes_library(hermesNodeRuntime STATIC
  hermes_node_runtime.cpp
)

target_include_directories(hermesNodeRuntime
  PUBLIC
    ${PROJECT_SOURCE_DIR}/include
  PRIVATE
    ${PROJECT_SOURCE_DIR}/hermes/include/hermes/napi
    ${PROJECT_SOURCE_DIR}/hermes/include
    ${PROJECT_SOURCE_DIR}/hermes/API
    ${CMAKE_BINARY_DIR}/generated
)

# version.h is re-derived on every build (see cmake/version.cmake); this
# orders that ahead of the compile that includes it.
add_dependencies(hermesNodeRuntime hermes-node-version)

if(HERMES_ENABLE_DEBUGGER)
  target_compile_definitions(hermesNodeRuntime PRIVATE HERMES_ENABLE_DEBUGGER)
endif()

target_compile_definitions(hermesNodeRuntime PRIVATE
  HERMES_NODE_CACHE_ARCH="${CMAKE_SYSTEM_PROCESSOR}"
)

target_link_libraries(hermesNodeRuntime
  PUBLIC
    hermesNodeEmbeddedModules
    hermesNodeEventLoop
    hermesNodeBindingRegistry
    hermesNodeBindings
    hermesNodeCompileCache
    hermesNodeModuleLoader
    hermesNodeProcess
    hermesNodeInspector
    hermesNapiCompile
    hermesvm_a
)
