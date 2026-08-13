# Copyright (c) Tzvetan Mikov.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

add_hermes_library(hermesNodeCompileCache STATIC
  compile_cache.cpp
)

target_include_directories(hermesNodeCompileCache
  PUBLIC
    ${PROJECT_SOURCE_DIR}/include
)

# This library talks to the filesystem through raw POSIX calls, not libuv,
# and never includes a Hermes header. zlib, for CRC32, is its only
# dependency -- keep it that way.
target_link_libraries(hermesNodeCompileCache
  PRIVATE
    zlib_a
)

# Execution layer: binds the storage layer above to Hermes. Kept separate so
# hermesNodeCompileCache stays free of Hermes and napi, which is what lets
# CompileCacheTest exercise the storage code with no runtime.
add_hermes_library(hermesNodeCompileCacheRun STATIC
  compile_cache_run.cpp
)

target_include_directories(hermesNodeCompileCacheRun
  PUBLIC
    ${PROJECT_SOURCE_DIR}/include
    # compile_cache_run.h includes <node_api.h>, so consumers of the header
    # need this path too -- it cannot be PRIVATE.
    ${PROJECT_SOURCE_DIR}/hermes/include/hermes/napi
  PRIVATE
    ${PROJECT_SOURCE_DIR}/hermes/include
    ${PROJECT_SOURCE_DIR}/hermes/API
)

target_link_libraries(hermesNodeCompileCacheRun
  PUBLIC
    hermesNodeCompileCache
    # hermes_compile_to_bytecode / hermes_free_bytecode. PUBLIC so consumers
    # of this static library link it transitively, same as hermesNapi below.
    hermesNapiCompile
)
