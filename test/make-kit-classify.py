#!/usr/bin/env python3
# Copyright (c) Tzvetan Mikov.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

"""Checks how utils/make-kit.py classifies a link line's libraries.

Driven by make-kit-classify.js; lit does not collect .py files itself.

make-kit.py has no other test. It runs only as part of cutting a kit, which
needs a full build, so a mistake in its argument classification is normally
discovered at somebody else's link -- which is exactly the class of failure
the manifest exists to prevent.
"""

import importlib.util
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
MAKE_KIT = os.path.join(os.path.dirname(HERE), "utils", "make-kit.py")

spec = importlib.util.spec_from_file_location("make_kit", MAKE_KIT)
make_kit = importlib.util.module_from_spec(spec)
spec.loader.exec_module(make_kit)

failures = []


def check(what, actual, expected):
    if actual != expected:
        failures.append("%s\n     got: %r\nexpected: %r" % (what, actual, expected))


def syslibs_for(args):
    """The syslibs bucket parse_link_line() produces for `args`."""
    # A link line parse_link_line() will accept: it insists on exactly one
    # whole-archived library, and records a stamp from the -o.
    rest = [
        "-o",
        "stamp",
        "-Wl,--whole-archive",
        "libhermesNapi.a",
        "-Wl,--no-whole-archive",
    ] + args
    _archives, _linkargs, syslibs, _driverflags, _forced, _stamp = (
        make_kit.parse_link_line(rest)
    )
    return syslibs


# An absolute shared library becomes -L<dir> -l<name>. The absolute path is
# where the library sat on the machine that cut the kit, which says nothing
# about the machine that will use it.
check(
    "single absolute .so",
    syslibs_for(["/usr/lib/x86_64-linux-gnu/libicuuc.so"]),
    ["-L/usr/lib/x86_64-linux-gnu", "-licuuc"],
)

# One -L per directory, however many libraries come from it. Repeated
# libraries stay repeated: make-kit.py emits ICU twice on purpose, for the
# static interdependency, and dropping the repeat would break that link.
check(
    "several libraries from one directory",
    syslibs_for(
        [
            "/usr/lib/x86_64-linux-gnu/libicuuc.so",
            "/usr/lib/x86_64-linux-gnu/libicui18n.so",
            "/usr/lib/x86_64-linux-gnu/libicuuc.so",
        ]
    ),
    ["-L/usr/lib/x86_64-linux-gnu", "-licuuc", "-licui18n", "-licuuc"],
)

# A -l flag CMake already emitted is left exactly as it is.
check(
    "existing -l flags untouched",
    syslibs_for(["-lpthread", "-ldl", "-lm"]),
    ["-lpthread", "-ldl", "-lm"],
)

# A versioned soname is NOT translatable. `-lfoo` resolves through the
# libfoo.so development symlink, which a machine with only the runtime
# package does not have, so -lfoo would fail where the absolute path
# succeeds. Left alone, and the manifest stays host-specific for it.
check(
    "versioned soname left absolute",
    syslibs_for(["/usr/lib/x86_64-linux-gnu/libfoo.so.5"]),
    ["/usr/lib/x86_64-linux-gnu/libfoo.so.5"],
)

# macOS. A .dylib is translatable the same way; a -framework and its name
# are a pair that must survive together and unchanged.
check(
    "dylib translated, framework pair intact",
    syslibs_for(["/usr/lib/libz.dylib", "-framework", "CoreFoundation"]),
    ["-L/usr/lib", "-lz", "-framework", "CoreFoundation"],
)

# Not an absolute path, so there is no directory to hoist into a -L.
check(
    "relative path left alone",
    syslibs_for(["build/libfoo.so"]),
    ["build/libfoo.so"],
)

if failures:
    for f in failures:
        print("FAIL: %s" % f)
    sys.exit(1)
print("PASS")
