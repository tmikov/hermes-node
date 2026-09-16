#!/usr/bin/env python3
# Copyright (c) Tzvetan Mikov.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

"""Reconcile the native corpus manifests against the test tree.

Usage: check-corpus.py <test dir>

Fails if a test with a wrappable RUN line is in neither manifest, if an entry
is in both, if an entry is stale, if a corpus entry is not a candidate, if an
exclusion carries no reason, if a line is duplicated, or if two corpus entries
would share a work directory.

Run by check-hermes-node-native before the suite, so the corpus cannot shrink
when someone edits a RUN line, or grow when someone adds a test.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from corpus_util import scan, slug


def read_list(path, want_reason):
    entries, problems, seen = {}, [], set()
    if not os.path.exists(path):
        return entries, ["missing manifest: " + path]
    with open(path, encoding="utf-8") as f:
        for n, line in enumerate(f, 1):
            line = line.rstrip("\n")
            if not line.strip() or line.lstrip().startswith("#"):
                continue
            if want_reason:
                name, sep, reason = line.partition(":")
                name = name.strip()
                if not sep or not reason.strip():
                    problems.append("%s:%d: expected '<path>: <reason>'"
                                    % (path, n))
                    continue
                value = reason.strip()
            else:
                name, value = line.strip(), ""
            if name in seen:
                problems.append("%s:%d: %s listed twice" % (path, n, name))
                continue
            seen.add(name)
            entries[name] = value
    return entries, problems


def main():
    if len(sys.argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    test_dir = os.path.abspath(sys.argv[1])
    here = os.path.dirname(os.path.abspath(__file__))
    contains, candidates = scan(test_dir)
    corpus, problems = read_list(os.path.join(here, "corpus.txt"), False)
    excluded, p2 = read_list(os.path.join(here, "excluded.txt"), True)
    problems += p2

    for rel in sorted(contains - set(corpus) - set(excluded)):
        problems.append(
            "%s has a wrappable RUN line but is in neither corpus.txt nor "
            "excluded.txt" % rel)
    for rel in sorted(set(corpus) & set(excluded)):
        problems.append("%s is in both corpus.txt and excluded.txt" % rel)
    for rel in sorted(set(corpus) | set(excluded)):
        if rel not in contains:
            problems.append(
                "%s is listed but no longer has a wrappable RUN line (stale "
                "entry, or the test was edited or deleted)" % rel)
    for rel in sorted(set(corpus) - candidates):
        if rel in contains:
            problems.append(
                "%s is in corpus.txt but is not a candidate: ShTest runs all "
                "its RUN lines and the wrapper cannot reproduce the others"
                % rel)

    # run-native.py derives a work directory per test and deletes it before
    # building. Two tests sharing one would race.
    slugs = {}
    for rel in sorted(corpus):
        s = slug(rel)
        if s in slugs:
            problems.append("%s and %s share the work directory %s"
                            % (slugs[s], rel, s))
        slugs[s] = rel

    if problems:
        for p in problems:
            print("check-corpus: " + p, file=sys.stderr)
        return 1
    print("check-corpus: %d candidates, %d in corpus, %d excluded"
          % (len(candidates), len(corpus), len(excluded)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
