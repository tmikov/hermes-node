// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// RED whenever lit's `display` probe found the display BUSY: another lit
// invocation or a hand run held the machine-wide display lock
// (utils/with-display.py, macOS) for the probe's whole lock-wait budget.
//
// Without this, a busy display would read as "no display": `display`
// false, every window test UNSUPPORTED, and lit counting the run as a
// success on a developer's Mac -- the silent skip this suite keeps finding.
// A missing display can be a local choice; a busy one never is, so this
// fails on every machine, not only where a display was promised, and says
// why. Nothing is wrong with the code when it fires: rerun once the other
// run has finished, and never run two lit invocations over one build
// directory at once.
//
// (Mutation: make utils/display_probe.py report busy for every probe -- this
// goes red, and test/ui-display-feature.js does too where a display is
// expected.)
//
// REQUIRES: display-busy
// RUN: echo 'display %display-state (%display-reason)' | %FileCheck %s

// CHECK: the display was not busy
