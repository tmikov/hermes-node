# Copyright (c) Tzvetan Mikov.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

add_hermes_library(hermesNodeBindings STATIC
  handle_wrap_base.cpp
  libuv_stream_base.cpp
  node_async_context_frame.cpp
  node_async_wrap.cpp
  node_buffer.cpp
  node_cares_wrap.cpp
  node_config.cpp
  node_constants.cpp
  node_credentials.cpp
  node_encoding.cpp
  node_errors.cpp
  node_file.cpp
  node_file_dir.cpp
  node_os.cpp
  node_fs_event_wrap.cpp
  node_stdio.cpp
  node_stream_wrap.cpp
  node_string_decoder.cpp
  node_symbols.cpp
  node_task_queue.cpp
  node_timers.cpp
  node_trace_events.cpp
  node_tty_wrap.cpp
  node_types.cpp
  node_url.cpp
  node_util.cpp
  node_uv.cpp
)

target_include_directories(hermesNodeBindings
  PUBLIC
    ${CMAKE_SOURCE_DIR}/include
  PRIVATE
    ${CMAKE_SOURCE_DIR}/hermes/include/hermes/napi
)

target_link_libraries(hermesNodeBindings
  PUBLIC
    uv_a
  PRIVATE
    simdutf
    ada
    cares_a
)
