#!/usr/bin/env python3
# Copyright (c) Tzvetan Mikov.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

"""Checks embedded_module_ids.validate()'s two rejection branches.

Driven by test/embedded-module-ids.js; lit does not collect .py files
itself.

validate() is what stops a future 188th built-in module from silently
colliding onto an existing Static Hermes unit name, or from producing a name
shermes rejects outright. Both branches need a regression test: nothing else
in the build ever exercises the reject path, since today's 187 module ids are
all clean.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import embedded_module_ids as e

failures = []


def expect_rejected(what, modules, needle):
    try:
        e.validate(modules)
    except SystemExit as ex:
        if needle not in str(ex):
            failures.append(
                "%s: message %r does not mention %r" % (what, str(ex), needle)
            )
        return
    failures.append("%s: validate() accepted it" % what)


def expect_accepted(what, modules):
    try:
        e.validate(modules)
    except SystemExit as ex:
        failures.append("%s: validate() rejected it: %s" % (what, ex))


# An id containing a character safe_id() does not translate (only '/' and
# '-' are) produces a unit name shermes's identifier rule rejects.
expect_rejected(
    "illegal character",
    [("a.b", False)],
    "shermes",
)

# safe_id() maps both '-' and '/' to '_'/'__', so 'a-b' and 'a_b' collide on
# the same unit name even though they are distinct module ids.
expect_rejected(
    "collision",
    [("a-b", False), ("a_b", False)],
    "must be distinct",
)

# A manifest with no illegal characters and no collisions is accepted.
expect_accepted(
    "clean manifest",
    [("fs", False), ("internal/errors", False)],
)

if failures:
    for f in failures:
        print("FAIL: %s" % f)
    sys.exit(1)
print("PASS")
