# picohash: hash library (public domain)
# Compiled as C via a thin wrapper because picohash.h uses void-pointer-to-
# function-pointer casts that are valid in C but illegal in C++.
add_library(picohash STATIC picohash_wrapper.c)
target_include_directories(picohash PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
)
