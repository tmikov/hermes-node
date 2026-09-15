# Copyright (c) Tzvetan Mikov.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# Producer for `build-native`. VM-free by construction: it stages sources,
# builds command lines and drives subprocesses, so it links the format layer
# and the build-exe layer and nothing else. That is what lets
# BuildNativeTest run with no runtime and no Hermes compiler -- the same
# property BundleFormatTest, BundleToolsTest and BuildExeTest rely on.
add_hermes_library(hermesNodeBuildNative STATIC
  staging.cpp
  native_compile.cpp
  job_pool.cpp
  unit_table.cpp
)

target_include_directories(hermesNodeBuildNative
  PUBLIC ${PROJECT_SOURCE_DIR}/include)

target_link_libraries(hermesNodeBuildNative PUBLIC
  hermesNodeBundle
  hermesNodeBuildExe)
