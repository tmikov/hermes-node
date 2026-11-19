#!/usr/bin/env bash
# Copyright (c) Tzvetan Mikov.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# Run lit over named tests with exactly the params CMake passes.
#
#   utils/lit-run.sh <build dir> [lit options] <test>...
#
# e.g. utils/lit-run.sh cmake-build-asan -a $PWD/test/ui-present.js
#
# The params are not restated here: they are read from the
# check-hermes-node-js command in <build dir>/build.ninja, which CMake
# generates from HERMES_NODE_LIT_PARAMS (top-level CMakeLists.txt). A list
# kept by hand would drift from that one the first time a param is added,
# and a test run with a param missing does not fail -- test/lit.cfg turns
# an absent path param into one that cannot resolve, so the test reports a
# reason that has nothing to do with what it checks.
#
# Test paths must be absolute, as for any hand-run lit invocation (see
# CLAUDE.md, "Test Infrastructure"). Needs bash and python3 only: no GNU
# userland, so it runs unchanged on macOS's bash 3.2.

if [ $# -lt 2 ]; then
  echo "usage: $0 <build dir> [lit options] <test>..." >&2
  exit 2
fi

build_dir=$1
shift

if [ ! -f "$build_dir/build.ninja" ]; then
  echo "lit-run.sh: $build_dir/build.ninja not found (a Ninja build" \
    "directory configured by CMake is required)" >&2
  exit 2
fi
if [ ! -f "$build_dir/bin/hermes-lit" ]; then
  echo "lit-run.sh: $build_dir/bin/hermes-lit not found; build the" \
    "check-hermes-node-js dependencies first" >&2
  exit 2
fi

# The rest is Python: parsing a Ninja command line portably is not a job
# for sed, and exec'ing from there keeps lit's exit status ours.
exec python3 - "$build_dir" "$@" <<'PYTHON'
import os
import shlex
import sys

build_dir, rest = sys.argv[1], sys.argv[2:]
ninja = os.path.join(build_dir, 'build.ninja')

command = None
with open(ninja, errors='replace') as f:
    in_rule = False
    for line in f:
        if line.startswith('build '):
            in_rule = line.startswith('build CMakeFiles/check-hermes-node-js ')
            continue
        if in_rule and line.lstrip().startswith('COMMAND = '):
            command = line.split('COMMAND = ', 1)[1].rstrip('\n')
            break

if command is None:
    sys.stderr.write(
        'lit-run.sh: no check-hermes-node-js command in %s\n' % ninja)
    sys.exit(2)

# Ninja escapes a literal '$' as '$$'; the rest is shell quoting CMake
# applied, which shlex undoes.
tokens = shlex.split(command.replace('$$', '$'))
params = []
for i, token in enumerate(tokens):
    if token == '--param' and i + 1 < len(tokens):
        params += ['--param', tokens[i + 1]]
if not params:
    sys.stderr.write(
        'lit-run.sh: the check-hermes-node-js command in %s passes no '
        '--param; refusing to run lit without them\n' % ninja)
    sys.exit(2)

lit = os.path.join(build_dir, 'bin', 'hermes-lit')
argv = ['python3', lit] + params + rest
os.execvp(argv[0], argv)
PYTHON
