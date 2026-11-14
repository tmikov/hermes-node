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
add_subdirectory(lib/vm-options)
add_subdirectory(lib/process)
add_subdirectory(lib/bindings)
add_subdirectory(lib/inspector)
add_subdirectory(lib/runtime)

option(HERMES_NODE_ENABLE_UI "Build the hermes-node-ui compositor" OFF)
if(HERMES_NODE_ENABLE_UI)
  # Sokol is added here rather than beside the other vendored dependencies
  # above, and the reason is the Global Constraint that a build WITHOUT this
  # option is completely unaffected. An INTERFACE library carrying one
  # include path could not plausibly affect anything -- but "could not
  # plausibly" is the argument, and keeping the target out of the graph
  # entirely is the fact.
  add_subdirectory(external/sokol)
  add_subdirectory(lib/compositor)
  # The GPU half. This one returns early on any platform without a backend
  # -- v1 GL is Linux-only and Metal is Task 16 -- so a target from it must
  # be tested for rather than assumed; see the `if(TARGET ...)` guards
  # below and in tools/hermes-node-ui.
  add_subdirectory(lib/compositor-gpu)
endif()

# CLI tools
add_subdirectory(tools/hermes-node)

# hermes-node-ui: the UI compositor binary (Task 6 of the compositor plan).
# Guarded the same way lib/compositor is above -- a build without the
# option produces neither the library nor this binary.
if(HERMES_NODE_ENABLE_UI)
  add_subdirectory(tools/hermes-node-ui)
endif()

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

# The UI compositor's reference addon (spec: The reference addon). It is a
# TEST FIXTURE rather than an examples/ entry, and deliberately so:
# examples/run-examples.sh skips any directory without a node_modules, so a
# dependency-free addon placed there would be permanently reported as not
# installed. The precedent it follows is hello_addon just above -- a .node
# addon built by our own CMake from test/fixtures/ -- and like that one its
# built path is passed to the entry script rather than resolved by
# require() from a package.
#
# if(TARGET ...) as well as the option, matching every other guard around
# this feature: lib/compositor-gpu returns early on a platform with no GPU
# backend (macOS until Task 16), where the option is on and there is
# nothing for an addon to draw into.
if(HERMES_NODE_ENABLE_UI AND TARGET hermesNodeCompositorGpu)
  add_library(hnui_demo_addon SHARED
    test/fixtures/hnui-demo/demo_addon.c
  )
  target_include_directories(hnui_demo_addon PRIVATE
    ${PROJECT_SOURCE_DIR}/hermes/include/hermes/napi
    # BOTH, and the pair is the point. An addon writes `#include <hnui.h>`
    # -- flat, the way it would against a shipped SDK directory holding
    # hnui.h, hnui_abi.h and sokol_gfx.h side by side -- so the directory
    # containing hnui.h is on the path. But hnui.h itself includes
    # <hermes/node-compat/ui/hnui_abi.h>, by its in-tree path, so the
    # include root has to be there too. Dropping either one breaks a
    # different half of the same include.
    ${PROJECT_SOURCE_DIR}/include/hermes/node-compat/ui
    ${PROJECT_SOURCE_DIR}/include
  )
  # sokol is the INTERFACE target carrying the vendored include path. The
  # addon compiles sokol_gfx.h's DECLARATIONS only; the sg_* symbols it
  # calls are exported by hermes-node-ui itself and resolved at dlopen
  # time, exactly as the NAPI surface already is.
  target_link_libraries(hnui_demo_addon PRIVATE sokol)
  # The same backend define the host is built with. Two translation units
  # of the same Sokol configured differently would disagree about struct
  # layouts while linking perfectly -- see lib/compositor-gpu/sokol_config.h.
  target_compile_definitions(hnui_demo_addon PRIVATE SOKOL_GLCORE)
  set_target_properties(hnui_demo_addon PROPERTIES
    PREFIX ""
    SUFFIX ".node"
  )
  if(APPLE)
    target_link_options(hnui_demo_addon PRIVATE -undefined dynamic_lookup)
  endif()
endif()

# JS tests (Lit-based)
set(LLVH_LIT_ARGS "-sv")
# No enable_ui (or similar) param here for HERMES_NODE_ENABLE_UI: a param
# lit trusts rather than verifies has already produced a silent green suite
# once (RULING 37) -- deleting the param, or a future param whose value
# stops matching the binary lit was actually pointed at, would make
# test/embedder-module.js and test/bundle-embedder-module.js quietly report
# UNSUPPORTED forever instead of failing loudly. test/lit.cfg instead runs
# hermes_node itself and asks it, the same way `linker-available` is decided
# by the presence of a real kit.manifest rather than a flag saying one was
# built.
set(HERMES_NODE_LIT_PARAMS
  hermes_node=${HERMES_TOOLS_OUTPUT_DIR}/hermes-node
  hermes=${HERMES_TOOLS_OUTPUT_DIR}/hermes
  FileCheck=${HERMES_TOOLS_OUTPUT_DIR}/FileCheck
  not=${HERMES_TOOLS_OUTPUT_DIR}/not
  source_dir=${CMAKE_CURRENT_SOURCE_DIR}
  test_exec_root=${CMAKE_CURRENT_BINARY_DIR}/test
  hello_addon=$<TARGET_FILE:hello_addon>
  kit_dir=${HERMES_NODE_KIT_DIR}
  # Same shape as hermes_node and kit_dir above: always a path,
  # never a flag lit is asked to trust. Passed unconditionally, even in a
  # build with HERMES_NODE_ENABLE_UI off, where this path simply names a
  # binary that does not exist -- test/lit.cfg runs it and decides the `ui`
  # feature from what actually happens, the same way `linker-available` is
  # decided by a real kit.manifest rather than a flag saying one was built.
  hermes_node_ui=${HERMES_TOOLS_OUTPUT_DIR}/hermes-node-ui
  # The interpreter CMake found, not whatever `python3` a RUN line would
  # pick off PATH. Two tests run repo scripts (utils/check-sgp-isolation.py),
  # and a suite that silently used a different interpreter from the build is
  # the same class of drift as a lit param the suite trusts rather than
  # verifies.
  python=${Python_EXECUTABLE}
  # The generated rename header and the directory holding the two compiled
  # sokol_gfx objects, for test/sokol-rename.js and test/sokol-isolation.js.
  # Passed unconditionally and as plain paths, exactly like hermes_node_ui
  # and kit_dir above: in a build with the option off they simply name files
  # that do not exist, and the `ui-enabled` feature -- decided by running
  # the binary -- is what gates the tests.
  sgp_rename_h=${CMAKE_CURRENT_BINARY_DIR}/lib/compositor-gpu/generated/sgp_rename.h
  sgp_objdir=${CMAKE_CURRENT_BINARY_DIR}/lib/compositor-gpu/CMakeFiles/hermesNodeCompositorGpu.dir
)

# hermes-node-kit is EXCLUDE_FROM_ALL (see tools/hermes-node/CMakeLists.txt:
# the merged archive is 40 MB in a Release build and 755 MB in an ASAN one),
# so an ordinary build does not produce it. The test suite depends on it
# anyway: --build-exe links against the kit, so a suite that ran with whatever
# kit happened to be lying around would be testing a link nobody performs --
# and the version check inside --build-exe would fail every test the moment
# the version string moved. Re-cutting is a few seconds; running against a
# stale kit is a silent wrong answer.
set(HERMES_NODE_JS_TEST_DEPENDS hermes hermes-node hello_addon hermes-node-kit)
# hermes-node-ui only exists as a target when HERMES_NODE_ENABLE_UI is on
# (see the guarded add_subdirectory above); naming it in DEPENDS
# unconditionally would be a hard CMake configure error on every other
# build. Guarded the same way the target itself is -- without this, a build
# with the option on could run the JS suite against a stale or missing
# binary and every `REQUIRES: ui` test would be silently UNSUPPORTED rather
# than failing for its stated reason.
if(HERMES_NODE_ENABLE_UI)
  list(APPEND HERMES_NODE_JS_TEST_DEPENDS hermes-node-ui)
endif()
# The reference addon, and the lit param naming it. Appended conditionally
# because the target only exists where a GPU backend was built -- unlike
# hermes_node_ui above, which is always a path because the BINARY always
# exists when the option is on. A $<TARGET_FILE:> generator expression
# cannot name a target that was never defined, so there is no "always a
# path" form available here; test/lit.cfg's _register_path_param() closes
# the gap from the other side, registering something that provably does
# not exist when the param is absent, so a test needing it fails loudly
# rather than silently running against a stale file.
if(TARGET hnui_demo_addon)
  list(APPEND HERMES_NODE_JS_TEST_DEPENDS hnui_demo_addon)
  list(APPEND HERMES_NODE_LIT_PARAMS
    hnui_demo_addon=$<TARGET_FILE:hnui_demo_addon>)
endif()
# test/sokol-rename.js reads the generated sgp_rename.h and
# test/sokol-isolation.js reads the two compiled sokol_gfx objects, so both
# artifacts have to exist before the suite runs -- the same reasoning that
# makes the suite DEPEND on hermes-node-kit above. if(TARGET ...) rather
# than if(HERMES_NODE_ENABLE_UI): lib/compositor-gpu returns early on a
# platform with no backend yet (macOS until Task 16), where the option is
# on and this target still does not exist.
if(TARGET hermesNodeCompositorGpu)
  list(APPEND HERMES_NODE_JS_TEST_DEPENDS hermesNodeCompositorGpu)
endif()
add_lit_testsuite(check-hermes-node-js "Running hermes-node JS tests"
  ${CMAKE_CURRENT_SOURCE_DIR}/test
  PARAMS ${HERMES_NODE_LIT_PARAMS}
  DEPENDS ${HERMES_NODE_JS_TEST_DEPENDS}
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
