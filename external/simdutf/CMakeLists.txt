# Copyright (c) Tzvetan Mikov.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# Wrapper CMakeLists.txt for vendored simdutf.
# SIMD-accelerated Unicode transcoding and base64.

add_library(simdutf STATIC simdutf/simdutf.cpp)
target_include_directories(simdutf PUBLIC simdutf/)
