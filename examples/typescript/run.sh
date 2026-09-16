#!/bin/bash
# Copyright (c) Tzvetan Mikov.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
#
# Runs the TypeScript compiler under hermes-node -- interpreted, from an AOT
# bundle, as a standalone executable, and natively compiled -- and checks what
# each one produced: one specific diagnostic at one specific position, and the
# JavaScript it emitted. Exit status alone would prove nothing here -- tsc
# exits 2 for any error at all, so a compiler that had loaded no standard
# library and complained about `Number` would pass a status check and fail
# this one. (That is not a hypothetical: it is exactly what a bundled run does
# when the lib*.d.ts files are not beside the container.)
#
# It also measures the compile cache, which matters more to this example than
# to any other: `typescript/lib/_tsc.js` is a single 6,213,092-byte module and
# compiling it is most of a cold run's wall clock. Measured on macOS arm64,
# Release: 6.62 s for the run that populates the cache against 0.95 s warm for
# the type-check below. The measurement uses a cache directory under ./out so
# the numbers do not depend on the state of the developer's real cache, and do
# not leave 3.9 MB in it.
#
# The bundle and executable arms run with the cache turned off, because the
# claim they are here to check is that a container needs none: measured 0.94 s
# for both, against 2.64 s interpreted with the cache off and 6.62 s for the
# run that fills it.
#
# No pty is needed: tsc is a plain CLI that reads no keys and draws no screen.
#
# The native build is behind TSC_BUILD_NATIVE=1 because it costs roughly two
# minutes and a 5.8 GB peak RSS -- above the ~3.7 GB the native-compilation
# spec records as a realistic `cc` ceiling -- which is more than a suite that
# runs on every checkout should spend. See README.md.
#
# Usage: ./run.sh [build-dir]     (default: cmake-build-release)
#        TSC_BUILD_NATIVE=1 ./run.sh

set -e

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BUILD_DIR="${1:-$ROOT/cmake-build-release}"
if [ -d "$BUILD_DIR" ]; then
  BUILD_DIR="$(cd "$BUILD_DIR" && pwd)"
fi

HERMES_NODE="$BUILD_DIR/bin/hermes-node"
TS_LIB="$HERE/node_modules/typescript/lib"

if [ ! -f "$HERMES_NODE" ]; then
  echo "ERROR: missing $HERMES_NODE -- build it first:" 1>&2
  echo "  cmake --build $BUILD_DIR --target hermes-node" 1>&2
  exit 1
fi
if [ ! -d "$HERE/node_modules" ]; then
  echo "ERROR: run 'npm install' in $HERE first." 1>&2
  exit 1
fi

# The whole middle section of this script is a cache measurement, and this
# variable is checked before --compile-cache= and turns the cache off
# outright. Exported in a shell that had been running the lit suite by hand,
# it would make the warm run as slow as the cold one and the failure would
# name a ratio rather than the cause.
unset HERMES_NODE_DISABLE_COMPILE_CACHE

OUT="$HERE/out"
cleanup() {
  rm -rf "$OUT"
  # Only ever restore a tree this script moved itself.
  if [ -d "$HERE/.node_modules_hidden" ]; then
    rm -rf "$HERE/node_modules"
    mv "$HERE/.node_modules_hidden" "$HERE/node_modules"
  fi
}
trap cleanup EXIT
rm -rf "$OUT"
mkdir -p "$OUT"

fail() {
  echo "FAIL: $1" 1>&2
  shift
  [ $# -gt 0 ] && printf '%s\n' "$*" | tail -c 800 1>&2
  exit 1
}

# Milliseconds. `date +%s` and bash's SECONDS are both whole seconds, which
# cannot express a warm run; python3 is already required to build this project
# and to run the two TUI examples. Its own startup lands between the two calls
# and so inflates the elapsed time, which makes the ratio asserted below
# conservative rather than generous.
now_ms() { python3 -c 'import time; print(int(time.time() * 1000))'; }

LAST_MS=0

# Runs the compiler and checks what came out. Four assertions, and each is one
# that a tsc which had silently done nothing -- or done the wrong thing --
# would fail:
#
#   - status 2, which is tsc's own "finished, with errors"
#   - exactly one diagnostic. A checker that failed to load lib.d.ts also
#     reports errors, just a great many more of them.
#   - that diagnostic, verbatim: file, line, column, code and text. TS2322 is
#     the assignment on line 12 of src/area.ts.
#   - the emitted JavaScript: annotations stripped, and the CommonJS exports
#     tsc synthesizes for an ES module source. Emit and check are separate
#     halves of tsc and a run can lose either one.
#
# The diagnostics are kept in the output directory so the native run can be
# diffed against the interpreted one rather than merely asserted to resemble
# it.
expect_tsc() {
  local label="$1" outdir="$2"
  shift 2
  rm -rf "$outdir"
  mkdir -p "$outdir"

  local diags status=0 t0 t1
  t0="$(now_ms)"
  diags="$(cd "$HERE" && "$@" --outDir "$outdir" --target es2020 \
      --module commonjs --pretty false src/area.ts 2>&1)" || status=$?
  t1="$(now_ms)"
  LAST_MS=$((t1 - t0))
  printf '%s\n' "$diags" > "$outdir/diagnostics.txt"

  [ "$status" = "2" ] || fail "$label: tsc exited $status, expected 2" "$diags"

  local n
  n="$(printf '%s\n' "$diags" | grep -c 'error TS' || true)"
  [ "$n" = "1" ] || fail "$label: expected exactly 1 diagnostic, got $n" "$diags"
  printf '%s' "$diags" | grep -qF \
    "src/area.ts(12,7): error TS2322: Type 'string' is not assignable to type 'number'." \
    || fail "$label: not the diagnostic src/area.ts is written to produce" "$diags"

  [ -f "$outdir/area.js" ] || fail "$label: tsc emitted no area.js"
  grep -q '^function area(width, height) {$' "$outdir/area.js" \
    || fail "$label: emitted area() is not the de-annotated function" \
            "$(cat "$outdir/area.js")"
  grep -q '^exports.area = area;$' "$outdir/area.js" \
    || fail "$label: emitted JavaScript carries no CommonJS export" \
            "$(cat "$outdir/area.js")"

  echo "  ok: $label (${LAST_MS} ms)"
}

echo "typescript $(grep -m1 '"version"' "$HERE/node_modules/typescript/package.json" \
  | cut -d'"' -f4), _tsc.js is $(wc -c < "$TS_LIB/_tsc.js" | tr -d ' ') bytes"

CACHE="$OUT/compile-cache"

echo "interpreted, cold compile cache:"
expect_tsc "tsc.js with an empty cache" "$OUT/cold" \
  "$HERMES_NODE" --compile-cache="$CACHE" "$HERE/tsc.js"
COLD_MS=$LAST_MS

# The cold run has to have written something, or the "warm" run below is not
# warm and the comparison measures nothing. `config` is excluded because the
# cache writes it on first use whether or not any entry lands beside it --
# counting it would make this check unfailable.
entries="$(find "$CACHE" -type f -not -name config | wc -l | tr -d ' ')"
[ "$entries" -ge 1 ] || fail "the cold run left no compile cache entries in $CACHE"

echo "interpreted, warm compile cache:"
expect_tsc "tsc.js with a populated cache" "$OUT/warm" \
  "$HERMES_NODE" --compile-cache="$CACHE" "$HERE/tsc.js"
WARM_MS=$LAST_MS

# A cache is only worth having if it changes nothing but the time.
diff -u "$OUT/cold/area.js" "$OUT/warm/area.js" \
  || fail "the cached run emitted different JavaScript than the cold run"
diff -u "$OUT/cold/diagnostics.txt" "$OUT/warm/diagnostics.txt" \
  || fail "the cached run reported different diagnostics than the cold run"

echo "compile cache: cold ${COLD_MS} ms, warm ${WARM_MS} ms, $entries entries, $(du -sk "$CACHE" | cut -f1) KB"

# 3x, against a gap measured at 6.7x. The bound is deliberately loose: the
# point of this check is that caching one six-megabyte module is worth
# multiples, not that it is worth some precise figure, and a tight bound on a
# wall-clock ratio would go red under parallel load rather than tell anyone
# anything.
[ "$WARM_MS" -gt 0 ] || fail "warm run measured 0 ms; the clock is not usable here"
if [ $((WARM_MS * 3)) -gt "$COLD_MS" ]; then
  fail "the warm run was only $(( COLD_MS * 100 / WARM_MS ))% of cold; expected at least 300%"
fi
echo "  ok: warm is $(( COLD_MS * 100 / WARM_MS ))% of cold, at least 300% required"

# Both container arms run with the compile cache off. A bundle carries its
# JavaScript already compiled, so it should not want one -- and a bundle that
# was quietly being carried by the developer's warm cache would measure the
# cache over again instead of the container. Set per command, so the cache
# measurement above is untouched and nothing lands in the real cache
# directory. `env` rather than a VAR=value prefix because these expand into
# the middle of expect_tsc's argument list.
NOCACHE=(env HERMES_NODE_DISABLE_COMPILE_CACHE=1)

DIST="$OUT/dist"
echo "hermes-node --build-bundle:"
if ! build_log="$("$HERE/build-bundle.sh" "$DIST" "$BUILD_DIR" 2>&1)"; then
  echo "$build_log" 1>&2
  fail "build-bundle.sh"
fi
[ -f "$DIST/tsc.hbb" ] || fail "$DIST/tsc.hbb was not produced"
# Deliberately not asserting a silent build, which is what tetris and ditz2
# do: this graph is not fully static and the three warnings it produces are
# all benign. See README.md, and the header of build-bundle.sh.
nwarn="$(printf '%s\n' "$build_log" | grep -c '^warning:' || true)"
echo "  ok: $DIST/tsc.hbb, $(wc -c < "$DIST/tsc.hbb" | tr -d ' ') bytes, $nwarn benign producer warnings"

# Arguments for a bundled program go after `--`; hermes-node parses anything
# before it as its own. Omitting it is silent for the flags it happens to
# understand -- `--bundle=tsc.hbb --version` prints hermes-node's version
# rather than tsc's -- and an error for the rest.
echo "hermes-node --bundle:"
expect_tsc "tsc.hbb, no compile cache" "$OUT/bundle-out" \
  "${NOCACHE[@]}" "$HERMES_NODE" --bundle="$DIST/tsc.hbb" --
BUNDLE_MS=$LAST_MS

diff -u "$OUT/warm/area.js" "$OUT/bundle-out/area.js" \
  || fail "the bundled run emitted different JavaScript than the interpreted run"
diff -u "$OUT/warm/diagnostics.txt" "$OUT/bundle-out/diagnostics.txt" \
  || fail "the bundled run reported different diagnostics than the interpreted run"
echo "  ok: identical emit and identical diagnostics, interpreted vs bundled"
echo "bundle: ${BUNDLE_MS} ms with no cache at all, against ${COLD_MS} ms for the run that fills one"

if [ -f "$DIST/tsc" ]; then
  echo "standalone executable:"
  # The point of the executable is that neither the runtime nor the package
  # tree is needed. Moving node_modules away is what proves it rather than
  # asserts it; the trap above puts it back however this exits. What tsc does
  # still need is its standard library, which build-bundle.sh put beside the
  # container -- and which is the same directory the executable re-roots at.
  mv "$HERE/node_modules" "$HERE/.node_modules_hidden"
  expect_tsc "dist/tsc with node_modules moved away" "$OUT/exe-out" \
    "${NOCACHE[@]}" "$DIST/tsc"
  rm -rf "$HERE/node_modules"
  mv "$HERE/.node_modules_hidden" "$HERE/node_modules"

  diff -u "$OUT/warm/area.js" "$OUT/exe-out/area.js" \
    || fail "the executable emitted different JavaScript than the interpreted run"
  diff -u "$OUT/warm/diagnostics.txt" "$OUT/exe-out/diagnostics.txt" \
    || fail "the executable reported different diagnostics than the interpreted run"
  echo "  ok: identical emit and identical diagnostics, interpreted vs executable"
else
  echo "standalone executable: skipped (no link kit in $BUILD_DIR)"
fi

if [ "${TSC_BUILD_NATIVE:-0}" != "1" ]; then
  echo "hermes-node build-native: skipped (set TSC_BUILD_NATIVE=1; ~2 min, ~6 GB RSS)"
  echo "PASS: typescript"
  exit 0
fi

# Asked for explicitly, so a missing toolchain is an error rather than a skip:
# the other examples skip their native arm because they run unattended, and
# this one only ever runs because somebody typed the variable.
if [ ! -x "$BUILD_DIR/kit/shermes" ]; then
  fail "TSC_BUILD_NATIVE=1 but there is no shermes in $BUILD_DIR/kit -- build it with:
  cmake --build $BUILD_DIR --target hermes-node-kit"
fi

NATIVE="$OUT/native"
mkdir -p "$NATIVE"
echo "hermes-node build-native:"
if ! build_log="$("$HERMES_NODE" build-native "$HERE/tsc.js" -o "$NATIVE/tsc" \
    --kit="$BUILD_DIR/kit" --verbose 2>&1)"; then
  echo "$build_log" 1>&2
  fail "build-native"
fi
[ -f "$NATIVE/tsc" ] || fail "$NATIVE/tsc was not produced"
echo "  ok: $NATIVE/tsc, $(wc -c < "$NATIVE/tsc" | tr -d ' ') bytes"

# Fully native, not a native shell around interpreted bytecode. The one blob
# the binary is allowed to carry is Hermes's own extensions unit; a second
# would mean some module had been packaged as bytecode after all.
python3 "$ROOT/test/fixtures/native/count-magic.py" "$NATIVE/tsc" 1 \
  || fail "$NATIVE/tsc carries more than the one expected bytecode blob"
echo "  ok: exactly one bytecode blob (Hermes's own extensions unit)"

# tsc finds lib.es2020.d.ts and friends by looking beside the file it is
# executing. Inside a bundle that file's __dirname is its build-time identity
# re-rooted at the executable's own directory, so the standard library has to
# be placed at exactly that path. The producer packages code, not data, which
# is the same reason gtop's terminfo travels beside its artifact.
mkdir -p "$NATIVE/node_modules/typescript/lib"
cp "$TS_LIB"/lib*.d.ts "$NATIVE/node_modules/typescript/lib/"
echo "  ok: $(ls "$NATIVE/node_modules/typescript/lib" | wc -l | tr -d ' ') lib*.d.ts files copied beside the artifact"

echo "native executable:"
expect_tsc "$NATIVE/tsc" "$OUT/native-out" "$NATIVE/tsc"

# The real claim is not that each run is individually acceptable -- it is that
# the natively compiled compiler and the interpreted one agree, byte for byte,
# on both halves of what tsc produces. Asserting each separately would let the
# two drift as far apart as the assertions are loose.
diff -u "$OUT/warm/area.js" "$OUT/native-out/area.js" \
  || fail "the native build emitted different JavaScript than the interpreted run"
diff -u "$OUT/warm/diagnostics.txt" "$OUT/native-out/diagnostics.txt" \
  || fail "the native build reported different diagnostics than the interpreted run"
echo "  ok: identical emit and identical diagnostics, interpreted vs native"

echo "PASS: typescript"
