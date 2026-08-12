# Copyright (c) Tzvetan Mikov.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# Note the singular "Test" suffix: unittests/lit.cfg discovers GoogleTest
# binaries by that suffix, so the fork's "HermesParserNativeTests" would
# never be collected here.
set(HermesParserNativeTestSources
  StringTableTest.cpp
  KindHashTest.cpp
  ContainerWriterTest.cpp
  SerializerTest.cpp
  SourcePositionMapTest.cpp
  ${PROJECT_SOURCE_DIR}/external/hermes-parser-native/napi/HermesParserJSSerializer.cpp
  ${PROJECT_SOURCE_DIR}/external/hermes-parser-native/napi/HermesParserDiagHandler.cpp
)

add_node_compat_unittest(HermesParserNativeTest
  ${HermesParserNativeTestSources}
)

target_include_directories(HermesParserNativeTest PRIVATE
  ${PROJECT_SOURCE_DIR}/external/hermes-parser-native/napi
  ${PROJECT_SOURCE_DIR}/hermes/include
)

target_link_libraries(HermesParserNativeTest
  hermesAST
  hermesParser
  hermesSema
  LLVHSupport
)
