# Copyright (c) Tzvetan Mikov.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

add_custom_target(NodeCompatUnitTests)
set_target_properties(NodeCompatUnitTests PROPERTIES FOLDER "Tests")

function(add_node_compat_unittest test_dirname)
  add_unittest(NodeCompatUnitTests ${test_dirname} ${ARGN})
  target_compile_definitions(${test_dirname} PUBLIC UNIT_TEST)
  # gtest headers reference llvh, which is only in scope inside the hermes
  # subdirectory.  Propagate the necessary include paths here.
  target_include_directories(${test_dirname} PRIVATE
    ${PROJECT_SOURCE_DIR}/hermes/external/llvh/include
    ${PROJECT_SOURCE_DIR}/hermes/external/llvh/gen/include
    ${CMAKE_BINARY_DIR}/hermes/external/llvh/include
  )
endfunction()

add_node_compat_unittest(UvIntegrationTest UvIntegrationTest.cpp)
target_link_libraries(UvIntegrationTest uv_a)

add_node_compat_unittest(UvEventLoopTest UvEventLoopTest.cpp)
target_link_libraries(UvEventLoopTest hermesNodeEventLoop)

add_node_compat_unittest(BindingRegistryTest BindingRegistryTest.cpp)
target_link_libraries(BindingRegistryTest
  hermesNodeBindingRegistry
  hermesNapi
  hermesvm_a
)

add_node_compat_unittest(ModuleLoaderTest ModuleLoaderTest.cpp)
target_link_libraries(ModuleLoaderTest
  hermesNodeEmbeddedModules
  hermesNodeModuleLoader
  hermesNodeBindingRegistry
  hermesNapi
  hermesvm_a
)

add_node_compat_unittest(CompileCacheTest CompileCacheTest.cpp)
target_link_libraries(CompileCacheTest hermesNodeCompileCache)

add_node_compat_unittest(CompileCacheRunTest CompileCacheRunTest.cpp)
target_link_libraries(CompileCacheRunTest
  hermesNodeCompileCacheRun
  hermesNapi
  hermesvm_a
)

add_node_compat_unittest(BundleFormatTest BundleFormatTest.cpp)
target_link_libraries(BundleFormatTest hermesNodeBundle)

# No VM, exactly like BundleFormatTest: the tools layer reads a container's
# tables and copies bytes, so its whole surface is testable with no runtime.
add_node_compat_unittest(BundleToolsTest BundleToolsTest.cpp)
target_link_libraries(BundleToolsTest hermesNodeBundleTools)

add_node_compat_unittest(RequireScannerTest RequireScannerTest.cpp)
target_link_libraries(RequireScannerTest hermesNodeBundleBuild)

# hermesNodeBundle, not hermesNodeBundleBuild: resolveSpecifier()
# (bundle_resolve.cpp) and both FileSource backends live in the VM-free
# format layer, so neither this test nor the agreement test below needs the
# parser, the compiler, or hermesvm_a to link.
add_node_compat_unittest(BundleResolveTest BundleResolveTest.cpp)
target_link_libraries(BundleResolveTest hermesNodeBundle)

add_node_compat_unittest(BundleFileSourceTest BundleFileSourceTest.cpp)
target_link_libraries(BundleFileSourceTest hermesNodeBundle)

add_node_compat_unittest(NodeProcessTest NodeProcessTest.cpp)
target_link_libraries(NodeProcessTest
  hermesNodeProcess
  hermesNapi
  hermesvm_a
)

add_node_compat_unittest(CAresIntegrationTest CAresIntegrationTest.cpp)
target_link_libraries(CAresIntegrationTest cares_a)

add_node_compat_unittest(LlhttpIntegrationTest LlhttpIntegrationTest.cpp)
target_link_libraries(LlhttpIntegrationTest llhttp_a)

# Checks that the node-kind hash committed in the vendored
# hermes-parser-native JavaScript package still matches the one the addon
# derives from the hermes/ submodule's ESTree.def. It lives here rather than
# in unittests/HermesParserNative/, whose sources are a verbatim copy from the
# fork and stay copyable in one directory operation.
add_node_compat_unittest(HermesParserKindHashSyncTest
  HermesParserKindHashSyncTest.cpp
)
target_include_directories(HermesParserKindHashSyncTest PRIVATE
  ${PROJECT_SOURCE_DIR}/external/hermes-parser-native/napi
  ${PROJECT_SOURCE_DIR}/hermes/include
)
# The committed JavaScript is read at run time rather than transcribed into
# the test, so the test cannot pass against a number that is no longer in the
# file it names.
target_compile_definitions(HermesParserKindHashSyncTest PRIVATE
  HPN_KIND_HASH_SRC_JS="${PROJECT_SOURCE_DIR}/external/hermes-parser-native/package/src/HermesParserKindHash.js"
  HPN_KIND_HASH_DIST_JS="${PROJECT_SOURCE_DIR}/external/hermes-parser-native/package/dist/HermesParserKindHash.js"
)

add_subdirectory(HermesParserNative)
