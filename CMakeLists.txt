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

# Enable WebAssembly. Node programs expect the `WebAssembly` global to exist,
# and a growing number of packages ship their native half as a .wasm blob
# rather than an addon, so a runtime without it fails at require() time on
# code that has no other problem. Hermes defaults this OFF; we override it
# for the same reason we override the debugger above, and it must likewise be
# set before add_subdirectory(hermes) -- HERMES_ENABLE_WASM controls an
# add_definitions() in Hermes's own scope, so it decides how the VM itself is
# compiled and cannot be flipped afterwards.
#
# Not free: measured on Linux x86_64, RelWithDebInfo, stripped, hermes-node
# grew 11,307,264 -> 12,543,112 bytes (+1,235,848, +10.9%) for wabt plus the
# Wasm frontend and IR generator. That lands in every --build-exe artifact
# too. FORCE is deliberately not used, unlike the debugger above, so a build
# that does not want the size can still configure -DHERMES_ENABLE_WASM=OFF.
set(HERMES_ENABLE_WASM ON CACHE BOOL "Enable WebAssembly support")

# Compile Hermes's own InternalJavaScript to a native Static Hermes unit
# instead of running it as bytecode at every runtime creation. Without this a
# "fully native" build-native artifact still interprets Hermes's bootstrap JS,
# which makes the feature's headline claim false and its residual-bytecode
# test impossible to write.
#
# Nearly free, because the archive already exists: Hermes builds
# hermesInternalUnit in every non-MSVC configuration to keep it from
# bitrotting, and only the alias selection is gated. Measured on macOS arm64:
# hermes-node grows 337,904 bytes (12,504,968 -> 12,842,872) and the embedded
# bytecode magic count drops by exactly one, 189 -> 188. A universal binary
# pays the size per slice.
#
# An earlier measurement of the same flip, on the same platform, reported
# 338,960. Both are real: the post-flip binaries were the same size to the
# byte, and the two pre-flip baselines differed by 1,056 -- almost certainly
# because they were taken at different commits, though that was never run
# down. The number above is the one whose two absolutes are recorded here, so
# the subtraction can be checked.
#
# FORCE, unlike HERMES_ENABLE_WASM above and like the debugger: a non-FORCE
# `set(... CACHE ...)` leaves an existing cache entry alone, so every build
# directory configured before this change would silently keep Hermes's OFF and
# report the feature as not working -- the exact trap CLAUDE.md records for
# the Wasm option. Forcing costs nothing here because there is no build
# configuration that wants the bytecode form.
set(HERMESVM_INTERNAL_JAVASCRIPT_NATIVE ON CACHE BOOL
    "Use natively compiled InternalJavaScript" FORCE)

# Pull in Hermes (includes cmake modules, gtest, NAPI, etc.)
add_subdirectory(hermes)

# hermesNapi calls dlopen/dlsym/dlerror/dlclose to load native addons, but does
# not declare the dependency, so every consumer has to supply it. glibc merged
# libdl into libc at 2.34, which is why nothing noticed: on a modern host these
# resolve with no -ldl anywhere and the omission is invisible. Building against
# glibc 2.28 turns it into undefined references at the link of anything that
# pulls hermesNapi in -- our unit tests and Hermes's own `hermes` CLI included,
# though not hermes-node itself, which happens to get -ldl by another route.
#
# Attaching it to the producing target rather than to each consumer means a
# target added later cannot forget it. This belongs upstream in
# hermes/API/napi/CMakeLists.txt; it is done here so the submodule stays
# untouched. Delete this block when that lands -- see dz 01a067ba-6571, which
# records what broke and why a build-only check does not catch it.
# CMAKE_DL_LIBS is empty on platforms that need nothing.
#
# The plain signature is required, not a style choice: Hermes.cmake:141 already
# links this target plainly, and CMake refuses to mix plain and keyword forms
# on one target. Plain also propagates to consumers, which is what we want.
if (CMAKE_DL_LIBS)
  target_link_libraries(hermesNapi ${CMAKE_DL_LIBS})
endif()

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
add_subdirectory(lib/build-native)
add_subdirectory(lib/vm-options)
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
#
# hermes-parser-napi is built but not run here, and it is listed for one
# source file. HermesParserNativeTest already compiles the vendored addon's
# HermesParserJSSerializer.cpp and HermesParserDiagHandler.cpp, but not
# napi/hermes-parser-napi.cpp -- the Node-API entry point, and the only file
# of the three that includes hermes/include/hermes/napi. Nothing else in this
# target compiles it, and CI runs exactly this target, so before this line a
# submodule bump that changed the NAPI headers or the AST API that file uses
# broke nothing anyone would notice until someone ran the examples by hand.
# It also gets the MODULE link checked, which is where the deliberately
# undefined napi_* symbols and the Apple -undefined dynamic_lookup live.
#
# Costs about two seconds of compile (3.6s under ASAN, measured from clean
# objects). check-hermes-node-examples depends on it too, for the different
# reason that the examples actually load it.
#
# One side effect worth knowing: the target's POST_BUILD step stages the
# addon into the source tree at external/hermes-parser-native/package/
# prebuilds/<platform>-<arch>/, and every build directory writes that same
# path. So running this target from an ASAN build and then a Release one
# leaves whichever ran last, which matters only to something that loads the
# addon without setting HERMES_PARSER_NATIVE_ADDON. That was already true of
# a default `cmake --build` -- the target is in ALL -- so this widens an
# existing hazard rather than introducing one.
# Reconciles test/native/corpus.txt and test/native/excluded.txt against the
# tree. A gate rather than a convention: without it the corpus shrinks
# silently the first time someone edits a RUN line, which is the whole failure
# mode the two manifests exist to prevent.
#
# Part of check-hermes-node, unlike the native suite it also gates: this
# reconciliation needs no kit, no linker, no shermes and nothing built, and
# runs in well under a second. Reachable only from check-hermes-node-native it
# would run nowhere anyone runs -- that target is Release-only and absent from
# CI -- so someone adding an ordinary-shaped test would see a green
# check-hermes-node having silently left the corpus incomplete.
add_custom_target(check-hermes-node-native-corpus
  COMMAND ${Python_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/test/native/check-corpus.py
          ${CMAKE_CURRENT_SOURCE_DIR}/test
  COMMENT "Reconciling the native test corpus"
  VERBATIM
)

add_custom_target(check-hermes-node
  DEPENDS check-hermes-node-unit check-hermes-node-js hermes-parser-napi
          check-hermes-node-native-corpus
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

# The same JS tests again, each built into a standalone executable with
# build-native so that natively compiled built-in modules are exercised by
# real programs. Deliberately NOT part of check-hermes-node, for the reason
# check-hermes-node-examples is not: cost. Each test is a link, and the ASAN
# kit this project develops against is ~755 MB and links far slower than the
# Release one -- folding these into the primary suite would make the
# configuration used for everyday work the slowest one to test. Run it
# against cmake-build-release.
#
# --config-prefix=litnative is what lets one source tree carry two suites:
# lit reads test/litnative.cfg instead of test/lit.cfg and walks test/
# normally. Selection is test/native/corpus.txt, applied by a custom format.
add_lit_testsuite(check-hermes-node-native
  "Running hermes-node JS tests with native built-ins"
  ${CMAKE_CURRENT_SOURCE_DIR}/test
  PARAMS ${HERMES_NODE_LIT_PARAMS}
  ARGS --config-prefix=litnative
  DEPENDS hermes hermes-node hello_addon hermes-node-kit
          check-hermes-node-native-corpus
)
