#!/bin/bash
# Copyright (c) Tzvetan Mikov.
# Run primordials test by concatenating the shim with the test file.
# Usage: run-primordials-test.sh <hermes-binary> <source-dir>
set -euo pipefail

HERMES="$1"
SRC_DIR="$2"

TMPFILE=$(mktemp /tmp/primordials-test.XXXXXX.js)
trap "rm -f '$TMPFILE'" EXIT

cat "$SRC_DIR/libjs/primordials.js" "$SRC_DIR/test/primordials.js" > "$TMPFILE"
"$HERMES" -Xasync-generators "$TMPFILE"
