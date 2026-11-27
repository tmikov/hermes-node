/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// Dear ImGui's build configuration for the addon, named by
// IMGUI_USER_CONFIG (addons/imgui/CMakeLists.txt) so that the vendored
// imconfig.h stays unmodified. Every ImGui unit and every addon unit sees
// this file, which is what keeps them agreeing about ImGui's types.

#ifndef HERMES_NODE_IMGUI_CONFIG_H
#define HERMES_NODE_IMGUI_CONFIG_H

// Only the current API: nothing in the addon may come to depend on a name
// ImGui is about to remove.
#define IMGUI_DISABLE_OBSOLETE_FUNCTIONS

// ImGui's default clipboard and URL-opening functions call the platform
// directly (Win32 and macOS ones exist). The clipboard goes through hnui,
// and nothing else may reach the platform behind the host's back.
#define IMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS

// ImWchar is 32 bits, so a character outside the Basic Multilingual Plane
// (an emoji, four UTF-8 bytes) reaches a text field intact.
#define IMGUI_USE_WCHAR32

#endif // HERMES_NODE_IMGUI_CONFIG_H
