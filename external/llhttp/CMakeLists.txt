# Copyright (c) Tzvetan Mikov.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# Wrapper CMakeLists.txt for vendored llhttp.
# Builds a static library only; no shared lib, tools, or install.

add_library(llhttp_a STATIC
  llhttp/src/llhttp.c
  llhttp/src/http.c
  llhttp/src/api.c
)

target_include_directories(llhttp_a PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/llhttp/include
)

set_target_properties(llhttp_a PROPERTIES
  C_STANDARD 99
  POSITION_INDEPENDENT_CODE ON
)
