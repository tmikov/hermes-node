#!/usr/bin/env python3
# Copyright (c) Tzvetan Mikov.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

"""Module-id helpers shared by the two embedded-registry generators.

gen-embedded-registry.py names byte arrays after a module;
gen-native-registry.py names Static Hermes units after the same module. Both
read this, so those two cannot drift.

CMake is the third, and it is NOT single-sourced here: it needs a unit name
inside its own loops, before the combined manifest exists, so it derives
hn_b_<safe_id> itself. That derivation is cross-checked against this one at
build time by gen-native-registry.py, which refuses to emit on a
disagreement -- see lib/embedded-modules/CMakeLists.txt. Two implementations
checked against each other, not one.
"""

import re
import sys

# shermes accepts an exported-unit name of ASCII alphanumerics and underscore
# only, and nothing else (hermes/include/hermes/Utils/Options.h).
_VALID_UNIT = re.compile(r"^[A-Za-z0-9_]+$")


def safe_id(module_id):
    """A C identifier fragment for a module id.

    e.g. 'internal/errors' -> 'internal__errors'
         'internal/streams/add-abort-signal' ->
             'internal__streams__add_abort_signal'
    """
    return module_id.replace("/", "__").replace("-", "_")


def unit_name(module_id):
    """The Static Hermes unit name for a built-in module.

    The hn_b_ prefix is load bearing. build-native names a *user* module's
    unit hn_m<index> (nativeUnitName(), lib/build-native/staging.cpp), both
    become sh_export_<name> symbols in one linked binary, and a collision is a
    duplicate-symbol failure at the customer's link rather than at ours.
    """
    return "hn_b_" + safe_id(module_id)


def parse_manifest(path):
    """[(module_id, is_bootstrap)] from a combined manifest file."""
    modules = []
    with open(path, "r") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            if line.startswith("@bootstrap "):
                modules.append((line[len("@bootstrap "):].strip(), True))
            else:
                modules.append((line, False))
    return modules


def validate(modules):
    """Refuse a manifest whose ids do not map to distinct, legal unit names.

    Raises SystemExit naming the offender. Both checks are real: safe_id
    replaces only '/' and '-', so an id containing '.' or '@' yields a name
    shermes rejects outright, and 'a-b' and 'a_b' both yield 'a_b'.
    """
    seen = {}
    for module_id, _ in modules:
        name = unit_name(module_id)
        if not _VALID_UNIT.match(name):
            sys.exit(
                "embedded module id %r maps to unit name %r, which shermes "
                "will reject: only ASCII letters, digits and underscore are "
                "allowed. Extend safe_id() to cover the new character."
                % (module_id, name)
            )
        if name in seen:
            sys.exit(
                "embedded module ids %r and %r both map to unit name %r. "
                "Unit names must be distinct; extend safe_id() so they are."
                % (seen[name], module_id, name)
            )
        seen[name] = module_id
