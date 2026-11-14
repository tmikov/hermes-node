#!/usr/bin/env python3
# Copyright (c) Tzvetan Mikov.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
"""Prove the two sokol_gfx instances cannot reach each other's state.

`sokol_gfx` keeps everything in one process-global (`static _sg_state_t
_sg`).  The compositor compiles it twice -- once plainly for the JS thread
that draws, once behind `sgp_rename.h` for the platform thread that presents
-- and the entire separation rests on that rename having been applied to
every public function, before the declarations rather than merely before the
implementation.

Nothing about that is checked by the compiler.  A function the rename list
missed compiles, links, and then routes one instance's call into the other
instance's `_sg` from the wrong thread; the C++ inline overloads
(`inline void sg_setup(const sg_desc&)`) fail the same way if the rename
lands after the declarations.  So this reads the two objects' symbol tables
and asserts the properties directly:

1.  The producing object defines `sg_*` and no `sgp_*`.
2.  The presenting object defines `sgp_*` and **no `sg_*` at all** -- which
    is the independent check on the generator.  `gen-sokol-rename.py`
    verifies its own list by comparing sokol_gfx.h's declarations against
    its definitions, but both halves are regexes over one file and would
    miss the same unusual spelling.  A count taken from the compiler's
    output shares no machinery with them.
3.  Neither object *references* (undefined-symbol) anything belonging to the
    other's instance.

Undefined `sg_*` symbols in the PRESENTING object are the interesting case
and the reason this is not a one-line `nm | grep`: they are exactly how a
missed rename shows up, since the un-renamed call site compiles to a
reference the producing copy then happily satisfies at link time.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys

# `nm -P` is the POSIX portable output format: "name type value size", one
# per line, on GNU binutils and on macOS alike.  The default format differs
# between the two.
NM_LINE = re.compile(r"^(\S+)\s+(\S)")

# Defined-and-exported symbol types. 'T'/'t' text, 'D'/'d' and 'B'/'b' data,
# 'W'/'w' weak (the C++ inline overloads land here when they are not
# inlined away).
DEFINED_TYPES = set("TtDdBbWwVvRrSsGg")
UNDEFINED_TYPES = set("U")


def find_nm():
    # llvm-nm first: this repo builds with Clang, and on a machine with no
    # binutils installed llvm-nm is what exists.  Either reads both ELF and
    # Mach-O.
    for candidate in ("llvm-nm", "nm"):
        path = shutil.which(candidate)
        if path:
            return path
    return None


def read_symbols(nm, obj):
    """Return (defined, undefined) sets of symbol names in `obj`."""
    try:
        proc = subprocess.run(
            [nm, "-P", obj], stdout=subprocess.PIPE, stderr=subprocess.PIPE
        )
    except OSError as exc:
        sys.stderr.write("check-sgp-isolation.py: could not run %s: %s\n" % (nm, exc))
        return None, None
    if proc.returncode != 0:
        sys.stderr.write(
            "check-sgp-isolation.py: %s failed on %s:\n%s\n"
            % (nm, obj, proc.stderr.decode("utf-8", "replace"))
        )
        return None, None

    defined, undefined = set(), set()
    for raw in proc.stdout.decode("utf-8", "replace").splitlines():
        m = NM_LINE.match(raw)
        if not m:
            continue
        name, kind = m.group(1), m.group(2)
        # Mach-O prefixes C symbols with an underscore; strip exactly one so
        # the name matches what the source wrote.
        if sys.platform == "darwin" and name.startswith("_"):
            name = name[1:]
        if kind in DEFINED_TYPES:
            defined.add(name)
        elif kind in UNDEFINED_TYPES:
            undefined.add(name)
    return defined, undefined


def sokol_names(names, prefix):
    """`sg_*` / `sgp_*` names, excluding the other prefix.

    `sg_` is a prefix of `sgp_`, so a naive startswith("sg_") also matches
    every renamed symbol.  Getting this backwards would make the check pass
    unconditionally, which is the one outcome a checker must not have.
    """
    if prefix == "sg_":
        return {n for n in names if n.startswith("sg_") and not n.startswith("sgp_")}
    return {n for n in names if n.startswith("sgp_")}


def report(label, names, limit=12):
    listed = sorted(names)
    shown = ", ".join(listed[:limit])
    if len(listed) > limit:
        shown += ", ... (%d more)" % (len(listed) - limit)
    sys.stderr.write("  %s (%d): %s\n" % (label, len(listed), shown))


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--producing", required=True, help="sokol_gfx_impl.cpp.o")
    ap.add_argument(
        "--presenting", required=True, help="sokol_gfx_present_impl.cpp.o"
    )
    args = ap.parse_args()

    nm = find_nm()
    if not nm:
        sys.stderr.write(
            "check-sgp-isolation.py: neither llvm-nm nor nm is on PATH.\n"
            "Refusing to report success without having read the symbol "
            "tables.\n"
        )
        return 1

    for path in (args.producing, args.presenting):
        if not os.path.exists(path):
            sys.stderr.write(
                "check-sgp-isolation.py: %s does not exist. Build "
                "hermesNodeCompositorGpu first.\n" % path
            )
            return 1

    prod_def, prod_undef = read_symbols(nm, args.producing)
    if prod_def is None:
        return 1
    pres_def, pres_undef = read_symbols(nm, args.presenting)
    if pres_def is None:
        return 1

    failures = []

    prod_sg = sokol_names(prod_def, "sg_")
    prod_sgp = sokol_names(prod_def, "sgp_")
    pres_sgp = sokol_names(pres_def, "sgp_")
    pres_sg = sokol_names(pres_def, "sg_")

    # An empty result would otherwise satisfy every "must not contain"
    # assertion below. Reading no symbols at all is the failure mode a
    # checker is likeliest to have and least likely to notice.
    if not prod_sg:
        failures.append(
            "the producing object defines no sg_* symbols at all -- it was "
            "not built from sokol_gfx.h, or nm read nothing"
        )
    if not pres_sgp:
        failures.append(
            "the presenting object defines no sgp_* symbols at all -- "
            "sgp_rename.h was not applied"
        )

    if prod_sgp:
        failures.append("the producing object defines renamed symbols")
        report("sgp_* defined in the producing object", prod_sgp)
    if pres_sg:
        failures.append(
            "the presenting object still defines un-renamed sg_* symbols -- "
            "the rename list missed them, or sgp_rename.h was included after "
            "sokol_gfx.h's declarations"
        )
        report("sg_* defined in the presenting object", pres_sg)

    # References, not just definitions. This is where a missed rename in a
    # CALL shows up: the call site compiles to an undefined sg_* that the
    # producing copy then satisfies at link time, silently joining the two
    # instances.
    pres_sg_undef = sokol_names(pres_undef, "sg_")
    if pres_sg_undef:
        failures.append(
            "the presenting object REFERENCES the producing instance's "
            "symbols -- a call that was not renamed will bind to the other "
            "instance's _sg at link time"
        )
        report("sg_* referenced by the presenting object", pres_sg_undef)

    prod_sgp_undef = sokol_names(prod_undef, "sgp_")
    if prod_sgp_undef:
        failures.append(
            "the producing object REFERENCES the presenting instance's "
            "symbols"
        )
        report("sgp_* referenced by the producing object", prod_sgp_undef)

    if failures:
        sys.stderr.write("check-sgp-isolation.py: FAILED\n")
        for f in failures:
            sys.stderr.write("  - %s\n" % f)
        return 1

    # Printed on success too, with the counts. A check whose passing case
    # shows nothing is hard to trust, and the counts are what a reader uses
    # to tell "isolated" from "read nothing".
    print(
        "sgp isolation OK: producing defines %d sg_*, presenting defines %d "
        "sgp_*, no cross-references" % (len(prod_sg), len(pres_sgp))
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
