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
