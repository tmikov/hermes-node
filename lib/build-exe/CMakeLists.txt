# Copyright (c) Tzvetan Mikov.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# Producer for --build-exe. VM-free by construction -- it reads an
# already-compiled container and runs the toolchain, so it links the format
# layer and nothing else. That is what lets BuildExeTest run with no
# runtime, the same property BundleFormatTest and BundleToolsTest rely on.
add_hermes_library(hermesNodeBuildExe STATIC
  kit_manifest.cpp
  build_exe.cpp
)

target_include_directories(hermesNodeBuildExe
  PUBLIC
    ${PROJECT_SOURCE_DIR}/include
  PRIVATE
    # The generated version.h: build_exe.cpp compares the kit's recorded
    # version against this binary's, so a kit cut from a different build
    # than the producer is refused rather than linked.
    ${CMAKE_BINARY_DIR}/generated)

# version.h is re-derived on every build (see cmake/version.cmake); this
# orders that ahead of the compile that includes it.
add_dependencies(hermesNodeBuildExe hermes-node-version)

target_link_libraries(hermesNodeBuildExe PUBLIC hermesNodeBundle)
