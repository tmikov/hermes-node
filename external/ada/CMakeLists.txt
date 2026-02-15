# Copyright (c) Tzvetan Mikov.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# Wrapper CMakeLists.txt for vendored Ada.
# WHATWG URL parser with IDNA support.

add_library(ada STATIC ada/ada.cpp)
target_include_directories(ada PUBLIC ada/)
target_compile_features(ada PUBLIC cxx_std_20)
