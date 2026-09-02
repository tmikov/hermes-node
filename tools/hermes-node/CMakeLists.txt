# Copyright (c) Tzvetan Mikov.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# Everything that makes a hermes-node-shaped link: the libraries, the
# whole-archive NAPI dance, dynamic export, and dead-stripping. Applied to
# the real binary and to the kit probe below, so that the kit is cut from
# exactly the closure the binary links and the two cannot drift.
function(hermes_node_link_setup target)
  target_link_libraries(${target}
    hermesNodeRuntime
    # The read-only verbs are dispatched from main(), before any runtime
    # exists, so the tools layer is linked here rather than reached through
    # hermesNodeRuntime.
    hermesNodeBundleTools
    # --dump-bytecode, the one verb that needs the Hermes disassembler. Its
    # own target so that hermesNodeBundleTools stays VM-free; see
    # lib/bytecode-dump/CMakeLists.txt.
    hermesNodeBytecodeDump
    # --build-exe, the verb that reads an already-built container and links
    # a standalone executable from it. Also dispatched from main() before
    # any runtime exists, and also VM-free; see lib/build-exe/CMakeLists.txt.
    hermesNodeBuildExe
  )

  target_include_directories(${target} PRIVATE
    ${CMAKE_BINARY_DIR}/generated
  )

  # version.h is re-derived on every build (see cmake/version.cmake); this
  # orders that ahead of the compile that includes it.
  add_dependencies(${target} hermes-node-version)

  # hermesNapi was removed from hermesvm_a upstream (54c50a06a).
  # We must whole-archive link it so all NAPI symbols are available.
  if(APPLE)
    target_link_libraries(${target} -force_load hermesNapi)
  elseif(MSVC)
    target_link_libraries(${target} hermesNapi)
    target_link_options(${target} PRIVATE /WHOLEARCHIVE:hermesNapi)
  else()
    target_link_libraries(${target}
      -Wl,--whole-archive
      hermesNapi
      -Wl,--no-whole-archive
    )
  endif()

  # Export NAPI symbols so dlopen'd addons can resolve them.
  if(APPLE)
    target_link_options(${target} PRIVATE -Wl,-export_dynamic)
  elseif(UNIX)
    target_link_options(${target} PRIVATE -rdynamic)
  endif()

  # Drop sections nothing references. Everything linked here is compiled with
  # -ffunction-sections -fdata-sections (Hermes sets them in its own scope; the
  # top-level CMakeLists.txt sets them for our libraries and the vendored
  # externals), so the granularity is a function or a variable rather than a
  # whole object file.
  #
  # This does not weaken the -rdynamic above: a symbol that ends up in the
  # dynamic symbol table is a GC root, so the exported NAPI surface an addon
  # resolves against survives. Measured on Linux x86_64, Release: 13,963,320 ->
  # 12,831,944 bytes (-8.1%), with the dynamic export count unchanged (3717
  # total, 145 napi_).
  #
  # Identical code folding is deliberately not enabled on Linux. The only linker
  # on this machine that offers it is GNU gold (--icf=safe: 12,442,216, a further
  # -3.0%), and gold is deprecated upstream and absent from many toolchains, so
  # -fuse-ld=gold would be a build requirement bought with a behavioral change --
  # ICF makes distinct functions share an address. lld, the linker that would
  # make this worth revisiting, is not installed here and the flag was not going
  # to be added untested. macOS needs nothing: ld64 folds by default.
  #
  # LTO is out of scope by design, not by measurement: it produces bitcode
  # objects that need a matching linker plugin on the end user's machine, which
  # conflicts with shipping a linkable static-library kit.
  #
  # The Apple spelling is written from the documented flag but has not been
  # exercised on macOS from this tree.
  if(APPLE)
    target_link_options(${target} PRIVATE -Wl,-dead_strip)
  elseif(UNIX)
    target_link_options(${target} PRIVATE -Wl,--gc-sections)
  endif()
endfunction()

add_hermes_tool(hermes-node hermes-node.cpp)
hermes_node_link_setup(hermes-node)

# A target that exists only to make CMake compute hermes-node's exact link
# closure and system-library list. The launcher intercepts the link and cuts
# the kit from it instead of producing a binary, so the kit can never disagree
# with what actually links.
#
# It is built with add_hermes_tool, not a bare add_executable, so that
# HERMES_EXTRA_LINKER_FLAGS and the Hermes C++ flags apply to it exactly as
# they do to hermes-node -- a probe that links differently would cut a kit
# that describes a link nobody performs.
#
# EXCLUDE_FROM_ALL because the merged archive is a full copy of every archive
# in the closure: 40 MB in a Release build, 755 MB in an ASAN one, which is
# too much to add to every incremental build of the primary development
# configuration. Anything that needs a kit builds this target explicitly.
#
# The kit's three real outputs (the merged archive, the copied-out NAPI
# archive, the manifest) are written by the launcher and are therefore
# invisible to the build system, which sees only the link's -o. So the -o is
# aimed INSIDE the kit directory, where the launcher creates it as an empty
# stamp: deleting the kit deletes the stamp with it, and the next build cuts
# the kit again. With the stamp anywhere else, `rm -rf kit` would leave a
# build that reports nothing to do and a kit that never comes back.
set(HERMES_NODE_KIT_DIR ${CMAKE_BINARY_DIR}/kit)
# The JS test suite passes this to lit as --param kit_dir, which is what turns
# the `linker-available` feature on; a variable set in this subdirectory is
# invisible to the top-level CMakeLists that builds the lit command line, so
# hand it up rather than spelling ${CMAKE_BINARY_DIR}/kit a second time there.
set(HERMES_NODE_KIT_DIR ${HERMES_NODE_KIT_DIR} PARENT_SCOPE)
add_hermes_tool(hermes-node-kit hermes-node.cpp)
hermes_node_link_setup(hermes-node-kit)
set_target_properties(hermes-node-kit PROPERTIES
  EXCLUDE_FROM_ALL TRUE
  RUNTIME_OUTPUT_DIRECTORY ${HERMES_NODE_KIT_DIR}
  OUTPUT_NAME kit.stamp
  RULE_LAUNCH_LINK
    "${Python_EXECUTABLE} ${PROJECT_SOURCE_DIR}/utils/make-kit.py \
     --kit-dir ${HERMES_NODE_KIT_DIR} \
     --version-header ${HERMES_NODE_VERSION_HEADER} --"
  # `ninja clean` knows about the stamp and nothing else, for the same reason
  # -- these three are written behind its back, so name them.
  ADDITIONAL_CLEAN_FILES
    "${HERMES_NODE_KIT_DIR}/libhermes-node-kit.a;\
${HERMES_NODE_KIT_DIR}/kit.manifest;\
${HERMES_NODE_KIT_DIR}/hermes-node-bundle-main.o;\
${HERMES_NODE_KIT_DIR}/$<TARGET_FILE_NAME:hermesNapi>"
)

# The app entry, compiled but never linked here: --build-exe links it
# against the kit together with the payload object it generates. An OBJECT
# library rather than a static one, because it must stay a loose .o -- see
# the entry-object constraint in the design.
add_library(hermesNodeBundleMain OBJECT bundle_main.cpp)
target_include_directories(hermesNodeBundleMain PRIVATE
  ${PROJECT_SOURCE_DIR}/include
  ${PROJECT_SOURCE_DIR}/hermes/include/hermes/napi)
# Not actually linked -- this is an OBJECT library whose members reach the
# final binary only through the kit archive -- but target_link_libraries is
# also how CMake propagates a dependency's usage requirements, and this file
# reads a container's VM options, which needs hermesNodeVmOptions's public
# include directories (hermes/Public/RuntimeConfig.h, reached transitively
# through hermesvm_a).
target_link_libraries(hermesNodeBundleMain PRIVATE hermesNodeVmOptions)
# Copied into the kit as a rule with a real OUTPUT rather than as a
# POST_BUILD step on hermes-node-kit. A POST_BUILD runs only when that
# target's link rule runs, and the kit's link is the stamp -- which does not
# depend on this object, and so stays up to date when bundle_main.cpp
# changes. The copy would then never run and the kit would keep serving a
# stale entry object. Naming the copy's output makes the build system track
# it: the object changing re-copies it, and deleting the kit directory
# (which deletes this file with the stamp) brings both back.
#
# The directory is created here rather than depended on: make-kit.py creates
# it too, with exist_ok, and neither rule can be ordered against the other
# without making hermes-node-kit depend on its own dependency.
add_custom_command(
  OUTPUT ${HERMES_NODE_KIT_DIR}/hermes-node-bundle-main.o
  COMMAND ${CMAKE_COMMAND} -E make_directory ${HERMES_NODE_KIT_DIR}
  COMMAND ${CMAKE_COMMAND} -E copy
          $<TARGET_OBJECTS:hermesNodeBundleMain>
          ${HERMES_NODE_KIT_DIR}/hermes-node-bundle-main.o
  DEPENDS hermesNodeBundleMain $<TARGET_OBJECTS:hermesNodeBundleMain>
  COMMENT "Copying the app entry object into the kit"
  COMMAND_EXPAND_LISTS
  VERBATIM)
add_custom_target(hermes-node-kit-entry
  DEPENDS ${HERMES_NODE_KIT_DIR}/hermes-node-bundle-main.o)
add_dependencies(hermes-node-kit hermes-node-kit-entry)
