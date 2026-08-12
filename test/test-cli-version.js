// Copyright (c) Tzvetan Mikov.
// RUN: %hermes-node --version | %FileCheck %s
// RUN: %hermes-node -v | %FileCheck %s
// CHECK: hermes-node {{[0-9]+\.[0-9]+\.[0-9]+}}
//
// Test: --version prints a derived version, not an empty or malformed one.
// The exact string depends on the git state, so this only pins the shape --
// enough to catch a version.h that was generated with an empty or missing
// substitution. This file's body is never executed.

'use strict';
