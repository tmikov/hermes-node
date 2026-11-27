// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// `hermes-node:imgui` with no loadable addon beside the binary.
//
// A copy of hermes-node-ui in a directory of its own looks for
// `hermes-node-imgui.node` there (the executable's directory, realpath'd)
// and finds nothing. Both `require.resolve()` and `require()` throw
// MODULE_NOT_FOUND naming the full path tried -- what an optional-
// dependency probe branches on -- and a second attempt in the same process
// answers the same, because a failed load is not cached.
//
// Then a file that is not an addon is put there: it exists, so it reaches
// the loader process.dlopen uses, and fails the way that does, with
// ERR_DLOPEN_FAILED.
//
// Needs no display and no built addon.
//
// The binary is hard-linked rather than copied where the filesystem allows
// (about 190 MB under ASAN): the kernel reports a hard link's own path as
// the executable, so its directory is still %t.dir. A copy is the fallback
// across filesystems. A failing run leaves %t.dir behind, since lit stops
// at the first failing RUN line; the next run's first line removes it.
//
// THE MUTATION THIS MUST BREAK: remove the stat() check in
// tools/hermes-node-ui/named_addons.cpp. The missing file then reaches
// dlopen and the MISSING lines say ERR_DLOPEN_FAILED.
//
// REQUIRES: ui
// RUN: rm -rf %t.dir && mkdir -p %t.dir
// RUN: ln %hermes-node-ui %t.dir/hermes-node-ui 2>/dev/null || cp %hermes-node-ui %t.dir/hermes-node-ui
// RUN: %timeout 60 %t.dir/hermes-node-ui %s %t.dir/hermes-node-imgui.node MISSING | %FileCheck --check-prefix=MISSING %s
// RUN: printf 'not an elf' > %t.dir/hermes-node-imgui.node
// RUN: %timeout 60 %t.dir/hermes-node-ui %s %t.dir/hermes-node-imgui.node UNLOADABLE | %FileCheck --check-prefix=UNLOADABLE %s
// RUN: rm -rf %t.dir

'use strict';

const expectedPath = process.argv[2];
const label = process.argv[3];

function attempt(fn) {
  try {
    fn();
    console.log(label + ' loaded');
  } catch (e) {
    if (label === 'MISSING')
      console.log(label + ' ' + e.code + ' path=' +
                  String(e.message).includes(expectedPath));
    else
      console.log(label + ' ' + e.code);
  }
}

if (label === 'MISSING') {
  for (let i = 0; i < 2; ++i) {
    attempt(() => require.resolve('hermes-node:imgui'));
    attempt(() => require('hermes-node:imgui'));
  }
} else {
  attempt(() => require('hermes-node:imgui'));
}

// MISSING: {{^}}MISSING MODULE_NOT_FOUND path=true{{$}}
// MISSING-NEXT: {{^}}MISSING MODULE_NOT_FOUND path=true{{$}}
// MISSING-NEXT: {{^}}MISSING MODULE_NOT_FOUND path=true{{$}}
// MISSING-NEXT: {{^}}MISSING MODULE_NOT_FOUND path=true{{$}}
// MISSING-NOT: MISSING

// UNLOADABLE: {{^}}UNLOADABLE ERR_DLOPEN_FAILED{{$}}
