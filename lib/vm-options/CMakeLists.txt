# Copyright (c) Tzvetan Mikov.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# The classifier and the RuntimeConfig mapping. Links hermesvm_a because
# hermes/VM/RuntimeFlags.h instantiates llvh::cl options and
# hermes/Public/RuntimeConfig.h defines the config being built.
#
# It must NOT link hermesCompilerDriver: that library defines the
# hermes::cl::compilerRuntimeFlags global, which registers the same option
# names this library's RuntimeFlags instance does, and llvh::cl aborts on a
# duplicate registration.
add_hermes_library(hermesNodeVmOptions STATIC
  vm_options.cpp
)

target_include_directories(hermesNodeVmOptions PUBLIC
  ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(hermesNodeVmOptions PUBLIC hermesvm_a)

# hermes/VM/RuntimeFlags.h includes hermes/ConsoleHost/ConsoleHost.h, which
# includes hermes/VM/Runtime.h, whose member layout branches on these two
# defines. Hermes sets them with directory-scoped add_definitions() in
# hermes/CMakeLists.txt, so they do not reach this directory, and without
# them this translation unit would see a different vm::Runtime than every
# Hermes TU does. Nothing here names a type from Runtime.h today (see the
# comment at the include in vm_options.cpp), so that mismatch is currently
# benign -- but "benign" is a property nothing enforces, and the next edit
# would break it with wrong field offsets rather than a diagnostic.
# Propagated from the same CMake variables Hermes tests, so the two cannot
# disagree.
if (HERMES_CHECK_NATIVE_STACK)
  target_compile_definitions(hermesNodeVmOptions PRIVATE HERMES_CHECK_NATIVE_STACK)
endif()
if (HERMES_MEMORY_INSTRUMENTATION)
  target_compile_definitions(hermesNodeVmOptions PRIVATE HERMES_MEMORY_INSTRUMENTATION)
endif()
