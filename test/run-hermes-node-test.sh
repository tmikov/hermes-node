#!/bin/bash
# Copyright (c) Tzvetan Mikov.
# Run a JS test with hermes-node and check for PASS in output.
# Usage: run-hermes-node-test.sh <hermes-node-binary> <source-dir> <test-script>
set -euo pipefail

HERMES_NODE="$1"
SRC_DIR="$2"
TEST_SCRIPT="$3"

OUTPUT=$("$HERMES_NODE" --node-lib-path "$SRC_DIR" "$TEST_SCRIPT" 2>&1)

# Check that PASS appears in the output.
if echo "$OUTPUT" | grep -q "^PASS$"; then
  echo "$(basename "$TEST_SCRIPT"): passed"
  exit 0
else
  echo "$(basename "$TEST_SCRIPT"): FAILED. Output:"
  echo "$OUTPUT"
  exit 1
fi
