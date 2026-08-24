# Copyright (c) Tzvetan Mikov.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

cmake_minimum_required(VERSION 3.21.0)
project(hermes-node-compat LANGUAGES C CXX)

# Match Hermes C++ standard (required since Hermes headers use C++17 features).
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Enable Hermes debugger/CDP support (CDPAgent, CDPDebugAPI, AsyncDebuggerAPI).
# Must be set before add_subdirectory(hermes) to override Hermes's default OFF.
set(HERMES_ENABLE_DEBUGGER ON CACHE BOOL "Enable Hermes debugger/CDP" FORCE)

# Pull in Hermes (includes cmake modules, gtest, NAPI, etc.)
add_subdirectory(hermes)

# Use Hermes cmake modules (Hermes.cmake, Lit.cmake, etc.)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/hermes/cmake/modules/")
include(Hermes)
include(Lit)

# LLVH source dir for gtest includes (used by add_unittest in Lit.cmake)
set(LLVH_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/hermes/external/llvh)

# Derive hermes-node version (from -DHERMES_NODE_VERSION, git tag, or fallback).
include(cmake/version.cmake)

# Section-splitting, so that the --gc-sections/-dead_strip on the hermes-node
# link (tools/hermes-node/CMakeLists.txt) drops unreferenced code at the
# granularity of a function rather than a whole object file. Hermes already
# builds with these inside its own scope -- which is why that link flag is
# worth 8% at all, its archives dominating the binary -- and this extends the
# same treatment to everything added after add_subdirectory(hermes): our
# libraries, the vendored externals and the unit tests. Deliberately set after
# that call, not before, so Hermes is not re-flagged redundantly and forced to
# rebuild.
#
# Measured contribution on Linux x86_64, Release: 9,336 bytes of the 1,131,376
# the link flag saves. Small, because our own code is a few percent of the
# binary; kept because it costs nothing and is the half of the pair that
# applies to the code we add.
if (NOT MSVC)
  add_compile_options(-ffunction-sections -fdata-sections)
endif()

# Match Hermes sanitizer flags for our own targets.
# Hermes sets these inside its subdirectory scope, so they don't propagate.
if (HERMES_ENABLE_ADDRESS_SANITIZER)
  add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address)
endif()

# Python is needed by embedded-modules build pipeline and hermes-lit.
# (Hermes's find_package is subdirectory-scoped, so we need our own.)
find_package(Python COMPONENTS Interpreter)

# Vendored external dependencies
add_subdirectory(external/libuv)
add_subdirectory(external/simdutf)
add_subdirectory(external/ada)
add_subdirectory(external/cares)
add_subdirectory(external/llhttp)
add_subdirectory(external/picohash)
add_subdirectory(external/zlib)
add_subdirectory(external/brotli)
add_subdirectory(external/zstd)
add_subdirectory(external/hermes-parser-native)

# Target architecture string used to key on-disk generation schemes: the
# compile cache's generation directory name (lib/runtime) and the AOT
# bundle's generation tag (lib/bundle). Single-sourced here as an INTERFACE
# library so the two features cannot silently diverge on what
# "architecture" means -- e.g. if the derivation is later normalized for a
# new cross-compile target, both consumers pick that up together instead of
# one lagging behind and admitting bundles built for different hardware.
add_library(hermesNodeCacheArch INTERFACE)
target_compile_definitions(hermesNodeCacheArch INTERFACE
  HERMES_NODE_CACHE_ARCH="${CMAKE_SYSTEM_PROCESSOR}"
)

# Our C++ libraries
add_subdirectory(lib/embedded-modules)
add_subdirectory(lib/event-loop)
add_subdirectory(lib/binding-registry)
add_subdirectory(lib/module-loader)
add_subdirectory(lib/compile-cache)
add_subdirectory(lib/bundle)
add_subdirectory(lib/bytecode-dump)
add_subdirectory(lib/build-exe)
add_subdirectory(lib/process)
add_subdirectory(lib/bindings)
add_subdirectory(lib/inspector)
add_subdirectory(lib/runtime)

# CLI tools
add_subdirectory(tools/hermes-node)

# Unit tests
add_subdirectory(unittests)

# Test NAPI addon for process.dlopen / require('.node') tests.
add_library(hello_addon SHARED
  test/fixtures/test-addon/hello_addon.c
)
target_include_directories(hello_addon PRIVATE
  ${PROJECT_SOURCE_DIR}/hermes/include/hermes/napi
)
set_target_properties(hello_addon PROPERTIES
  PREFIX ""
  SUFFIX ".node"
)
if(APPLE)
  target_link_options(hello_addon PRIVATE -undefined dynamic_lookup)
endif()

# JS tests (Lit-based)
set(LLVH_LIT_ARGS "-sv")
set(HERMES_NODE_LIT_PARAMS
  hermes_node=${HERMES_TOOLS_OUTPUT_DIR}/hermes-node
  hermes=${HERMES_TOOLS_OUTPUT_DIR}/hermes
  FileCheck=${HERMES_TOOLS_OUTPUT_DIR}/FileCheck
  not=${HERMES_TOOLS_OUTPUT_DIR}/not
  source_dir=${CMAKE_CURRENT_SOURCE_DIR}
  test_exec_root=${CMAKE_CURRENT_BINARY_DIR}/test
  hello_addon=$<TARGET_FILE:hello_addon>
  kit_dir=${HERMES_NODE_KIT_DIR}
)

# hermes-node-kit is EXCLUDE_FROM_ALL (see tools/hermes-node/CMakeLists.txt:
# the merged archive is 40 MB in a Release build and 755 MB in an ASAN one),
# so an ordinary build does not produce it. The test suite depends on it
# anyway: --build-exe links against the kit, so a suite that ran with whatever
# kit happened to be lying around would be testing a link nobody performs --
# and the version check inside --build-exe would fail every test the moment
# the version string moved. Re-cutting is a few seconds; running against a
# stale kit is a silent wrong answer.
add_lit_testsuite(check-hermes-node-js "Running hermes-node JS tests"
  ${CMAKE_CURRENT_SOURCE_DIR}/test
  PARAMS ${HERMES_NODE_LIT_PARAMS}
  DEPENDS hermes hermes-node hello_addon hermes-node-kit
)

# Unit tests (GTest, discovered and run by Lit)
add_lit_testsuite(check-hermes-node-unit "Running hermes-node unit tests"
  ${CMAKE_CURRENT_SOURCE_DIR}/unittests
  PARAMS unittests_dir=${CMAKE_CURRENT_BINARY_DIR}/unittests
  DEPENDS NodeCompatUnitTests
)

# check-hermes-node target: run all tests (unit tests + JS tests)
add_custom_target(check-hermes-node
  DEPENDS check-hermes-node-unit check-hermes-node-js
  COMMENT "Running hermes-node-compat tests"
)

# Examples are not part of check-hermes-node: they need a network install of
# Babel and friends, while the default suite stays offline. Examples that
# have not been installed are skipped rather than failed.
add_custom_target(check-hermes-node-examples
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/examples/run-examples.sh
          ${CMAKE_CURRENT_BINARY_DIR}
  DEPENDS hermes-node hermes-parser-napi
  COMMENT "Running hermes-node examples"
  VERBATIM
)
