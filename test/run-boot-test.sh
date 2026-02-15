#!/bin/bash
# Copyright (c) Tzvetan Mikov.
# Run bootstrap test with hermes-node.
# Usage: run-boot-test.sh <hermes-node-binary> <source-dir>
set -euo pipefail

HERMES_NODE="$1"
SRC_DIR="$2"

OUTPUT=$("$HERMES_NODE" --node-lib-path "$SRC_DIR" "$SRC_DIR/test/test-boot.js" 2>&1)

# Check that PASS appears in the output.
if echo "$OUTPUT" | grep -q "^PASS$"; then
  echo "Bootstrap test passed"
  exit 0
else
  echo "Bootstrap test FAILED. Output:"
  echo "$OUTPUT"
  exit 1
fi
