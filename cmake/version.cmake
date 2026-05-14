# Copyright (c) Tzvetan Mikov.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
#
# hermes-node version derivation.
#
# Priority:
#   1. HERMES_NODE_VERSION passed via -D (release workflow path).
#   2. `git describe --tags --always --dirty --match v*` for local builds.
#   3. Fallback "0.0.0-dev" for tarballs without git metadata.

if(NOT DEFINED HERMES_NODE_VERSION)
  execute_process(
    COMMAND git describe --tags --always --dirty --match "v*"
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE _git_desc
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE _git_rc
  )
  if(_git_rc EQUAL 0 AND _git_desc)
    string(REGEX REPLACE "^v" "" HERMES_NODE_VERSION "${_git_desc}")
  else()
    set(HERMES_NODE_VERSION "0.0.0-dev")
  endif()
endif()

message(STATUS "hermes-node version: ${HERMES_NODE_VERSION}")

configure_file(
  ${CMAKE_SOURCE_DIR}/include/hermes/node-compat/version.h.in
  ${CMAKE_BINARY_DIR}/generated/hermes/node-compat/version.h
  @ONLY
)
