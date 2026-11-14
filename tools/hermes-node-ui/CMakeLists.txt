# Copyright (c) Tzvetan Mikov.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# hermes-node-ui: the UI compositor binary. The top-level CMakeLists.txt
# only add_subdirectory()s this directory when HERMES_NODE_ENABLE_UI is on,
# so a build without the option produces no trace of it -- no target, no
# binary, nothing for the rest of this file to guard a second time.

# UiHost is its own library rather than compiled straight into the
# hermes-node-ui executable, so unittests/UiHostTest.cpp can link it too
# (RULING 41): the predicate's two halves are each only safely testable
# with the code under test in a process a GTest controls directly -- a lit
# test alone cannot bound a runtime-finished-only regression (the suite
# configures no per-test timeout, so a hang there blocks a worker slot
# indefinitely) or exercise the window-requested branch (nothing in this
# build can call requestWindow() from a real script). Header and source
# stay in this directory, per the plan's File Structure; only how they are
# compiled changes.
add_hermes_library(hermesNodeUiHost STATIC ui_host.cpp)
target_include_directories(hermesNodeUiHost PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(hermesNodeUiHost PUBLIC hermesNodeRuntime uv_a)

add_hermes_tool(hermes-node-ui main.cpp)
# Plain signature, not PRIVATE: hermes_node_link_setup() below links this
# same target with the plain form (matching Hermes.cmake:141's own plain
# link of hermesNapi, per that file's comment), and CMake refuses to mix
# keyword and plain target_link_libraries calls against one target.
target_link_libraries(hermes-node-ui hermesNodeUiHost)

# hermes_node_link_setup() is defined in tools/hermes-node/CMakeLists.txt.
# CMake functions have ordinary (global) scope once defined -- unlike a
# variable, which is directory-scoped -- so this call is valid only because
# the top-level CMakeLists.txt add_subdirectory()s tools/hermes-node before
# tools/hermes-node-ui. Reusing it rather than duplicating its body gives
# hermes-node-ui the same NAPI whole-archive link, -rdynamic/export_dynamic
# export and dead-stripping hermes-node itself gets: both binaries link
# hermesNodeRuntime, and both need a dlopen'd addon to resolve the same NAPI
# surface, so the two link recipes must not be allowed to drift apart.
hermes_node_link_setup(hermes-node-ui)
