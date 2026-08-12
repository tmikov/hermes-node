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
#
# The derivation itself lives in gen-version.cmake, which runs both here and on
# every build; see the comment there for why once at configure time is not
# enough.

set(HERMES_NODE_VERSION_SCRIPT ${CMAKE_SOURCE_DIR}/cmake/gen-version.cmake)
set(HERMES_NODE_VERSION_HEADER
  ${CMAKE_BINARY_DIR}/generated/hermes/node-compat/version.h)

# An explicit -DHERMES_NODE_VERSION pins the value; otherwise the script asks
# git each time.
if(DEFINED HERMES_NODE_VERSION)
  set(_pinned_version "${HERMES_NODE_VERSION}")
else()
  set(_pinned_version "")
endif()

set(HERMES_NODE_VERSION_ARGS
  -DSRC_DIR=${CMAKE_SOURCE_DIR}
  -DIN_FILE=${CMAKE_SOURCE_DIR}/include/hermes/node-compat/version.h.in
  -DOUT_FILE=${HERMES_NODE_VERSION_HEADER}
  -DPINNED_VERSION=${_pinned_version}
)

# Generate now, so the header exists before anything tries to compile against
# it, and so configure still reports the version it resolved.
execute_process(
  COMMAND ${CMAKE_COMMAND} ${HERMES_NODE_VERSION_ARGS} -DVERBOSE=1
          -P ${HERMES_NODE_VERSION_SCRIPT}
)

# Re-derive on every build. tools/hermes-node makes hermes-node depend on this,
# which orders it ahead of the compile that consumes the header.
add_custom_target(hermes-node-version
  COMMAND ${CMAKE_COMMAND} ${HERMES_NODE_VERSION_ARGS}
          -P ${HERMES_NODE_VERSION_SCRIPT}
  BYPRODUCTS ${HERMES_NODE_VERSION_HEADER}
  COMMENT "Deriving hermes-node version"
  VERBATIM
)
