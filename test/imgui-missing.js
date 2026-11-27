// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// `hermes-node:imgui` with no loadable addon beside the binary, and then
// with one, in ONE process: a failed load is not cached, so each attempt
// answers for the file as it is at that moment.
//
// A copy of hermes-node-ui in a directory of its own looks for
// `hermes-node-imgui.node` there (the executable's directory, realpath'd).
//
//  MISSING     nothing there: both `require.resolve()` and `require()`
//              throw MODULE_NOT_FOUND naming the full path tried -- what an
//              optional-dependency probe branches on -- and a second round
//              answers the same.
//  UNLOADABLE  the script then writes ten bytes that are not an addon
//              there: the file exists, so it reaches the loader
//              process.dlopen uses, and the next `require()` fails the way
//              that does, with ERR_DLOPEN_FAILED.
//  LOADED      given a third argument, the path of the real addon, the
//              script then puts that in the garbage file's place, and the
//              next `require()` succeeds and returns the addon's exports.
//              Only test/imgui-retry.js passes it, since it needs a built
//              addon; this file's own run needs neither a display nor one.
//
// The binary is hard-linked rather than copied where the filesystem allows
// (about 190 MB under ASAN): the kernel reports a hard link's own path as
// the executable, so its directory is still %t.dir. A copy is the fallback
// across filesystems; the addon is placed the same way. A failing run
// leaves %t.dir behind, since lit stops at the first failing RUN line; the
// next run's first line removes it.
//
// THE MUTATIONS THIS MUST BREAK (recorded in progress-imgui-addon.md):
//  - remove the stat() check in tools/hermes-node-ui/named_addons.cpp: the
//    missing file reaches dlopen and the MISSING lines say
//    ERR_DLOPEN_FAILED;
//  - cache a failed load (a scratch edit of requireEmbedderModuleExports
//    in libjs/loader.js that rethrows the first error for the name): the
//    UNLOADABLE line says MODULE_NOT_FOUND, here and in
//    test/imgui-retry.js.
//
// REQUIRES: ui
// RUN: rm -rf %t.dir && mkdir -p %t.dir
// RUN: ln %hermes-node-ui %t.dir/hermes-node-ui 2>/dev/null || cp %hermes-node-ui %t.dir/hermes-node-ui
// RUN: %timeout 60 %t.dir/hermes-node-ui %s %t.dir/hermes-node-imgui.node | %FileCheck %s
// RUN: rm -rf %t.dir

'use strict';

const fs = require('fs');

const expectedPath = process.argv[2];
const realAddon = process.argv[3];

function attempt(label, fn) {
  try {
    const result = fn();
    if (label === 'LOADED')
      console.log(label + ' version=' + result.version);
    else
      console.log(label + ' loaded');
  } catch (e) {
    console.log(
        label + ' ' + e.code + ' path=' +
        String(e.message).includes(expectedPath));
  }
}

for (let i = 0; i < 2; ++i) {
  attempt('MISSING', () => require.resolve('hermes-node:imgui'));
  attempt('MISSING', () => require('hermes-node:imgui'));
}

fs.writeFileSync(expectedPath, 'not an elf');
attempt('UNLOADABLE', () => require('hermes-node:imgui'));

if (realAddon !== undefined) {
  fs.unlinkSync(expectedPath);
  try {
    fs.linkSync(realAddon, expectedPath);
  } catch (e) {
    fs.copyFileSync(realAddon, expectedPath);
  }
  attempt('LOADED', () => require('hermes-node:imgui'));
}
console.log('DONE');

// CHECK: {{^}}MISSING MODULE_NOT_FOUND path=true{{$}}
// CHECK-NEXT: {{^}}MISSING MODULE_NOT_FOUND path=true{{$}}
// CHECK-NEXT: {{^}}MISSING MODULE_NOT_FOUND path=true{{$}}
// CHECK-NEXT: {{^}}MISSING MODULE_NOT_FOUND path=true{{$}}
// CHECK-NEXT: {{^}}UNLOADABLE ERR_DLOPEN_FAILED path=true{{$}}
// CHECK-NEXT: {{^}}DONE{{$}}
