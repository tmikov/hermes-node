#!/usr/bin/env bash
#
# Copyright (c) Tzvetan Mikov.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
#
# Build a distributable Linux hermes-node inside an old-glibc container.
#
# An ordinary local build inherits your machine's glibc: symbol versions bind
# against your libc.so at link time, glibc's headers redirect to newer entry
# points, and CMake's feature checks find whatever your system has. None of
# that is a build option, which is why this is a container and not a flag.
# Building against glibc 2.28 resolves every one of those the other way, with
# no source change, and 2.28 is also Node's own Tier 1 x64 requirement.
#
# This mirrors the build-linux job in .github/workflows/release.yml. The image,
# the CMake flags and the glibc budget are duplicated between the two on
# purpose -- CI runs its build *as* a container job and cannot call this script
# -- so if you change one, change the other. The floor assertion below is what
# stops the two silently drifting into different answers.

set -euo pipefail

REPO_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)

IMAGE="docker.io/library/almalinux:8"
OUT="${REPO_ROOT}/cmake-build-portable"
GLIBC_BUDGET="GLIBC_2.28"
ENGINE=""
VERSION=""
JOBS=""

usage() {
  cat <<'USAGE'
Usage: utils/build-portable.sh [options]

Builds hermes-node and the --build-exe kit in a container with an old glibc,
producing a tree that runs on distributions far older than this machine.

Options:
  --image <ref>    Container image to build in (default: almalinux:8)
  --out <dir>      Output directory (default: cmake-build-portable/)
  --version <str>  Version to stamp (default: git describe, else 0.0.0-dev)
  --jobs <n>       Parallel build jobs (default: the container's nproc)
  --engine <name>  podman or docker (default: whichever is found)
  -h, --help       This message

Output layout, which is what a release tarball contains:

  <out>/dist/hermes-node
  <out>/dist/kit/{kit.manifest,libhermes-node-kit.a,libhermesNapi.a,
                  hermes-node-bundle-main.o}

The kit sits beside the binary because that is where --build-exe looks by
default, so from <out>/dist the feature works with no --kit flag.

This does not run the test suite: the container has no writable checkout and
the ported Node tests create temp directories inside the source tree. Use an
ordinary local build for testing, and CI for testing this configuration.
USAGE
}

while [ $# -gt 0 ]; do
  case "$1" in
    --image)   IMAGE="$2"; shift 2 ;;
    --out)     OUT="$2"; shift 2 ;;
    --version) VERSION="$2"; shift 2 ;;
    --jobs)    JOBS="$2"; shift 2 ;;
    --engine)  ENGINE="$2"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
  esac
done

if [ -z "$ENGINE" ]; then
  if command -v podman >/dev/null 2>&1; then
    ENGINE=podman
  elif command -v docker >/dev/null 2>&1; then
    ENGINE=docker
  else
    echo "error: neither podman nor docker found on PATH." >&2
    echo "       This build needs a container runtime; install one, or pass" >&2
    echo "       --engine with something compatible." >&2
    exit 1
  fi
elif ! command -v "$ENGINE" >/dev/null 2>&1; then
  echo "error: --engine '$ENGINE' is not on PATH." >&2
  exit 1
fi

# Derived on the host: the container sees the source read-only and, in a
# worktree, .git is a file pointing outside the mount, so git there cannot
# answer. Passing the version in also keeps the stamp identical to what a
# local build would produce from the same checkout.
if [ -z "$VERSION" ]; then
  VERSION=$(git -C "$REPO_ROOT" describe --tags --always --dirty --match 'v*' \
            2>/dev/null || echo "0.0.0-dev")
fi

mkdir -p "$OUT"

INNER="${OUT}/.container-build.sh"
cat > "$INNER" <<INNER_EOF
#!/bin/sh
set -e

echo "=== toolchain ==="
# Separate transactions: dnf abandons the whole set over one unresolvable
# name. ccache lives in EPEL. No ninja -- it is in powertools, not EPEL.
dnf install -y epel-release >/dev/null
dnf install -y cmake clang python3 git make binutils >/dev/null
cmake --version | head -1
clang --version | head -1
ldd --version | head -1

echo "=== configure ==="
# POSITION_INDEPENDENT_CODE is load-bearing: this image's clang defaults to
# non-PIE, and without PIC the kit's objects cannot be linked on a
# PIE-default distribution, which is most of them.
cmake -S /src -B /out/build \\
  -DCMAKE_BUILD_TYPE=Release \\
  -DCMAKE_C_COMPILER=clang \\
  -DCMAKE_CXX_COMPILER=clang++ \\
  -DCMAKE_POSITION_INDEPENDENT_CODE=ON \\
  -DHERMES_NODE_VERSION="${VERSION}" \\
  > /out/configure.log 2>&1 || { tail -30 /out/configure.log; exit 1; }

echo "=== build hermes-node ==="
cmake --build /out/build --target hermes-node -j "${JOBS:-\$(nproc)}" \\
  > /out/build.log 2>&1 || { tail -40 /out/build.log; exit 1; }

echo "=== build kit ==="
cmake --build /out/build --target hermes-node-kit -j "${JOBS:-\$(nproc)}" \\
  > /out/kit.log 2>&1 || { tail -40 /out/kit.log; exit 1; }

echo "=== glibc floor ==="
max=\$(objdump -T /out/build/bin/hermes-node \\
      | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1)
echo "highest glibc symbol version required: \$max"
highest=\$(printf '%s\\n%s\\n' "\$max" "${GLIBC_BUDGET}" | sort -V | tail -1)
if [ "\$highest" != "${GLIBC_BUDGET}" ]; then
  echo "error: glibc floor is \$max, budget is ${GLIBC_BUDGET}" >&2
  exit 1
fi

echo "=== stage ==="
rm -rf /out/dist
mkdir -p /out/dist/kit
cp /out/build/bin/hermes-node /out/dist/
# Named individually: kit.stamp is a build-system artifact and must not ship,
# and a renamed output should fail here rather than produce a partial kit.
cp /out/build/kit/kit.manifest \\
   /out/build/kit/libhermes-node-kit.a \\
   /out/build/kit/libhermesNapi.a \\
   /out/build/kit/hermes-node-bundle-main.o \\
   /out/dist/kit/

echo "=== smoke test ==="
/out/dist/hermes-node --version
echo 'console.log("portable build OK");' > /tmp/hello.js
/out/dist/hermes-node /tmp/hello.js
INNER_EOF
chmod +x "$INNER"

echo "building in ${IMAGE} (engine: ${ENGINE}, version: ${VERSION})"
"$ENGINE" run --rm \
  -v "${REPO_ROOT}:/src:ro" \
  -v "${OUT}:/out" \
  "$IMAGE" /out/.container-build.sh

cat <<DONE

wrote ${OUT}/dist
  hermes-node plus kit/, laid out as a release tarball is.

To check it links off this machine's toolchain, which is the thing a kit
exists for and the thing CI's in-container smoke test cannot prove:

  ${OUT}/dist/hermes-node --build-bundle=/tmp/app.hbb <entry>.js
  ${OUT}/dist/hermes-node --build-exe=/tmp/app /tmp/app.hbb
  /tmp/app
DONE
