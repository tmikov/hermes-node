# Copyright (c) Tzvetan Mikov.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

add_hermes_library(hermesNodeCompileCache STATIC
  cache_config.cpp
  compile_cache.cpp
)

target_include_directories(hermesNodeCompileCache
  PUBLIC
    ${PROJECT_SOURCE_DIR}/include
)

# This library talks to the filesystem through raw POSIX calls, not libuv,
# and never includes a Hermes header. zlib, for CRC32, and picohash, for
# SHA-256 (Wasm content-keyed entries), are its only dependencies -- keep it
# that way.
target_link_libraries(hermesNodeCompileCache
  PRIVATE
    zlib_a
    picohash
)

# Execution layer: binds the storage layer above to Hermes. Kept separate so
# hermesNodeCompileCache stays free of Hermes and napi, which is what lets
# CompileCacheTest exercise the storage code with no runtime.
#
# wasm_cache_hooks.cpp lives here rather than in hermesNodeCompileCache above
# for the same reason: it calls hermes_set_wasm_cache, which needs a napi_env.
add_hermes_library(hermesNodeCompileCacheRun STATIC
  compile_cache_run.cpp
  wasm_cache_hooks.cpp
)

target_include_directories(hermesNodeCompileCacheRun
  PUBLIC
    ${PROJECT_SOURCE_DIR}/include
    # compile_cache_run.h includes <node_api.h>, so consumers of the header
    # need this path too -- it cannot be PRIVATE.
    ${PROJECT_SOURCE_DIR}/hermes/include/hermes/napi
  PRIVATE
    ${PROJECT_SOURCE_DIR}/hermes/include
    # Also reaches napi/hermes_napi.h, whose WebAssembly bytecode cache
    # section wasm_cache_hooks.cpp consults -- no consumer of this target's
    # public headers needs it.
    ${PROJECT_SOURCE_DIR}/hermes/API
)

target_link_libraries(hermesNodeCompileCacheRun
  PUBLIC
    hermesNodeCompileCache
    # hermes_compile_to_bytecode / hermes_free_bytecode, used only inside
    # compile_cache_run.cpp -- compile_cache_run.h does not include Hermes
    # NAPI compile headers, so PUBLIC is not required for a consumer to
    # compile against this target. Kept PUBLIC anyway so the dependency is
    # visible on this target's link interface; PRIVATE would link identically,
    # since CMake still forwards a static library's PRIVATE dependencies via
    # $<LINK_ONLY:> for linking purposes.
    hermesNapiCompile
    # hermes_set_wasm_cache, used only inside wasm_cache_hooks.cpp. Kept
    # PUBLIC for the same reason as hermesNapiCompile above.
    hermesNapi
)

# HERMES_ENABLE_WASM only reaches Hermes's own add_definitions() scope (see
# the top-level CMakeLists.txt), so wasm_cache_hooks.cpp needs its own copy of
# the macro to guard its Wasm-specific logic out of a build that opted out of
# WebAssembly entirely.
if(HERMES_ENABLE_WASM)
  target_compile_definitions(hermesNodeCompileCacheRun PRIVATE
    HERMES_ENABLE_WASM
  )
endif()
