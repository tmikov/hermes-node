# Copyright (c) Tzvetan Mikov.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# The GPU half of the compositor: per-backend sharing, completion and
# blitting, plus BOTH sokol_gfx instances. The top-level CMakeLists.txt only
# add_subdirectory()s this when HERMES_NODE_ENABLE_UI is on.

# v1 is GL on X11 (GLX, desktop GL 4.3 core) for Linux and any other
# non-Apple Unix, and Metal on macOS. A platform that is not Unix has no
# backend, and this whole directory is skipped there.
#
# A message(FATAL_ERROR) here would be the louder choice and is deliberately
# NOT what this does: the option is one a build configuration turns on for
# every platform it covers (CI's asan-debug-o1 does, in
# .github/workflows/ci.yml), and a platform without a backend should still
# build the rest. There the library is not built, hermes-node-ui keeps
# exactly the windowless behaviour Task 6 gave it, and the `ui-gpu` lit
# feature reports the window tests UNSUPPORTED.
if(NOT UNIX)
  message(STATUS
    "hermes-node UI: no GPU backend for this platform "
    "(v1 is Linux GL and macOS Metal). "
    "hermes-node-ui will build without window support.")
  return()
endif()

if(APPLE)
  # Metal (Task 16). The producing half (metal_backend.mm) and the
  # presenting half (metal_present.mm) are split for the reason the GL pair
  # below is; see metal_present.h. Nothing to look up: Cocoa, QuartzCore
  # and Metal are system frameworks, linked below.
  set(HNUI_GPU_BACKEND_SOURCES metal_backend.mm metal_present.mm)
else()
find_package(OpenGL REQUIRED)
find_package(X11 REQUIRED)

# sokol_app needs Xi (raw mouse input) and Xcursor (custom cursors) beyond
# plain X11, and it is compiled in tools/hermes-node-ui/ rather than here.
# Checked at configure time all the same: the alternative is an undefined
# reference from a 600k-line header at the very end of a long link, which
# names an internal Sokol symbol and not the missing development package.
foreach(_x11_component X11 Xi Xcursor)
  if(NOT TARGET X11::${_x11_component})
    message(FATAL_ERROR
      "hermes-node UI: X11::${_x11_component} not found. sokol_app needs "
      "X11, Xi and Xcursor; on Debian/Ubuntu these are libx11-dev, "
      "libxi-dev and libxcursor-dev. Configure without "
      "-DHERMES_NODE_ENABLE_UI=ON to build hermes-node alone.")
  endif()
endforeach()

# The presenting half, gl_present.cpp, is in a translation unit of its own
# because it must include sgp_rename.h before sokol_gfx.h's declarations
# and gl_backend.cpp must not -- see gl_present.h's class comment. It is
# also the one unit that compiles sokol_glue.h's implementation, which the
# design says is compiled "exactly once, in the presenting unit, or not at
# all".
set(HNUI_GPU_BACKEND_SOURCES gl_backend.cpp gl_present.cpp)
endif()

# sgp_rename.h: one `#define sg_x sgp_x` per public sokol_gfx function, so
# the presenting instance gets its own copy of the implementation. Generated
# rather than checked in, because it is derived entirely from the vendored
# sokol_gfx.h -- a checked-in copy would be a second thing to update on a
# Sokol bump, and the failure mode of forgetting is a symbol that silently
# reaches the other instance's state.
set(SGP_RENAME_H ${CMAKE_CURRENT_BINARY_DIR}/generated/sgp_rename.h)
set(SOKOL_GFX_H ${PROJECT_SOURCE_DIR}/external/sokol/sokol/sokol_gfx.h)

add_custom_command(
  OUTPUT ${SGP_RENAME_H}
  COMMAND ${Python_EXECUTABLE}
          ${PROJECT_SOURCE_DIR}/utils/gen-sokol-rename.py
          --input ${SOKOL_GFX_H}
          --output ${SGP_RENAME_H}
  DEPENDS ${PROJECT_SOURCE_DIR}/utils/gen-sokol-rename.py ${SOKOL_GFX_H}
  COMMENT "Generating sgp_rename.h from sokol_gfx.h"
  VERBATIM
)

# Listed among the sources below as well; this target exists so that
# something can DEPEND on the header alone -- the lit test that reads it
# does, and so does check-sgp-isolation.
add_custom_target(hermes-node-sgp-rename DEPENDS ${SGP_RENAME_H})

add_hermes_library(hermesNodeCompositorGpu STATIC
  # The backend's two halves, chosen above.
  ${HNUI_GPU_BACKEND_SOURCES}
  ui_trace.cpp
  # The one body of all three Sokol loggers, sokol_app's included.
  sokol_log.cpp
  test_hooks.cpp
  sokol_gfx_impl.cpp
  sokol_gfx_present_impl.cpp
  # A generated header listed as a source, which is how CMake learns that
  # these objects cannot be compiled before it exists.
  ${SGP_RENAME_H}
)

target_include_directories(hermesNodeCompositorGpu PUBLIC
  ${PROJECT_SOURCE_DIR}/include
  # backend.h, ui_trace.h and sokol_log.h, for tools/hermes-node-ui.
  ${CMAKE_CURRENT_SOURCE_DIR}
)
target_include_directories(hermesNodeCompositorGpu PRIVATE
  ${CMAKE_CURRENT_BINARY_DIR}/generated
)

# PUBLIC, so tools/hermes-node-ui inherits the backend define and the
# windowing libraries without restating them. sokol_app.h is compiled there
# and needs the identical SOKOL_GLCORE: two translation units of the same
# Sokol build configured differently would disagree about struct layouts
# while linking perfectly (see sokol_config.h).
if(APPLE)
  target_compile_definitions(hermesNodeCompositorGpu PUBLIC SOKOL_METAL)

  target_link_libraries(hermesNodeCompositorGpu PUBLIC
    hermesNodeCompositor
    sokol
    "-framework Cocoa"
    "-framework QuartzCore"
    "-framework Metal"
  )

  # Sokol's Metal implementation is Objective-C, so the two sokol_gfx units
  # are compiled as Objective-C++ -- under their .cpp names, so that their
  # object names, and the lit params and isolation check that name those
  # objects, are the same on both platforms. A per-source option rather
  # than LANGUAGE OBJCXX keeps the language CXX, so Hermes's CMAKE_CXX_FLAGS
  # still apply. ARC on every Objective-C(++) unit: sokol_config.h refuses
  # to compile a Metal Sokol unit without it, because Sokol's texture
  # injection releases an injected texture under manual reference counting.
  set_source_files_properties(sokol_gfx_impl.cpp sokol_gfx_present_impl.cpp
    PROPERTIES COMPILE_OPTIONS "-xobjective-c++;-fobjc-arc")
  set_source_files_properties(metal_backend.mm metal_present.mm
    PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
else()
target_compile_definitions(hermesNodeCompositorGpu PUBLIC SOKOL_GLCORE)

target_link_libraries(hermesNodeCompositorGpu PUBLIC
  hermesNodeCompositor
  sokol
  OpenGL::GL
  X11::X11
  X11::Xi
  X11::Xcursor
)
endif()

# check-sgp-isolation: prove the two sokol_gfx instances do not reach each
# other's state, by reading the objects' symbol tables rather than by
# trusting that the rename header was included in the right place.
#
# **This is also what catches a rename list that is merely incomplete.** The
# generator cross-checks sokol_gfx.h's declarations against its definitions,
# but both halves of that check are regexes over the same file and would
# miss the same unusual spelling; the presenting object exporting zero
# `sg_*` symbols is an independent measurement of the same property, taken
# from the compiler's output instead.
#
# Wired into the lit suite (test/sokol-isolation.js) rather than left as a
# target somebody remembers to run -- this plan has already been bitten once
# by tests that existed and never ran anywhere.
#
# The two USER objects are the backend's own code that calls each instance.
# One that references the other instance's family -- the presenter reading
# the producer's command-buffer accessor, say -- links cleanly and reads the
# wrong `_sg`, so the checker refuses it. Promoted to the parent scope
# because the top-level CMakeLists.txt passes them to lit
# (HERMES_NODE_LIT_PARAMS, for test/sokol-isolation.js); the Metal backend
# names its own objects here.
set(SGP_ISOLATION_OBJ_DIR
  ${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/hermesNodeCompositorGpu.dir)
set(HNUI_SGP_PRODUCING_USER_OBJ ${SGP_ISOLATION_OBJ_DIR}/gl_backend.cpp.o)
set(HNUI_SGP_PRESENTING_USER_OBJ ${SGP_ISOLATION_OBJ_DIR}/gl_present.cpp.o)
if(APPLE)
  set(HNUI_SGP_PRODUCING_USER_OBJ ${SGP_ISOLATION_OBJ_DIR}/metal_backend.mm.o)
  set(HNUI_SGP_PRESENTING_USER_OBJ ${SGP_ISOLATION_OBJ_DIR}/metal_present.mm.o)
endif()
set(HNUI_SGP_PRODUCING_USER_OBJ ${HNUI_SGP_PRODUCING_USER_OBJ} PARENT_SCOPE)
set(HNUI_SGP_PRESENTING_USER_OBJ ${HNUI_SGP_PRESENTING_USER_OBJ} PARENT_SCOPE)
add_custom_target(check-sgp-isolation
  COMMAND ${Python_EXECUTABLE}
          ${PROJECT_SOURCE_DIR}/utils/check-sgp-isolation.py
          --producing ${SGP_ISOLATION_OBJ_DIR}/sokol_gfx_impl.cpp.o
          --presenting ${SGP_ISOLATION_OBJ_DIR}/sokol_gfx_present_impl.cpp.o
          --producing-user ${HNUI_SGP_PRODUCING_USER_OBJ}
          --presenting-user ${HNUI_SGP_PRESENTING_USER_OBJ}
  DEPENDS hermesNodeCompositorGpu
          ${PROJECT_SOURCE_DIR}/utils/check-sgp-isolation.py
  COMMENT "Checking sokol_gfx symbol isolation"
  VERBATIM
)
