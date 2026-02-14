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

# Our C++ libraries
add_subdirectory(lib/placeholder)

# CLI tools
add_subdirectory(tools/hermes-node)

# Unit tests
add_subdirectory(unittests)

# check-hermes-node target: run all tests (unit tests + lit tests)
add_custom_target(check-hermes-node
  DEPENDS NodeCompatUnitTests
  COMMENT "Running hermes-node-compat tests"
)
