# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# A Node-API addon. Built as a MODULE library named "hermes-parser.node".
# The napi_* symbols are deliberately left undefined: they resolve from the
# host process at dlopen time, which is what allows one binary to work in
# both Node and any other Node-API host.
add_library(hermes-parser-napi MODULE
  hermes-parser-napi.cpp
  HermesParserJSSerializer.cpp
  HermesParserDiagHandler.cpp
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
  ${CMAKE_SOURCE_DIR}/include/hermes/napi
  )

target_compile_definitions(hermes-parser-napi PRIVATE
  NODE_GYP_MODULE_NAME=hermes_parser
  )

if (APPLE)
  # Allow napi_* to be resolved by the loading process.
  target_link_options(hermes-parser-napi PRIVATE
    "SHELL:-undefined dynamic_lookup")
endif ()
