/**
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// tools/embedded_module_ids.py's validate() is what stops a future built-in
// module id from silently colliding onto another one's Static Hermes unit
// name, or from producing a name shermes rejects outright. It has no other
// test: every id in the real manifest is clean today, so the reject paths
// are otherwise never exercised.
//
// The checks live in tools/test_embedded_module_ids.py, which lit does not
// collect itself (config.suffixes is .js/.ts). This file is the driver. It
// needs no hermes-node -- it imports the shared module and calls it.

// RUN: python3 %source_dir/tools/test_embedded_module_ids.py | %FileCheck %s
// CHECK: PASS
