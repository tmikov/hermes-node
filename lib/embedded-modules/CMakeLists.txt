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

# Generate the registry C++ file from all .inc files.
set(REGISTRY_CPP "${CMAKE_CURRENT_BINARY_DIR}/embedded_modules_registry.cpp")

add_custom_command(
  OUTPUT "${REGISTRY_CPP}"
  COMMAND ${Python_EXECUTABLE} ${GEN_REGISTRY_SCRIPT}
          "${MANIFEST_FILE}" "${INC_DIR}" "${REGISTRY_CPP}"
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
