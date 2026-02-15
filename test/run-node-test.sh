#!/bin/bash
# Copyright (c) Tzvetan Mikov.
# Run a Node.js test file with hermes-node.
# Node tests exit 0 on success and non-zero on failure.
# Usage: run-node-test.sh <hermes-node-binary> <source-dir> <test-script>
set -euo pipefail

HERMES_NODE="$1"
SRC_DIR="$2"
TEST_SCRIPT="$3"

TEST_NAME=$(basename "$TEST_SCRIPT")

# Run with NODE_SKIP_FLAG_CHECK to avoid Flags: header processing that
# requires child_process. Also skip global leak checks since our environment
# differs from Node.
OUTPUT=$("$HERMES_NODE" --node-lib-path "$SRC_DIR" "$TEST_SCRIPT" 2>&1) || {
  EXIT_CODE=$?
  echo "$TEST_NAME: FAILED (exit code $EXIT_CODE). Output:"
  echo "$OUTPUT"
  exit 1
}

echo "$TEST_NAME: passed"
exit 0
