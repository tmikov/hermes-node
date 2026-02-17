# Copyright (c) Tzvetan Mikov.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

cmake_minimum_required(VERSION 3.21.0)
project(hermes-node-compat LANGUAGES C CXX)

# Pull in Hermes (includes cmake modules, gtest, NAPI, etc.)
add_subdirectory(hermes)

# Use Hermes cmake modules (Hermes.cmake, Lit.cmake, etc.)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/hermes/cmake/modules/")
include(Hermes)
include(Lit)

# LLVH source dir for gtest includes (used by add_unittest in Lit.cmake)
set(LLVH_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/hermes/external/llvh)

# Match Hermes sanitizer flags for our own targets.
# Hermes sets these inside its subdirectory scope, so they don't propagate.
if (HERMES_ENABLE_ADDRESS_SANITIZER)
  add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address)
endif()

# Vendored external dependencies
add_subdirectory(external/libuv)
add_subdirectory(external/simdutf)
add_subdirectory(external/ada)
add_subdirectory(external/cares)
add_subdirectory(external/llhttp)

# Our C++ libraries
add_subdirectory(lib/event-loop)
add_subdirectory(lib/binding-registry)
add_subdirectory(lib/module-loader)
add_subdirectory(lib/process)
add_subdirectory(lib/bindings)

# CLI tools
add_subdirectory(tools/hermes-node)

# Unit tests
add_subdirectory(unittests)

# JS tests (Lit-based)
# Python is needed by hermes-lit (Hermes's find_package is subdirectory-scoped).
find_package(Python COMPONENTS Interpreter)
set(LLVH_LIT_ARGS "-sv")
set(HERMES_NODE_LIT_PARAMS
  hermes_node=${HERMES_TOOLS_OUTPUT_DIR}/hermes-node
  hermes=${HERMES_TOOLS_OUTPUT_DIR}/hermes
  FileCheck=${HERMES_TOOLS_OUTPUT_DIR}/FileCheck
  source_dir=${CMAKE_CURRENT_SOURCE_DIR}
  test_exec_root=${CMAKE_CURRENT_BINARY_DIR}/test
)

add_lit_testsuite(check-hermes-node-js "Running hermes-node JS tests"
  ${CMAKE_CURRENT_SOURCE_DIR}/test
  PARAMS ${HERMES_NODE_LIT_PARAMS}
  DEPENDS hermes hermes-node
)

# check-hermes-node target: run all tests (unit tests + JS tests)
add_custom_target(check-hermes-node
  DEPENDS NodeCompatUnitTests check-hermes-node-js
  COMMENT "Running hermes-node-compat tests"
)
