#!/bin/bash
# Copyright (c) Tzvetan Mikov.
# Test process.stdout and process.stderr output separation.
# Verifies that stdout output goes to stdout and stderr output goes to stderr.
# Usage: run-stdio-test.sh <hermes-node-binary> <source-dir>
set -euo pipefail

HERMES_NODE="$1"
SRC_DIR="$2"
TEST_SCRIPT="$SRC_DIR/test/test-stdio.js"

# Capture stdout and stderr separately.
STDOUT_FILE=$(mktemp)
STDERR_FILE=$(mktemp)
trap 'rm -f "$STDOUT_FILE" "$STDERR_FILE"' EXIT

"$HERMES_NODE" --node-lib-path "$SRC_DIR" "$TEST_SCRIPT" \
  > "$STDOUT_FILE" 2> "$STDERR_FILE" || {
  echo "test-stdio.js: FAILED (non-zero exit). stdout:"
  cat "$STDOUT_FILE"
  echo "stderr:"
  cat "$STDERR_FILE"
  exit 1
}

STDOUT_CONTENT=$(cat "$STDOUT_FILE")
STDERR_CONTENT=$(cat "$STDERR_FILE")

FAILED=0

# Check PASS on stdout.
if ! echo "$STDOUT_CONTENT" | grep -q "^PASS$"; then
  echo "test-stdio.js: FAILED — PASS not found on stdout."
  echo "stdout: $STDOUT_CONTENT"
  FAILED=1
fi

# Check stdout-test on stdout.
if ! echo "$STDOUT_CONTENT" | grep -q "stdout-test"; then
  echo "test-stdio.js: FAILED — 'stdout-test' not found on stdout."
  FAILED=1
fi

# Check console-test on stdout.
if ! echo "$STDOUT_CONTENT" | grep -q "console-test"; then
  echo "test-stdio.js: FAILED — 'console-test' not found on stdout."
  FAILED=1
fi

# Check stderr-test on stderr.
if ! echo "$STDERR_CONTENT" | grep -q "stderr-test"; then
  echo "test-stdio.js: FAILED — 'stderr-test' not found on stderr."
  FAILED=1
fi

# stderr-test should NOT be on stdout.
if echo "$STDOUT_CONTENT" | grep -q "stderr-test"; then
  echo "test-stdio.js: FAILED — 'stderr-test' appeared on stdout (should be stderr)."
  FAILED=1
fi

if [ "$FAILED" -ne 0 ]; then
  echo "stdout was:"
  cat "$STDOUT_FILE"
  echo "stderr was:"
  cat "$STDERR_FILE"
  exit 1
fi

echo "test-stdio.js: passed"
exit 0
