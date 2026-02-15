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

# JS tests (run with stock hermes CLI and hermes-node)
add_custom_target(check-hermes-node-js
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test/run-primordials-test.sh
    $<TARGET_FILE:hermes>
    ${CMAKE_CURRENT_SOURCE_DIR}
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test/run-boot-test.sh
    $<TARGET_FILE:hermes-node>
    ${CMAKE_CURRENT_SOURCE_DIR}
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test/run-hermes-node-test.sh
    $<TARGET_FILE:hermes-node>
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/test/test-constants.js
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test/run-hermes-node-test.sh
    $<TARGET_FILE:hermes-node>
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/test/test-types.js
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test/run-hermes-node-test.sh
    $<TARGET_FILE:hermes-node>
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/test/test-util.js
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test/run-hermes-node-test.sh
    $<TARGET_FILE:hermes-node>
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/test/test-string-decoder.js
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test/run-hermes-node-test.sh
    $<TARGET_FILE:hermes-node>
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/test/test-errors.js
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test/run-hermes-node-test.sh
    $<TARGET_FILE:hermes-node>
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/test/test-config.js
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test/run-hermes-node-test.sh
    $<TARGET_FILE:hermes-node>
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/test/test-symbols.js
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test/run-hermes-node-test.sh
    $<TARGET_FILE:hermes-node>
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/test/test-options.js
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test/run-hermes-node-test.sh
    $<TARGET_FILE:hermes-node>
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/test/test-bootstrap.js
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test/run-hermes-node-test.sh
    $<TARGET_FILE:hermes-node>
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/test/test-buffer.js
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test/run-hermes-node-test.sh
    $<TARGET_FILE:hermes-node>
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/test/test-encoding.js
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test/run-hermes-node-test.sh
    $<TARGET_FILE:hermes-node>
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/test/test-async-wrap.js
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test/run-hermes-node-test.sh
    $<TARGET_FILE:hermes-node>
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/test/test-nexttick.js
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test/run-hermes-node-test.sh
    $<TARGET_FILE:hermes-node>
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/test/test-timers.js
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test/run-stdio-test.sh
    $<TARGET_FILE:hermes-node>
    ${CMAKE_CURRENT_SOURCE_DIR}
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test/run-hermes-node-test.sh
    $<TARGET_FILE:hermes-node>
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/test/test-stream-wrap.js
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test/run-hermes-node-test.sh
    $<TARGET_FILE:hermes-node>
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/test/test-core-modules.js
  DEPENDS hermes hermes-node
  COMMENT "Running JS tests"
)

# check-hermes-node target: run all tests (unit tests + JS tests)
add_custom_target(check-hermes-node
  DEPENDS NodeCompatUnitTests check-hermes-node-js
  COMMENT "Running hermes-node-compat tests"
)
