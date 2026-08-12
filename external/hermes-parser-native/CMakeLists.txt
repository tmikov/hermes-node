# Copyright (c) Tzvetan Mikov.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# Wrapper CMakeLists.txt for the vendored hermes-parser-native addon.
#
# Declares the target here rather than using napi/CMakeLists.txt: that file
# hardcodes ${CMAKE_SOURCE_DIR}/include/hermes/napi, which points at the
# Hermes root in the tree it came from and at this repository's root here.
#
# A Node-API addon, built as a MODULE library named "hermes-parser.node".
# The napi_* symbols are deliberately left undefined: they resolve from the
# host process at dlopen time, which is what lets one binary work in both
# Node and hermes-node.
add_library(hermes-parser-napi MODULE
  napi/hermes-parser-napi.cpp
  napi/HermesParserJSSerializer.cpp
  napi/HermesParserDiagHandler.cpp
)

target_link_libraries(hermes-parser-napi
  hermesAST
  hermesParser
  hermesSema
  LLVHSupport
)

set_target_properties(hermes-parser-napi PROPERTIES
  PREFIX ""
  OUTPUT_NAME "hermes-parser"
  SUFFIX ".node"
  POSITION_INDEPENDENT_CODE ON
)

target_include_directories(hermes-parser-napi PRIVATE
  ${PROJECT_SOURCE_DIR}/hermes/include/hermes/napi
  ${PROJECT_SOURCE_DIR}/hermes/include
  ${PROJECT_SOURCE_DIR}/hermes/external/llvh/include
  ${PROJECT_BINARY_DIR}/hermes/external/llvh/include
)

target_compile_definitions(hermes-parser-napi PRIVATE
  NODE_GYP_MODULE_NAME=hermes_parser
)

if (APPLE)
  # Allow napi_* to be resolved by the loading process.
  target_link_options(hermes-parser-napi PRIVATE
    "SHELL:-undefined dynamic_lookup")
endif ()

# The package's loader (package/dist/HermesParserAddon.js) looks first at
# package/prebuilds/<platform>-<arch>/hermes-parser.node. Putting the build
# output there lets the vendored package behave exactly like a published
# one, so the example needs no environment variable.
#
# The directory is gitignored. Note that several build directories write to
# the same place, so the last build wins; anything that cares which addon it
# loads should set HERMES_PARSER_NATIVE_ADDON explicitly.
if (CMAKE_SYSTEM_NAME STREQUAL "Darwin")
  set(HPN_PLATFORM "darwin")
else ()
  set(HPN_PLATFORM "linux")
endif ()
if (CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
  set(HPN_ARCH "arm64")
else ()
  set(HPN_ARCH "x64")
endif ()

set(HPN_PREBUILD_DIR
  ${CMAKE_CURRENT_SOURCE_DIR}/package/prebuilds/${HPN_PLATFORM}-${HPN_ARCH})

add_custom_command(TARGET hermes-parser-napi POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory ${HPN_PREBUILD_DIR}
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          $<TARGET_FILE:hermes-parser-napi>
          ${HPN_PREBUILD_DIR}/hermes-parser.node
  COMMENT "Staging hermes-parser.node into the vendored package"
  VERBATIM
)

# Regenerating dist/ from src/ needs npm and Babel. Deliberately excluded
# from ALL: dist/ is committed so that a normal build needs no JavaScript
# toolchain. Run explicitly after editing src/, then commit the result.
add_custom_target(hermes-parser-native-dist
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/scripts/regen-dist.sh
          ${PROJECT_SOURCE_DIR}/hermes/include
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  COMMENT "Regenerating vendored hermes-parser-native dist/"
  VERBATIM
)
