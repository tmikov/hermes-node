/**
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// utils/make-kit.py decides how every library on CMake's link line is
// recorded in kit.manifest, and it had no test: it runs only while cutting
// a kit, so a classification mistake surfaces at somebody else's link.
//
// The checks live in make-kit-classify.py, which lit does not collect
// itself (config.suffixes is .js/.ts). This file is the driver. It needs no
// hermes-node and no kit -- it imports the script and calls it.

// RUN: python3 %source_dir/test/make-kit-classify.py | %FileCheck %s
// CHECK: PASS
