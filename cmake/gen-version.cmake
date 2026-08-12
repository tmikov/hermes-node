# Copyright (c) Tzvetan Mikov.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
#
# Derives the hermes-node version and writes version.h.
#
# Written as a script (cmake -P) rather than inline in version.cmake so it can
# run at build time as well as at configure time. `git describe` changes its
# answer without any CMake input changing, so a configure-time-only derivation
# reports whatever it said the last time CMake happened to configure -- a plain
# rebuild after a commit keeps the old hash, and cannot be made to notice.
#
# Inputs (pass with -D):
#   SRC_DIR         repository root, for git
#   IN_FILE         version.h.in template
#   OUT_FILE        version.h to write
#   PINNED_VERSION  explicit version; git is not consulted when non-empty
#   VERBOSE         print the resolved version as a STATUS message

if(PINNED_VERSION)
  set(HERMES_NODE_VERSION "${PINNED_VERSION}")
else()
  execute_process(
    COMMAND git describe --tags --always --dirty --match "v*"
    WORKING_DIRECTORY ${SRC_DIR}
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

if(VERBOSE)
  message(STATUS "hermes-node version: ${HERMES_NODE_VERSION}")
endif()

# configure_file leaves the output alone when the content would be identical,
# so running this on every build only perturbs the build graph when the version
# has actually moved. Nothing but hermes-node.cpp includes the header.
configure_file(${IN_FILE} ${OUT_FILE} @ONLY)
