// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// **The forcing function for the `ui-imgui` lit feature.**
//
// test/lit.cfg turns `ui-imgui` on when hermes-node-ui runs
// test/fixtures/imgui/probe.js, exits 0 and prints `imgui ok ` first. Every
// ImGui test is gated on it, so a feature that went off -- the addon not
// built, not beside the binary, the probe's output reworded, the match in
// lit.cfg drifting -- would turn all of them UNSUPPORTED with nothing red.
// The same hole `sokol-validation` closes with
// test/ui-sokol-validation-feature.js; this is its counterpart.
//
// Two assertions. The AGREE prefix: this file runs the probe itself and its
// answer is the one lit.cfg computed (`%ui-imgui-state`). The ON prefix:
// that answer is `on`.
// Unlike `sokol-validation`, there is no legitimate `off`: the addon is
// built wherever hermes-node-ui is, and a UI binary that cannot load it is a
// defect in every configuration.
//
// Gated on `ui` alone, which is what the probe needs. It opens no window, so
// it runs under %timeout and not %with-display.
//
// THE MUTATIONS THIS MUST BREAK: delete the registerNamedAddons() call in
// tools/hermes-node-ui/main.cpp (both runs answer `off`: ON goes red); make
// lit.cfg's probe match something else (lit says `off`, the binary `on`:
// AGREE and ON go red).
//
// REQUIRES: ui
// RUN: if %timeout 60 %hermes-node-ui %source_dir/test/fixtures/imgui/probe.js > %t.probe 2>/dev/null && head -n 1 %t.probe | grep -q '^imgui ok '; then echo on; else echo off; fi | sed 's/^/agree binary=/' | tr -d '\n' > %t.agree
// RUN: echo ' lit=%ui-imgui-state' >> %t.agree
// RUN: %FileCheck --check-prefix=AGREE %s < %t.agree
// RUN: %FileCheck --check-prefix=ON %s < %t.agree

// AGREE: {{^agree binary=(on lit=on|off lit=off)$}}
// ON: {{^agree binary=on lit=on$}}
