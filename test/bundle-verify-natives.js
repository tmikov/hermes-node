// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.
//
// --verify-natives is an audit, not an enforcement: it reports OK, MISSING,
// ERROR or MISMATCH against the sidecar file sitting beside the container
// at the moment it runs, and exits non-zero when anything but OK shows up.

// RUN: rm -rf %t.dir && mkdir -p %t.dir
// RUN: cp %hello_addon %t.dir/hello_addon.node
// RUN: cp %source_dir/test/fixtures/bundle-natives/main.js %t.dir/main.js
// RUN: %hermes-node --build-bundle=%t.dir/app.hbb %t.dir/main.js > /dev/null

// RUN: %hermes-node --bundle=%t.dir/app.hbb --verify-natives | %FileCheck --check-prefix=OK %s
// OK: OK {{ *}}hello_addon.node

// A changed addon is reported and the exit code is non-zero.
// RUN: printf 'x' >> %t.dir/hello_addon.node
// RUN: %not %hermes-node --bundle=%t.dir/app.hbb --verify-natives 2>&1 | %FileCheck --check-prefix=BAD %s
// BAD: MISMATCH {{ *}}hello_addon.node
// BAD: error: 1 of 1 native addon

// A deleted addon likewise.
// RUN: rm %t.dir/hello_addon.node
// RUN: %not %hermes-node --bundle=%t.dir/app.hbb --verify-natives 2>&1 | %FileCheck --check-prefix=GONE %s
// GONE: MISSING {{ *}}hello_addon.node

// Present but unreadable is not MISSING: a directory at the sidecar path
// opens fine and fails on the first read, which is a different thing to
// tell an operator than "the file is not there". The reason rides on the
// row itself, without --verbose, because nobody re-runs a CI job to find
// out what went wrong.
// RUN: mkdir -p %t.dir/hello_addon.node
// RUN: %not %hermes-node --bundle=%t.dir/app.hbb --verify-natives 2>&1 | %FileCheck --check-prefix=ERR %s
// ERR: ERROR {{ *}}hello_addon.node ({{.*}}): cannot read {{.*}}hello_addon.node: {{.+}}
// ERR: error: 1 of 1 native addon
// RUN: rmdir %t.dir/hello_addon.node

// A container with no natives says so rather than printing nothing: silence
// in a log is indistinguishable from the verb not having run.
// RUN: rm -rf %t.plain && mkdir -p %t.plain
// RUN: echo "console.log('x');" > %t.plain/main.js
// RUN: %hermes-node --build-bundle=%t.plain/app.hbb %t.plain/main.js > /dev/null
// RUN: %hermes-node --bundle=%t.plain/app.hbb --verify-natives | %FileCheck --check-prefix=NONE %s
// NONE: no native addons recorded

// A bundle reached through a symlink to the container file itself: the
// sidecar lives beside the file the link resolves to, not beside the link.
// This is the shape a "current" deployment symlink takes, and it is exactly
// what openBundle() (bundle_run.cpp) realpath's the bundle path for before
// it dlopens a sidecar -- verifyNatives() has to agree, or it could report
// OK (or MISSING) about a directory the run never consults.
// RUN: rm -rf %t.link && mkdir -p %t.link
// RUN: cp %hello_addon %t.link/hello_addon.node
// RUN: cp %source_dir/test/fixtures/bundle-natives/main.js %t.link/main.js
// RUN: %hermes-node --build-bundle=%t.link/app.hbb %t.link/main.js > /dev/null
// RUN: rm -f %t.link.symlink.hbb && ln -s %t.link/app.hbb %t.link.symlink.hbb
// RUN: %hermes-node --bundle=%t.link.symlink.hbb --verify-natives | %FileCheck --check-prefix=SYMLINK %s
// SYMLINK: OK {{ *}}hello_addon.node

// This file is a lit driver only; the RUN lines above are the test.
