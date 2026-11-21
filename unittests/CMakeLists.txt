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

add_node_compat_unittest(BuildExeTest BuildExeTest.cpp)
target_link_libraries(BuildExeTest hermesNodeBuildExe)

add_node_compat_unittest(VmOptionsTest VmOptionsTest.cpp)
target_link_libraries(VmOptionsTest hermesNodeVmOptions)

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

if(HERMES_NODE_ENABLE_UI)
  add_node_compat_unittest(CompositorTest CompositorTest.cpp)
  target_link_libraries(CompositorTest hermesNodeCompositor)

  # hermesNodeUiHost (tools/hermes-node-ui/CMakeLists.txt) is the split
  # RULING 41 introduced so this could link it directly. hermesNapi and
  # hermesvm_a are added explicitly, the same way ModuleLoaderTest and
  # NodeProcessTest do above: hermesNodeUiHost pulls in hermesNodeRuntime,
  # which links hermesvm_a PUBLIC already, but a plain (non-whole-archive)
  # link needs hermesNapi named too for the NAPI symbols runHermesNode()
  # calls into -- this binary dlopens no addons of its own, so it does not
  # need the whole-archive treatment hermes_node_link_setup() gives the
  # real binaries, only the symbols it actually calls.
  add_node_compat_unittest(UiEventsTest UiEventsTest.cpp)
  target_link_libraries(UiEventsTest hermesNodeUiEvents)

  add_node_compat_unittest(UiHostTest UiHostTest.cpp)
  target_link_libraries(UiHostTest
    hermesNodeUiHost
    hermesNapi
    hermesvm_a
  )

  # HnuiAbiCProbe.c is compiled as C, not C++, and deliberately so: it is
  # what enforces hnui_abi.h's "stays compilable as plain C" claim (see the
  # header's own "C99 is the floor" comment) -- without it, nothing in this
  # build notices a C++-only construct creeping into that header. Its C
  # standard is set independently of HnuiAbiTest.cpp's C++17, which
  # CXX_STANDARD below leaves untouched.
  # sokol is an INTERFACE target carrying one include path, added at the
  # top level before this directory. Linking it here is what lets
  # HnuiAbiTest.cpp include sokol_gfx.h's declarations and assert
  # hnui_gpu_view against the real sg_view on EVERY platform the UI option
  # is on for -- including macOS, where gl_backend.cpp (and so the
  # RULING 45 copy of those asserts) is not built. See RULING 46 and
  # the comment at those assertions.
  add_node_compat_unittest(HnuiAbiTest HnuiAbiTest.cpp HnuiAbiCProbe.c)
  target_link_libraries(HnuiAbiTest hermesNodeCompositor sokol)
  set_target_properties(HnuiAbiTest PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
  )
  set_source_files_properties(HnuiAbiCProbe.c PROPERTIES COMPILE_OPTIONS "-pedantic")
  # hnui.h includes node_api.h, and HnuiAbiCProbe.c exercises
  # HNUI_CHECK_VERSION from plain C99. Declarations only: the test defines
  # the three functions the macro calls itself.
  target_include_directories(HnuiAbiTest PRIVATE
    ${PROJECT_SOURCE_DIR}/hermes/include/hermes/napi)

  # test_hooks.{h,cpp} (lib/compositor-gpu/, Task 9) has no GL, X11 or
  # Metal dependency of its own -- it only touches SurfaceTable and UiEvent
  # -- but it is compiled into hermesNodeCompositorGpu, which exists
  # wherever the option configured (that directory refuses any platform
  # without a backend).
  add_node_compat_unittest(TestHooksTest TestHooksTest.cpp)
  target_link_libraries(TestHooksTest hermesNodeCompositorGpu)

  # A binary of its own, because it turns tracing on for the whole
  # process: HERMES_NODE_DEBUG_NATIVE is read once, on first use.
  add_node_compat_unittest(UiTraceTest UiTraceTest.cpp)
  target_link_libraries(UiTraceTest hermesNodeCompositorGpu)
endif()

add_subdirectory(HermesParserNative)
