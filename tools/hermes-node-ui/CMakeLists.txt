# Copyright (c) Tzvetan Mikov.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# hermes-node-ui: the UI compositor binary. The top-level CMakeLists.txt
# only add_subdirectory()s this directory when HERMES_NODE_ENABLE_UI is on,
# so a build without the option produces no trace of it -- no target, no
# binary, nothing for the rest of this file to guard a second time.

# UiHost is its own library rather than compiled straight into the
# hermes-node-ui executable, so unittests/UiHostTest.cpp can link it too
# (RULING 41): the predicate's two halves are each only safely testable
# with the code under test in a process a GTest controls directly -- a lit
# test alone cannot bound a runtime-finished-only regression (the suite
# configures no per-test timeout, so a hang there blocks a worker slot
# indefinitely) or exercise the window-requested branch (nothing in this
# build can call requestWindow() from a real script). Header and source
# stay in this directory, per the plan's File Structure; only how they are
# compiled changes.
add_hermes_library(hermesNodeUiHost STATIC ui_host.cpp)
target_include_directories(hermesNodeUiHost PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(hermesNodeUiHost PUBLIC hermesNodeRuntime uv_a)

# The platform-event converter (sapp_event -> UiEvent) and the test hook
# that injects platform events into it are their own library for the same
# reason UiHost is: unittests/UiEventsTest.cpp links it. It
# includes sokol_app.h for declarations only and calls no sapp_* function,
# so linking it drags in neither the sokol_app implementation nor a display.
# ui_events.cpp, the JS-thread half of input, is NOT here: it calls into
# NAPI and the runtime's error path, and lives in the executable beside
# ui_module.cpp.
add_hermes_library(hermesNodeUiEvents STATIC
  sapp_event_convert.cpp
  event_injector.cpp
)
target_include_directories(hermesNodeUiEvents PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(hermesNodeUiEvents PUBLIC hermesNodeCompositor sokol)

# named_addons.cpp registers `hermes-node:imgui`, whose init loads
# hermes-node-imgui.node (addons/imgui/) from this binary's directory at
# run time. There is no link dependency on the addon.
add_hermes_tool(hermes-node-ui main.cpp named_addons.cpp)
# Plain signature, not PRIVATE: hermes_node_link_setup() below links this
# same target with the plain form (matching Hermes.cmake:141's own plain
# link of hermesNapi, per that file's comment), and CMake refuses to mix
# keyword and plain target_link_libraries calls against one target.
target_link_libraries(hermes-node-ui hermesNodeUiHost hermesNodeUiEvents)

# hermes_node_link_setup() is defined in tools/hermes-node/CMakeLists.txt.
# CMake functions have ordinary (global) scope once defined -- unlike a
# variable, which is directory-scoped -- so this call is valid only because
# the top-level CMakeLists.txt add_subdirectory()s tools/hermes-node before
# tools/hermes-node-ui. Reusing it rather than duplicating its body gives
# hermes-node-ui the same NAPI whole-archive link, -rdynamic/export_dynamic
# export and dead-stripping hermes-node itself gets: both binaries link
# hermesNodeRuntime, and both need a dlopen'd addon to resolve the same NAPI
# surface, so the two link recipes must not be allowed to drift apart.
hermes_node_link_setup(hermes-node-ui)

# main.cpp builds an EmbedderModule entry, which needs the NAPI declarations
# hermesNodeRuntime keeps PRIVATE (lib/runtime/CMakeLists.txt). Same line,
# and the same reason, as tools/hermes-node/CMakeLists.txt.
target_include_directories(hermes-node-ui PRIVATE
  ${PROJECT_SOURCE_DIR}/hermes/include/hermes/napi
)

# Window support. Unconditional: lib/compositor-gpu builds a backend on
# every platform the option configures on, and refuses to configure on any
# other, so hermesNodeCompositorGpu always exists here.
#
# ui_module.cpp (the NAPI surface the JS thread calls), ui_platform.cpp (the
# sokol_app callbacks the platform thread runs) and ui_test_hooks.cpp hold
# the window, sharing ui_internal.h, and sokol_app_impl.cpp is the single
# compilation of sokol_app's implementation. All go into the EXECUTABLE
# rather than into hermesNodeUiHost, deliberately: that library is what
# unittests/UiHostTest.cpp links, and pulling sokol_app and a GL backend
# into a GTest that only exercises a condition-variable predicate would make
# the test need a display to link. UiHost reaches the window through a
# function pointer main() sets (see setWindowRunner) precisely so that stays
# true.
#
# hnui_api.cpp implements the public C entry points an addon calls. It
# lives in the EXECUTABLE rather than in a library for the reason the
# comment above gives for ui_module.cpp and its siblings -- and for one of
# its own: the symbols are exported from this binary
# (hermes_node_link_setup's -rdynamic) so a dlopen'd addon resolves them,
# which only works if they are linked into the binary rather than left in
# an archive member nothing references.
target_sources(hermes-node-ui PRIVATE
  ui_module.cpp
  ui_platform.cpp
  ui_test_hooks.cpp
  ui_events.cpp
  hnui_api.cpp
  sokol_app_impl.cpp
)
# The platform services (platform_services.h): the clipboard, both
# directions, and the trace's read-backs of the title, icon and clipboard
# ownership. One implementation per platform, chosen here rather than by
# a preprocessor conditional in ui_platform.cpp. X11's sits on our own
# clipboard read, which replaces Sokol's (Ruling 55), and is built under
# the condition the GL backend is built under, which is what provides X11.
# macOS's is platform_services_mac.mm, added with the Metal sources
# further down.
if(UNIX AND NOT APPLE)
  target_sources(hermes-node-ui PRIVATE
    platform_services_x11.cpp
    x11_readback.cpp
    x11_clipboard.cpp
  )
endif()
if(APPLE)
  # macOS: the NSApplication subclass and the window helpers
  # (mac_app.mm), the in-process window driver the test hooks export
  # (mac_test_driver.mm), and the platform services. sokol_app's Metal
  # implementation is Objective-C, so its unit is compiled as
  # Objective-C++ under its .cpp name, the way lib/compositor-gpu compiles
  # the two sokol_gfx units; ARC on every Objective-C(++) unit, which
  # sokol_config.h enforces for the Sokol ones.
  target_sources(hermes-node-ui PRIVATE
    mac_app.mm
    mac_test_driver.mm
    platform_services_mac.mm
  )
  set_source_files_properties(sokol_app_impl.cpp
    PROPERTIES COMPILE_OPTIONS "-xobjective-c++;-fobjc-arc")
  set_source_files_properties(mac_app.mm mac_test_driver.mm
    platform_services_mac.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
endif()
# Plain signature, matching the call above -- CMake refuses to mix plain
# and keyword forms on one target, and hermes_node_link_setup() already
# links this one plainly.
target_link_libraries(hermes-node-ui hermesNodeCompositorGpu)
