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
    ${CMAKE_SOURCE_DIR}/hermes/external/llvh/include
    ${CMAKE_SOURCE_DIR}/hermes/external/llvh/gen/include
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
  hermesvm_a
)
# hermes_napi.h (internal) lives in hermes/API/napi/.
# hermesvm_a propagates most Hermes includes, but we also need:
#   - hermes/API/napi/ for hermes_napi.h
#   - Generated config header in the build tree
target_include_directories(BindingRegistryTest PRIVATE
  ${CMAKE_SOURCE_DIR}/hermes/API/napi
  # Generated config (libhermesvm-config.h) — not propagated from Hermes scope.
  ${CMAKE_BINARY_DIR}/hermes/lib/config
)
# Hermes uses add_definitions(-DHERMESVM_GC_${HERMESVM_GCKIND}) inside its
# subdirectory scope. Targets outside that scope need the define explicitly
# when including Hermes VM headers (e.g. hermes_napi.h).
target_compile_definitions(BindingRegistryTest PRIVATE
  HERMESVM_GC_${HERMESVM_GCKIND}
)

add_node_compat_unittest(ModuleLoaderTest ModuleLoaderTest.cpp)
target_link_libraries(ModuleLoaderTest
  hermesNodeModuleLoader
  hermesNodeBindingRegistry
  hermesvm_a
)
target_include_directories(ModuleLoaderTest PRIVATE
  ${CMAKE_SOURCE_DIR}/hermes/API/napi
  ${CMAKE_BINARY_DIR}/hermes/lib/config
)
target_compile_definitions(ModuleLoaderTest PRIVATE
  HERMESVM_GC_${HERMESVM_GCKIND}
  LIBJS_PATH="${CMAKE_SOURCE_DIR}/libjs/"
  TEST_DATA_PATH="${CMAKE_SOURCE_DIR}/unittests/module-loader-testdata/"
)

add_node_compat_unittest(NodeProcessTest NodeProcessTest.cpp)
target_link_libraries(NodeProcessTest
  hermesNodeProcess
  hermesvm_a
)
target_include_directories(NodeProcessTest PRIVATE
  ${CMAKE_SOURCE_DIR}/hermes/API/napi
  ${CMAKE_BINARY_DIR}/hermes/lib/config
)
target_compile_definitions(NodeProcessTest PRIVATE
  HERMESVM_GC_${HERMESVM_GCKIND}
)
