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

# Stage the built addon into package/prebuilds/<platform>-<arch>/, which is
# the last of the three locations the package's loader
# (package/dist/HermesParserAddon.js) tries, in this order:
#
#   1. $HERMES_PARSER_NATIVE_ADDON, if set and non-empty.
#   2. A hardcoded <package>/../../../../../cmake-build-debug/tools/
#      hermes-parser-native/hermes-parser.node. That path only resolves
#      inside the fork's Hermes tree; this repository never produces a
#      tools/hermes-parser-native/ directory, so it is dead here.
#   3. package/prebuilds/<platform>-<arch>/hermes-parser.node.
#
# So this step is what makes step 3 work, and step 3 is the only one that
# works in this repository without help from the caller. Nothing on this
# branch actually takes it: examples/flow-bundler/run.sh sets
# HERMES_PARSER_NATIVE_ADDON unconditionally, precisely so that it tests the
# build directory it was given rather than whatever was staged last. The step
# is kept anyway, because it is what lets anything that is *not* run.sh -- a
# scratch script, a manual `require('hermes-parser')`, an npm consumer of the
# vendored package -- load the addon with no environment variable at all.
#
# Two hazards, neither of which affects run.sh:
#
# - This writes into the source tree, not the build tree, and every build
#   directory writes to the same path. The last build to run wins, and a
#   read-only source checkout cannot build at all. Anything that cares which
#   addon it loads should set HERMES_PARSER_NATIVE_ADDON explicitly.
# - examples/flow-bundler/.npmrc sets install-links=true, so npm *copies* the
#   package at install time. An installed consumer therefore holds a snapshot
#   of whatever was in prebuilds/ when `npm install` ran, not a live link to
#   it.
#
# The prebuilds/ directory is gitignored.
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
