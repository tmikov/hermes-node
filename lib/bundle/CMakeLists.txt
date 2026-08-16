# Copyright (c) Tzvetan Mikov.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# Format layer: no napi, and no link dependency on the Hermes VM. This is
# what lets BundleFormatTest exercise the container with no runtime.
# bundle_generation.cpp includes a Hermes header for BYTECODE_VERSION, which
# is header-only -- it does not link hermesvm_a or any other Hermes library,
# so BundleFormatTest still runs with no runtime.
# mapped_file.cpp is POSIX (open/fstat/mmap) and nothing more, so it does
# not cost this target its VM-free property. It lives here rather than in
# the tools layer because the run layer maps a container too, and one copy
# of that syscall sequence is the point.
# atomic_write.cpp is POSIX (open/write/close/rename) and nothing more, for
# the same reason and the same shape: the producer (bundle_build.cpp) and
# the extractor (bundle_tools.cpp) both need the temp-file-then-rename
# guarantee, and this is one copy of it rather than two.
add_hermes_library(hermesNodeBundle STATIC
  bundle_writer.cpp
  bundle_reader.cpp
  bundle_generation.cpp
  mapped_file.cpp
  atomic_write.cpp
)

target_include_directories(hermesNodeBundle
  PUBLIC
    ${PROJECT_SOURCE_DIR}/include
  PRIVATE
    ${PROJECT_SOURCE_DIR}/hermes/include
    ${CMAKE_BINARY_DIR}/generated
)

# version.h is re-derived on every build (see cmake/version.cmake); this
# orders that ahead of the compile that includes it.
add_dependencies(hermesNodeBundle hermes-node-version)

target_link_libraries(hermesNodeBundle PRIVATE
  zlib_a
  # HERMES_NODE_CACHE_ARCH, single-sourced (see top-level CMakeLists.txt) so
  # this and the compile cache's generation name (lib/runtime) can never
  # disagree on what "architecture" means.
  hermesNodeCacheArch
)
