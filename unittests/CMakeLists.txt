# Copyright (c) Tzvetan Mikov.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

add_custom_target(NodeCompatUnitTests)
set_target_properties(NodeCompatUnitTests PROPERTIES FOLDER "Tests")

function(add_node_compat_unittest test_dirname)
  add_unittest(NodeCompatUnitTests ${test_dirname} ${ARGN})
  target_compile_definitions(${test_dirname} PUBLIC UNIT_TEST)
endfunction()
