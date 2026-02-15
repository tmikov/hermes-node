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
