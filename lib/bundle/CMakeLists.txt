# Copyright (c) Tzvetan Mikov.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# Format layer: no napi, and no link dependency on the Hermes VM. This is
# what lets BundleFormatTest exercise the container with no runtime.
# bundle_generation.cpp includes a Hermes header for BYTECODE_VERSION, which
# is header-only -- it does not link hermesvm_a or any other Hermes library,
# so BundleFormatTest still runs with no runtime.
# mapped_file.cpp is POSIX (open/fstat/mmap) and nothing more, so it does
# not cost this target its VM-free property. It lives here rather than in
# the tools layer because the run layer maps a container too, and one copy
# of that syscall sequence is the point.
# atomic_write.cpp is POSIX (open/write/close/rename) and nothing more, for
# the same reason and the same shape: the producer (bundle_build.cpp) and
# the extractor (bundle_tools.cpp) both need the temp-file-then-rename
# guarantee, and this is one copy of it rather than two.
add_hermes_library(hermesNodeBundle STATIC
  bundle_writer.cpp
  bundle_reader.cpp
  bundle_generation.cpp
  mapped_file.cpp
  atomic_write.cpp
)

target_include_directories(hermesNodeBundle
  PUBLIC
    ${PROJECT_SOURCE_DIR}/include
  PRIVATE
    ${PROJECT_SOURCE_DIR}/hermes/include
    ${CMAKE_BINARY_DIR}/generated
)

# version.h is re-derived on every build (see cmake/version.cmake); this
# orders that ahead of the compile that includes it.
add_dependencies(hermesNodeBundle hermes-node-version)

target_link_libraries(hermesNodeBundle PRIVATE
  zlib_a
  # HERMES_NODE_CACHE_ARCH, single-sourced (see top-level CMakeLists.txt) so
  # this and the compile cache's generation name (lib/runtime) can never
  # disagree on what "architecture" means.
  hermesNodeCacheArch
)

# Build layer: static discovery of require() calls, using the Hermes parser.
# Kept as a separate target from hermesNodeBundle so that BundleFormatTest and
# any other consumer of the format layer alone stays free of the Hermes VM
# link dependency; only producer-side code that needs to scan and compile
# sources links this one.
add_hermes_library(hermesNodeBundleBuild STATIC
  require_scanner.cpp
  bundle_resolve.cpp
  bundle_build.cpp
)

target_include_directories(hermesNodeBundleBuild
  PUBLIC
    ${PROJECT_SOURCE_DIR}/include
    # bundle_build.h includes <node_api.h>, so consumers of that header need
    # this path too -- it cannot be PRIVATE. Matches
    # hermesNodeCompileCacheRun's identical need (lib/compile-cache).
    ${PROJECT_SOURCE_DIR}/hermes/include/hermes/napi
  PRIVATE
    ${PROJECT_SOURCE_DIR}/hermes/include
    ${PROJECT_SOURCE_DIR}/hermes/API
)

target_link_libraries(hermesNodeBundleBuild
  PUBLIC
    hermesNodeBundle
    # hermes_compile_to_bytecode / hermes_free_bytecode, used only inside
    # bundle_build.cpp -- bundle_build.h does not include Hermes NAPI compile
    # headers, so PUBLIC is not required for a consumer to compile against
    # this target. Kept PUBLIC anyway to mirror hermesNodeCompileCacheRun's
    # choice (see its CMakeLists.txt for the reasoning): PRIVATE would link
    # identically, since CMake still forwards a static library's PRIVATE
    # dependencies via $<LINK_ONLY:> for linking purposes.
    hermesNapiCompile
  PRIVATE
    hermesvm_a
)

# Run layer: maps a container and executes bytecode straight out of it. Kept
# separate from hermesNodeBundleBuild for the same reason that one is kept
# separate from hermesNodeBundle: running a bundle needs neither the parser
# nor the compiler, only hermes_run_bytecode.
add_hermes_library(hermesNodeBundleRun STATIC
  bundle_run.cpp
)

target_include_directories(hermesNodeBundleRun
  PUBLIC
    ${PROJECT_SOURCE_DIR}/include
    # bundle_run.h includes <node_api.h>, so consumers of that header need
    # this path too -- it cannot be PRIVATE.
    ${PROJECT_SOURCE_DIR}/hermes/include/hermes/napi
  PRIVATE
    ${PROJECT_SOURCE_DIR}/hermes/include
    ${PROJECT_SOURCE_DIR}/hermes/API
)

target_link_libraries(hermesNodeBundleRun
  PUBLIC
    hermesNodeBundle
    # hermes_run_bytecode, used only inside bundle_run.cpp. PUBLIC so the
    # dependency stays visible on this target's link interface, matching
    # hermesNodeCompileCacheRun's choice for hermesNapiCompile.
    hermesNapi
)
