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

set(WRAPPED_DIR ${CMAKE_CURRENT_BINARY_DIR}/wrapped)
set(HBC_DIR ${CMAKE_CURRENT_BINARY_DIR}/hbc)
set(INC_DIR ${CMAKE_CURRENT_BINARY_DIR}/inc)

file(MAKE_DIRECTORY ${WRAPPED_DIR} ${HBC_DIR} ${INC_DIR})

set(JS_COMPILER_FLAGS
  -Xes6-block-scoping
  -Xasync-generators
  -Wno-undefined-variable
  -emit-binary
  -g
)

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
  DEPENDS ${GEN_REGISTRY_SCRIPT} ${MANIFEST_FILE} ${ALL_INC_FILES}
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
