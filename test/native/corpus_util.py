#!/usr/bin/env python3
# Copyright (c) Tzvetan Mikov.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

"""Shared by run-native.py and check-corpus.py.

The wrapper derives a work directory per test and deletes it before building;
the checker enforces that those directories are distinct. Both must agree on
how the name is derived, so it lives here once -- a collision would have two
parallel wrappers rmtree each other's build.
"""

import hashlib
import os

# A test whose SOLE RUN line is one of these is mechanically wrappable.
# ShTest executes EVERY RUN line, so a file with a second one runs a command
# the wrapper cannot reproduce -- which is why the criterion is "sole", not
# "contains".
#
# RUN_BARE is the shape that has no FileCheck: the test asserts for itself and
# reports by exit status. Fifteen top-level tests are written that way, and
# leaving the shape out did not merely skip them -- it made them invisible,
# since a file matching no shape is a candidate for nothing and the checker
# says nothing about it. crypto, repl, querystring, dgram, https, tls,
# cluster and diagnostics_channel were reachable only through those fifteen.
RUN_TOP = "// RUN: %hermes-node %s | %FileCheck %s"
RUN_BARE = "// RUN: %hermes-node %s"
RUN_NODE = "// RUN: TEST_THREAD_ID=$$ %hermes-node %s"


def slug(rel):
    """A unique directory name for a test path relative to test/.

    A digest and nothing else, which costs readability on purpose. A
    Unix socket path must fit sockaddr_un.sun_path -- 104 bytes on macOS,
    108 on Linux -- and the tests that bind one build it inside this
    directory, so every character here comes out of that budget. A
    readable prefix (node-tests_parallel_test-net-server-listen-path_js)
    is 49 characters, which put both socket tests over the limit: bind
    truncated, two names collided, and the failures read as EADDRINUSE
    and ENOENT rather than as a length problem. Measured worst case with
    this name is in docs/superpowers/plans/progress-native-builtins.md.

    To go from a directory back to its test: grep the digest against
    `python3 -c "import corpus_util; ..."`, or just re-derive it -- it is
    sha1 of the path relative to test/, first 8 hex digits.
    """
    rel = rel.replace(os.sep, "/")
    return hashlib.sha1(rel.encode()).hexdigest()[:8]


def run_lines(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        return [l.strip() for l in f if l.strip().startswith("// RUN:")]


def shapes_for(rel):
    """The wrappable RUN shapes a test at this path may use.

    A node-ported test needs TEST_THREAD_ID, which the ported common/
    modules read to name a per-run temp directory; a top-level test either
    pipes into FileCheck or reports by exit status.
    """
    if rel.replace(os.sep, "/").startswith("node-tests/"):
        return (RUN_NODE,)
    return (RUN_TOP, RUN_BARE)


def scan(test_dir):
    """(contains, candidates) as sets of paths relative to test_dir.

    contains  -- has a wrappable RUN line anywhere
    candidates -- its SOLE RUN line is a wrappable one
    """
    contains, candidates = set(), set()
    for root, dirs, files in os.walk(test_dir):
        dirs[:] = [d for d in dirs
                   if d not in ("native", "fixtures", "common", "Output")]
        for name in files:
            if not name.endswith((".js", ".ts")):
                continue
            rel = os.path.relpath(os.path.join(root, name), test_dir)
            rel = rel.replace(os.sep, "/")
            lines = run_lines(os.path.join(root, name))
            for shape in shapes_for(rel):
                if shape in lines:
                    contains.add(rel)
                    if lines == [shape]:
                        candidates.add(rel)
                    break
    return contains, candidates
