add_library(zlib_a STATIC
  zlib/adler32.c
  zlib/compress.c
  zlib/crc32.c
  zlib/deflate.c
  zlib/gzclose.c
  zlib/gzlib.c
  zlib/gzread.c
  zlib/gzwrite.c
  zlib/inflate.c
  zlib/infback.c
  zlib/inftrees.c
  zlib/inffast.c
  zlib/trees.c
  zlib/uncompr.c
  zlib/zutil.c
)
target_include_directories(zlib_a PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/zlib)
