# Copyright (c) Tzvetan Mikov.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# Build pipeline for embedded JS modules:
#   .js -> wrap-cjs.py -> hermesc -emit-binary -> xxd.py -> .inc
#   All .inc files -> gen-embedded-registry.py -> registry .cpp
#   embedded_modules.cpp + registry .cpp -> hermesNodeEmbeddedModules library

set(MANIFEST_FILE ${CMAKE_CURRENT_SOURCE_DIR}/embedded-modules.txt)
# Re-run configure when the manifest changes (adds/removes modules).
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${MANIFEST_FILE})
set(WRAP_CJS_SCRIPT ${PROJECT_SOURCE_DIR}/tools/wrap-cjs.py)
set(GEN_REGISTRY_SCRIPT ${PROJECT_SOURCE_DIR}/tools/gen-embedded-registry.py)
set(XXD_SCRIPT ${PROJECT_SOURCE_DIR}/hermes/lib/InternalJavaScript/xxd.py)

set(LIBJS_DIR ${PROJECT_SOURCE_DIR}/libjs)
set(LIBJS_SHIMS_DIR ${PROJECT_SOURCE_DIR}/libjs/shims)
set(LIBJS_NODE_DIR ${PROJECT_SOURCE_DIR}/libjs-node)

set(hermesc_EXE ${HERMES_TOOLS_OUTPUT_DIR}/hermesc${CMAKE_EXECUTABLE_SUFFIX})
set(shermes_EXE ${HERMES_TOOLS_OUTPUT_DIR}/shermes${CMAKE_EXECUTABLE_SUFFIX})
set(NATIVE_C_DIR ${CMAKE_CURRENT_BINARY_DIR}/native-c)
file(MAKE_DIRECTORY ${NATIVE_C_DIR})
set(ALL_NATIVE_C_FILES "")

# Every <module id> <unit name> pair CMake derived, cross-checked by
# gen-native-registry.py against its own derivation. Truncated here, before
# either loop appends to it, so a reconfigure does not accumulate duplicates.
set(NATIVE_UNIT_MAP ${CMAKE_CURRENT_BINARY_DIR}/native-unit-names.txt)
file(WRITE ${NATIVE_UNIT_MAP} "")

set(WRAPPED_DIR ${CMAKE_CURRENT_BINARY_DIR}/wrapped)
set(HBC_DIR ${CMAKE_CURRENT_BINARY_DIR}/hbc)
set(INC_DIR ${CMAKE_CURRENT_BINARY_DIR}/inc)

file(MAKE_DIRECTORY ${WRAPPED_DIR} ${HBC_DIR} ${INC_DIR})

# The language features every built-in module is compiled with, and the ONLY
# flags the bytecode and native pipelines share. Everything else below --
# output format, debug level, warning control -- differs per tool, and sharing
# the whole list would hand shermes the bytecode-only -emit-binary.
#
# A third copy of these values lives in C++, as kJSLanguageFlags
# (include/hermes/node-compat/bundle/cjs_wrapper.h), which drives the require()
# scanner and the shermes argv for a user's own modules. Nothing mechanically
# forces that copy and this one to agree. Changing one means changing the
# other: block scoping off makes every let-in-loop closure capture the wrong
# binding, with no diagnostic at build time or run time.
set(HERMES_NODE_JS_LANGUAGE_FLAGS
  -Xes6-block-scoping
  -Xasync-generators
)

set(JS_COMPILER_FLAGS
  ${HERMES_NODE_JS_LANGUAGE_FLAGS}
  -Wno-undefined-variable
  -emit-binary
  -g
)

# Emits the rule that turns one built-in module's compiler input into
# generated C, records the unit name it used, and appends the C file to
# ALL_NATIVE_C_FILES.
#
# The input is the SAME file the bytecode pipeline compiles -- already
# CJS-wrapped for an ordinary module, raw for a bootstrap one -- which is what
# makes line and column parity with the bytecode build structural rather than
# something a test has to assert. wrap-cjs.py puts the wrapper header on the
# same line as source line 1 for exactly that reason.
#
# A macro rather than a function: list(APPEND) has to land in the
# directory-level ALL_NATIVE_C_FILES the add_library at the end of this file
# reads, and a macro runs in the caller's scope where a function would not.
macro(add_native_builtin MODULE_ID SAFE_ID INPUT_FILE)
  set(_unit_name "hn_b_${SAFE_ID}")
  file(APPEND ${NATIVE_UNIT_MAP} "${MODULE_ID} ${_unit_name}\n")
  # Flat: SAFE_ID has every '/' replaced with '__', so this is always
  # directly in NATIVE_C_DIR, created once at the top of this file. No
  # per-module subdirectory to make.
  set(_native_c "${NATIVE_C_DIR}/${SAFE_ID}.c")
  add_custom_command(
    OUTPUT "${_native_c}"
    COMMAND ${shermes_EXE}
            -emit-c
            # shermes defaults to -g0, which emits no source locations at all.
            # Without this every built-in frame in a stack trace prints
            # "(native)" with no file or line. Matches what build-native
            # passes for a user's modules (native_compile.cpp).
            -g2
            -O
            # A built-in module reads primordials, internalBinding and process
            # as undeclared globals. The exact flag the bytecode pipeline uses
            # above, and not -w: blanket suppression would make "187 modules,
            # zero warnings" unobservable rather than true, since a real
            # diagnostic from a future module would be silenced alongside
            # these. Measured: the narrow flag leaves all 187 clean, and the
            # archive is byte-for-byte the size -w produced.
            -Wno-undefined-variable
            # The bytecode pipeline ignores a //# sourceMappingURL comment;
            # shermes would honour one and rewrite the location table.
            -sm-comment=off
            ${HERMES_NODE_JS_LANGUAGE_FLAGS}
            -source-name=${MODULE_ID}
            -exported-unit=${_unit_name}
            -o ${_native_c}
            "${INPUT_FILE}"
    DEPENDS shermes "${INPUT_FILE}"
    COMMENT "Compiling ${MODULE_ID} to native C"
    VERBATIM
  )
  list(APPEND ALL_NATIVE_C_FILES "${_native_c}")
endmacro()

# Parse the manifest to get module entries.
file(STRINGS ${MANIFEST_FILE} MANIFEST_LINES)

set(ALL_INC_FILES "")
set(ALL_JS_SOURCES "")

foreach(LINE IN LISTS MANIFEST_LINES)
  # Skip comments and empty lines.
  string(STRIP "${LINE}" LINE)
  if("${LINE}" STREQUAL "" OR "${LINE}" MATCHES "^#")
    continue()
  endif()

  # Parse @bootstrap prefix.
  set(IS_BOOTSTRAP FALSE)
  if("${LINE}" MATCHES "^@bootstrap ")
    set(IS_BOOTSTRAP TRUE)
    string(REGEX REPLACE "^@bootstrap " "" MODULE_ID "${LINE}")
  else()
    set(MODULE_ID "${LINE}")
  endif()

  # Generate safe identifier (replace / with __ and - with _).
  string(REPLACE "/" "__" SAFE_ID "${MODULE_ID}")
  string(REPLACE "-" "_" SAFE_ID "${SAFE_ID}")

  # Resolve the source file path.
  # For bootstrap: libjs/<id>.js
  # For CJS: check libjs/shims/<id>.js first, then libjs-node/<id>.js
  if(IS_BOOTSTRAP)
    set(SOURCE_FILE "${LIBJS_DIR}/${MODULE_ID}.js")
  else()
    set(SHIM_FILE "${LIBJS_SHIMS_DIR}/${MODULE_ID}.js")
    if(EXISTS "${SHIM_FILE}")
      set(SOURCE_FILE "${SHIM_FILE}")
    else()
      set(SOURCE_FILE "${LIBJS_NODE_DIR}/${MODULE_ID}.js")
    endif()
  endif()

  list(APPEND ALL_JS_SOURCES "${SOURCE_FILE}")

  if(IS_BOOTSTRAP)
    # Bootstrap modules: compile directly (no CJS wrapper).
    set(INPUT_FOR_HERMESC "${SOURCE_FILE}")
  else()
    # CJS modules: wrap first.
    # Ensure subdirectory exists for the wrapped file.
    get_filename_component(WRAPPED_SUBDIR "${WRAPPED_DIR}/${SAFE_ID}" DIRECTORY)
    file(MAKE_DIRECTORY "${WRAPPED_SUBDIR}")

    set(WRAPPED_FILE "${WRAPPED_DIR}/${SAFE_ID}.js")
    add_custom_command(
      OUTPUT "${WRAPPED_FILE}"
      COMMAND ${Python_EXECUTABLE} ${WRAP_CJS_SCRIPT}
              "${SOURCE_FILE}" "${MODULE_ID}" "${WRAPPED_FILE}"
      DEPENDS ${WRAP_CJS_SCRIPT} "${SOURCE_FILE}"
      COMMENT "Wrapping ${MODULE_ID}"
      VERBATIM
    )
    set(INPUT_FOR_HERMESC "${WRAPPED_FILE}")
  endif()

  add_native_builtin("${MODULE_ID}" "${SAFE_ID}" "${INPUT_FOR_HERMESC}")

  # Compile to bytecode.
  set(HBC_FILE "${HBC_DIR}/${SAFE_ID}.hbc")
  get_filename_component(HBC_SUBDIR "${HBC_FILE}" DIRECTORY)
  file(MAKE_DIRECTORY "${HBC_SUBDIR}")

  add_custom_command(
    OUTPUT "${HBC_FILE}"
    COMMAND ${hermesc_EXE} ${JS_COMPILER_FLAGS}
            -out=${HBC_FILE} "${INPUT_FOR_HERMESC}"
    DEPENDS hermesc "${INPUT_FOR_HERMESC}"
    COMMENT "Compiling ${MODULE_ID} to bytecode"
    VERBATIM
  )

  # Convert to hex include.
  set(INC_FILE "${INC_DIR}/${SAFE_ID}.hbc.inc")
  get_filename_component(INC_SUBDIR "${INC_FILE}" DIRECTORY)
  file(MAKE_DIRECTORY "${INC_SUBDIR}")

  add_custom_command(
    OUTPUT "${INC_FILE}"
    COMMAND ${Python_EXECUTABLE} ${XXD_SCRIPT} "${HBC_FILE}" > "${INC_FILE}"
    DEPENDS ${XXD_SCRIPT} "${HBC_FILE}"
    COMMENT "Generating ${SAFE_ID}.hbc.inc"
    VERBATIM
  )

  list(APPEND ALL_INC_FILES "${INC_FILE}")
endforeach()

# ============================================================
# Vendored NPM packages
# ============================================================
set(VENDORED_DIR ${PROJECT_SOURCE_DIR}/vendored)

# Collect all vendored package directories.
file(GLOB VENDORED_PKG_DIRS CONFIGURE_DEPENDS "${VENDORED_DIR}/*/")

set(VENDORED_PACKAGES_ENTRIES "")
set(VENDORED_MODULE_IDS_ENTRIES "")
set(VENDORED_MANIFEST_LINES "")

foreach(PKG_DIR IN LISTS VENDORED_PKG_DIRS)
  get_filename_component(PKG_NAME "${PKG_DIR}" NAME)

  # Read "main" field from package.json (default: index.js).
  set(PKG_MAIN "index.js")
  set(PKG_JSON_PATH "${PKG_DIR}/package.json")
  if(EXISTS "${PKG_JSON_PATH}")
    file(READ "${PKG_JSON_PATH}" PKG_JSON)
    string(REGEX MATCH "\"main\"[[:space:]]*:[[:space:]]*\"([^\"]+)\"" _match "${PKG_JSON}")
    if(CMAKE_MATCH_1)
      set(PKG_MAIN "${CMAKE_MATCH_1}")
    endif()
  endif()
  # Normalize and strip .js extension from main to get the entry module ID.
  # Keep this simple and path-like because module IDs are slash-separated.
  set(PKG_MAIN_STEM "${PKG_MAIN}")
  string(REGEX REPLACE "^\\./" "" PKG_MAIN_STEM "${PKG_MAIN_STEM}")
  string(REGEX REPLACE "\\.js$" "" PKG_MAIN_STEM "${PKG_MAIN_STEM}")
  set(PKG_ENTRY_ID "vendored/${PKG_NAME}/${PKG_MAIN_STEM}")

  # Build vendored-packages.js template entries.
  if(VENDORED_PACKAGES_ENTRIES)
    string(APPEND VENDORED_PACKAGES_ENTRIES ", ")
  endif()
  string(APPEND VENDORED_PACKAGES_ENTRIES "'${PKG_NAME}': '${PKG_ENTRY_ID}'")

  # Glob all .js files in this package.
  file(GLOB_RECURSE PKG_JS_FILES CONFIGURE_DEPENDS "${PKG_DIR}/*.js")

  foreach(JS_FILE IN LISTS PKG_JS_FILES)
    # Module ID = path relative to project root, minus .js extension.
    file(RELATIVE_PATH REL_PATH "${PROJECT_SOURCE_DIR}" "${JS_FILE}")
    string(REGEX REPLACE "\\.js$" "" MODULE_ID "${REL_PATH}")

    # Add to module IDs list for vendored-packages.js.
    if(VENDORED_MODULE_IDS_ENTRIES)
      string(APPEND VENDORED_MODULE_IDS_ENTRIES ", ")
    endif()
    string(APPEND VENDORED_MODULE_IDS_ENTRIES "'${MODULE_ID}'")

    # Add to manifest for gen-embedded-registry.py.
    list(APPEND VENDORED_MANIFEST_LINES "${MODULE_ID}")

    # Safe ID for file names.
    string(REPLACE "/" "__" SAFE_ID "${MODULE_ID}")
    string(REPLACE "-" "_" SAFE_ID "${SAFE_ID}")

    list(APPEND ALL_JS_SOURCES "${JS_FILE}")

    # CJS wrap.
    get_filename_component(WRAPPED_SUBDIR "${WRAPPED_DIR}/${SAFE_ID}" DIRECTORY)
    file(MAKE_DIRECTORY "${WRAPPED_SUBDIR}")

    set(WRAPPED_FILE "${WRAPPED_DIR}/${SAFE_ID}.js")
    add_custom_command(
      OUTPUT "${WRAPPED_FILE}"
      COMMAND ${Python_EXECUTABLE} ${WRAP_CJS_SCRIPT}
              "${JS_FILE}" "${MODULE_ID}" "${WRAPPED_FILE}"
      DEPENDS ${WRAP_CJS_SCRIPT} "${JS_FILE}"
      COMMENT "Wrapping vendored ${MODULE_ID}"
      VERBATIM
    )

    add_native_builtin("${MODULE_ID}" "${SAFE_ID}" "${WRAPPED_FILE}")

    # Compile to bytecode.
    set(HBC_FILE "${HBC_DIR}/${SAFE_ID}.hbc")
    get_filename_component(HBC_SUBDIR "${HBC_FILE}" DIRECTORY)
    file(MAKE_DIRECTORY "${HBC_SUBDIR}")

    add_custom_command(
      OUTPUT "${HBC_FILE}"
      COMMAND ${hermesc_EXE} ${JS_COMPILER_FLAGS}
              -out=${HBC_FILE} "${WRAPPED_FILE}"
      DEPENDS hermesc "${WRAPPED_FILE}"
      COMMENT "Compiling vendored ${MODULE_ID} to bytecode"
      VERBATIM
    )

    # Convert to hex include.
    set(INC_FILE "${INC_DIR}/${SAFE_ID}.hbc.inc")
    get_filename_component(INC_SUBDIR "${INC_FILE}" DIRECTORY)
    file(MAKE_DIRECTORY "${INC_SUBDIR}")

    add_custom_command(
      OUTPUT "${INC_FILE}"
      COMMAND ${Python_EXECUTABLE} ${XXD_SCRIPT} "${HBC_FILE}" > "${INC_FILE}"
      DEPENDS ${XXD_SCRIPT} "${HBC_FILE}"
      COMMENT "Generating vendored ${SAFE_ID}.hbc.inc"
      VERBATIM
    )

    list(APPEND ALL_INC_FILES "${INC_FILE}")
  endforeach()
endforeach()

# Generate vendored-packages.js from template.
set(VENDORED_PACKAGES_JS_IN "${CMAKE_CURRENT_SOURCE_DIR}/vendored-packages.js.in")
set(VENDORED_PACKAGES_JS "${CMAKE_CURRENT_BINARY_DIR}/vendored-packages.js")
configure_file("${VENDORED_PACKAGES_JS_IN}" "${VENDORED_PACKAGES_JS}" @ONLY)

# Process vendored-packages.js as a bootstrap module (no CJS wrap).
set(VP_SAFE_ID "vendored_packages")

# VP_SAFE_ID is already safe_id("vendored-packages").
add_native_builtin("vendored-packages" "${VP_SAFE_ID}" "${VENDORED_PACKAGES_JS}")

set(VP_HBC_FILE "${HBC_DIR}/${VP_SAFE_ID}.hbc")
add_custom_command(
  OUTPUT "${VP_HBC_FILE}"
  COMMAND ${hermesc_EXE} ${JS_COMPILER_FLAGS}
          -out=${VP_HBC_FILE} "${VENDORED_PACKAGES_JS}"
  DEPENDS hermesc "${VENDORED_PACKAGES_JS}"
  COMMENT "Compiling vendored-packages to bytecode"
  VERBATIM
)

set(VP_INC_FILE "${INC_DIR}/${VP_SAFE_ID}.hbc.inc")
add_custom_command(
  OUTPUT "${VP_INC_FILE}"
  COMMAND ${Python_EXECUTABLE} ${XXD_SCRIPT} "${VP_HBC_FILE}" > "${VP_INC_FILE}"
  DEPENDS ${XXD_SCRIPT} "${VP_HBC_FILE}"
  COMMENT "Generating vendored_packages.hbc.inc"
  VERBATIM
)

list(APPEND ALL_INC_FILES "${VP_INC_FILE}")

# Write combined manifest (original + vendored-packages bootstrap + vendored modules).
set(COMBINED_MANIFEST "${CMAKE_CURRENT_BINARY_DIR}/combined-manifest.txt")
file(READ "${MANIFEST_FILE}" _manifest_content)
set(_combined "${_manifest_content}\n# Vendored packages bootstrap\n@bootstrap vendored-packages\n\n# Vendored modules\n")
foreach(_vline IN LISTS VENDORED_MANIFEST_LINES)
  string(APPEND _combined "${_vline}\n")
endforeach()
file(WRITE "${COMBINED_MANIFEST}" "${_combined}")

# Generate the registry C++ file from all .inc files.
set(REGISTRY_CPP "${CMAKE_CURRENT_BINARY_DIR}/embedded_modules_registry.cpp")

add_custom_command(
  OUTPUT "${REGISTRY_CPP}"
  COMMAND ${Python_EXECUTABLE} ${GEN_REGISTRY_SCRIPT}
          "${COMBINED_MANIFEST}" "${INC_DIR}" "${REGISTRY_CPP}"
  DEPENDS ${GEN_REGISTRY_SCRIPT}
          ${PROJECT_SOURCE_DIR}/tools/embedded_module_ids.py
          ${MANIFEST_FILE} ${ALL_INC_FILES}
  COMMENT "Generating embedded modules registry"
  VERBATIM
)

# Build the library.
add_library(hermesNodeEmbeddedModules STATIC
  embedded_modules.cpp
  "${REGISTRY_CPP}"
)

target_include_directories(hermesNodeEmbeddedModules
  PUBLIC
    ${PROJECT_SOURCE_DIR}/include
    # Hermes public NAPI headers (js_native_api.h, node_api_types.h, etc.).
    ${PROJECT_SOURCE_DIR}/hermes/include/hermes/napi
    # embedded_modules.h includes napi/hermes_napi.h, which lives here. It
    # arrived transitively from the hermesNapi link below; naming it makes a
    # requirement of the public header a declaration rather than a
    # coincidence.
    ${PROJECT_SOURCE_DIR}/hermes/API
  PRIVATE
    ${INC_DIR}
)

# The registry .cpp #includes .inc files from INC_DIR.
set_source_files_properties("${REGISTRY_CPP}" PROPERTIES
  INCLUDE_DIRECTORIES "${INC_DIR}"
)

target_link_libraries(hermesNodeEmbeddedModules
  PUBLIC hermesNapi
)

# ============================================================
# Native built-ins (EXCLUDE_FROM_ALL; cut into the kit)
# ============================================================
# The same 187 modules, compiled to Static Hermes units instead of bytecode,
# in an archive of their own. A build-native link names this archive ahead of
# the merged kit archive, so the linker resolves findEmbeddedModule() here and
# never pulls the bytecode member -- which is what keeps 2.1 MB of built-in
# bytecode out of a native artifact rather than merely unused inside it.
#
# EXCLUDE_FROM_ALL because a plain hermes-node build has no use for it and
# should not pay: measured 8.2 s wall / 47.6 s CPU at -j16 on a 16-core macOS
# arm64, dominated by cc. (The design doc's 5.4 s was a projection.)
# hermes-node-kit depends on it, and check-hermes-node-js already depends on
# the kit.
set(NATIVE_REGISTRY_CPP
    "${CMAKE_CURRENT_BINARY_DIR}/embedded_modules_registry_native.cpp")

# COMBINED_MANIFEST, not MANIFEST_FILE: the combined one also carries the
# vendored modules, which come from a CONFIGURE_DEPENDS glob, so adding or
# removing a vendored package must rebuild this table.
add_custom_command(
  OUTPUT "${NATIVE_REGISTRY_CPP}"
  COMMAND ${Python_EXECUTABLE}
          ${PROJECT_SOURCE_DIR}/tools/gen-native-registry.py
          "${COMBINED_MANIFEST}" "${NATIVE_UNIT_MAP}" "${NATIVE_REGISTRY_CPP}"
  DEPENDS ${PROJECT_SOURCE_DIR}/tools/gen-native-registry.py
          ${PROJECT_SOURCE_DIR}/tools/embedded_module_ids.py
          "${COMBINED_MANIFEST}"
          "${NATIVE_UNIT_MAP}"
  COMMENT "Generating native embedded modules registry"
  VERBATIM
)

add_library(hermesNodeBuiltinsNative STATIC EXCLUDE_FROM_ALL
  "${NATIVE_REGISTRY_CPP}"
  ${ALL_NATIVE_C_FILES}
)

target_include_directories(hermesNodeBuiltinsNative PRIVATE
  ${PROJECT_SOURCE_DIR}/include
  ${PROJECT_SOURCE_DIR}/hermes/include/hermes/napi
  # REQUIRED, and the one easy to miss: embedded_modules.h includes
  # napi/hermes_napi.h, which lives under hermes/API. Every other includer
  # gets this transitively from hermesNapi; this target links nothing, so it
  # has to name it. lib/event-loop/CMakeLists.txt does the same.
  ${PROJECT_SOURCE_DIR}/hermes/API
  # What the generated C includes: the SH headers and the generated
  # libhermesvm-config.h -- the same two directories make-kit.py stages into
  # the kit as include/ (--sh-include), so an in-tree compile and a kit
  # compile see the same headers.
  ${PROJECT_SOURCE_DIR}/hermes/include
  ${CMAKE_BINARY_DIR}/hermes/lib/config
)

# Applied to the C sources only; the registry is C++ and needs none of it.
#
# -fno-strict-aliasing and -fno-strict-overflow are REQUIRED, not tuning:
# Static Hermes's generated C reads and writes C++ objects through mirroring C
# structs, so unrelated types alias by construction. Hermes appends both to
# CMAKE_C_FLAGS inside its own subdirectory scope only
# (hermes/CMakeLists.txt), which a top-level target like this one does not
# inherit -- so they have to be named here.
#
# -std=gnu11 as a compile option rather than the C_STANDARD/C_EXTENSIONS
# target properties, which do not exist as SOURCE properties and would have
# silently done nothing. gnu rather than c11 because static_h.h uses a
# zero-length array. This is the same flag native_compile.cpp passes for the
# same generated C.
#
# NDEBUG is deliberately NOT set here. The generated C calls _SH_MODEL(),
# whose symbol is suffixed _dbg or _rel depending on whether NDEBUG was
# defined, and hermesvm_a -- which this links against -- got its answer from
# CMake's own per-config flags. Being a target in the same build is what makes
# the two agree by construction; utils/make-kit.py has to transport that
# decision by hand for the kit's own compiles, and got it wrong once.
set_source_files_properties(${ALL_NATIVE_C_FILES} PROPERTIES
  COMPILE_OPTIONS
    "-std=gnu11;-fno-strict-aliasing;-fno-strict-overflow;-w"
)
