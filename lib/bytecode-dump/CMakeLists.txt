# Copyright (c) Tzvetan Mikov.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# Disassembly layer: the only part of the bundle tooling that needs the
# Hermes VM libraries, which is why it is a target of its own rather than
# more source in lib/bundle. hermesNodeBundle and hermesNodeBundleTools stay
# free of any Hermes VM link dependency, and BundleFormatTest and
# BundleToolsTest keep running with no runtime because of it.
#
# It reads a file of bytecode; the fact that one can be extracted from a
# container is not its concern, so it links neither the bundle format nor
# the compile cache. It does include one header-only magic constant from
# each (bundle_format.h, compile_cache.h) to recognize a file this binary
# wrote and say which one it is -- reads of those formats, not uses of them,
# and they cost no link dependency: hermesvm_a below is the only library
# this target links.
add_hermes_library(hermesNodeBytecodeDump STATIC
  bytecode_dump.cpp
)

target_include_directories(hermesNodeBytecodeDump
  PUBLIC
    ${PROJECT_SOURCE_DIR}/include
  PRIVATE
    ${PROJECT_SOURCE_DIR}/hermes/include
    # llvh is only in scope inside the hermes subdirectory; the disassembler
    # headers reach into it (raw_ostream, MemoryBuffer), so name it here the
    # same way unittests/CMakeLists.txt has to.
    ${PROJECT_SOURCE_DIR}/hermes/external/llvh/include
    ${PROJECT_SOURCE_DIR}/hermes/external/llvh/gen/include
    ${CMAKE_BINARY_DIR}/hermes/external/llvh/include
)

target_link_libraries(hermesNodeBytecodeDump PRIVATE hermesvm_a)
