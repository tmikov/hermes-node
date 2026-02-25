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
)

if(HERMES_ENABLE_DEBUGGER)
  target_compile_definitions(hermesNodeRuntime PRIVATE HERMES_ENABLE_DEBUGGER)
endif()

target_link_libraries(hermesNodeRuntime
  PUBLIC
    hermesNodeEmbeddedModules
    hermesNodeEventLoop
    hermesNodeBindingRegistry
    hermesNodeBindings
    hermesNodeModuleLoader
    hermesNodeProcess
    hermesNodeInspector
    hermesNapiCompile
    hermesvm_a
)
