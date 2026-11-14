#!/usr/bin/env python3
# Copyright (c) Tzvetan Mikov.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
"""Generate sgp_rename.h: one `#define sg_x sgp_x` per public sokol_gfx symbol.

`sokol_gfx` keeps its whole state in one process-global (`static _sg_state_t
_sg`) and every call must come from the thread that owns that instance's GPU
context.  The compositor needs two instances -- the JS thread draws, the main
thread presents -- so the implementation is compiled twice, the second time
with this header included FIRST.  See
docs/superpowers/specs/2026-09-18-ui-compositor-design.md, "Two copies of
sokol_gfx".

Three properties of the output are load-bearing, and each is here rather than
in a comment somewhere because getting it wrong is silent:

*   **Only functions are renamed, never types.**  The two instances exchange
    `sg_image`, `sg_view`, `sg_desc` and friends across the boundary (that is
    the whole point -- the presenting side wraps textures the producing side
    made), so a renamed type would give the two copies structurally identical
    but differently-named types and break every signature that crosses.  What
    must not collide is the *code*, because the code reaches `_sg`.

*   **The list is sorted**, so a Sokol bump produces a diff proportional to
    what actually changed rather than one reordered by wherever upstream moved
    a definition.

*   **Declarations and definitions are cross-checked**, and a disagreement is
    a hard error.  The design says to generate from the `SOKOL_API_IMPL`
    definitions; this does that, and then asserts the result matches the
    `SOKOL_GFX_API_DECL` declarations exactly.  A function present in one and
    not the other would otherwise drop out of the rename list quietly, and a
    single un-renamed function is not a compile error -- it links, and then
    one instance's call lands in the other instance's `_sg`.  That is the
    exact failure this whole mechanism exists to prevent, so the generator
    refuses to emit rather than emit something incomplete.
"""
import argparse
import os
import re
import sys

# A public definition: `SOKOL_API_IMPL <return type> sg_name(...)`, at the
# start of a line.  The return type may carry `*` and multiple words
# (`const char*`), which is why it is matched loosely and the name is pinned
# by the `(` that must follow it.
IMPL_RE = re.compile(r"^SOKOL_API_IMPL\s+[A-Za-z_0-9\s\*]+?\b(sg_[A-Za-z_0-9]+)\s*\(")

# The matching declaration, in the header half of the file.
DECL_RE = re.compile(
    r"^SOKOL_GFX_API_DECL\s+[A-Za-z_0-9\s\*]+?\b(sg_[A-Za-z_0-9]+)\s*\("
)

# Sokol's own naming convention for a handle type, e.g.
# `typedef struct sg_view { uint32_t id; } sg_view;`.  Collected so the
# generator can refuse to rename a name that is also a type -- see the
# module docstring's first property.  If upstream ever names a function and a
# type identically this fires; today nothing does.
TYPEDEF_RE = re.compile(r"\}\s*(sg_[A-Za-z_0-9]+)\s*;")


def collect(path):
    impls, decls, types = set(), set(), set()
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            m = IMPL_RE.match(line)
            if m:
                impls.add(m.group(1))
            m = DECL_RE.match(line)
            if m:
                decls.add(m.group(1))
            m = TYPEDEF_RE.search(line)
            if m:
                types.add(m.group(1))
    return impls, decls, types


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--input", required=True, help="path to sokol_gfx.h")
    ap.add_argument("--output", required=True, help="path to write sgp_rename.h")
    ap.add_argument(
        "--prefix",
        default="sgp_",
        help="replacement prefix for the second instance (default: sgp_)",
    )
    args = ap.parse_args()

    impls, decls, types = collect(args.input)

    if not impls:
        sys.stderr.write(
            "gen-sokol-rename.py: found no SOKOL_API_IMPL definitions in %s.\n"
            "Either the file is not sokol_gfx.h, or upstream changed how it\n"
            "marks public functions. Emitting an empty rename header would\n"
            "silently give both sokol_gfx instances the same symbols.\n"
            % args.input
        )
        return 1

    # The cross-check described in the module docstring. Reported in both
    # directions, with the names, because "the counts differ" is not enough to
    # act on.
    only_impl = sorted(impls - decls)
    only_decl = sorted(decls - impls)
    if only_impl or only_decl:
        sys.stderr.write(
            "gen-sokol-rename.py: sokol_gfx.h's public declarations and "
            "definitions disagree.\n"
        )
        if only_decl:
            sys.stderr.write(
                "  declared but never defined: %s\n" % ", ".join(only_decl)
            )
        if only_impl:
            sys.stderr.write(
                "  defined but never declared: %s\n" % ", ".join(only_impl)
            )
        sys.stderr.write(
            "  Refusing to emit: a name missing from the rename list links "
            "fine and then\n  reaches the OTHER instance's _sg state at run "
            "time.\n"
        )
        return 1

    collisions = sorted(impls & types)
    if collisions:
        sys.stderr.write(
            "gen-sokol-rename.py: %s is both a function and a type in "
            "sokol_gfx.h.\n  Renaming it would rename the type too, breaking "
            "every signature that\n  crosses between the two instances. This "
            "generator only knows how to\n  rename functions.\n"
            % ", ".join(collisions)
        )
        return 1

    names = sorted(impls)

    out = []
    out.append("/*")
    out.append(" * Copyright (c) Tzvetan Mikov.")
    out.append(" *")
    out.append(" * This source code is licensed under the MIT license found in the")
    out.append(" * LICENSE file in the root directory of this source tree.")
    out.append(" */")
    out.append("")
    out.append("/* GENERATED FILE -- do not edit, and do not check in.")
    out.append(" *")
    out.append(" * Produced by utils/gen-sokol-rename.py from")
    out.append(" * external/sokol/sokol/sokol_gfx.h. Include this BEFORE")
    out.append(" * sokol_gfx.h -- before its DECLARATIONS, not merely before its")
    out.append(" * implementation: the header also defines C++ inline overloads")
    out.append(" * (`inline void sg_setup(const sg_desc&)`), and those bind to")
    out.append(" * whichever copy was declared first.")
    out.append(" *")
    out.append(" * Functions only. Types are deliberately NOT renamed: the two")
    out.append(" * instances pass sg_image/sg_view/sg_desc values to each other,")
    out.append(" * and a renamed type would break every one of those signatures.")
    out.append(" */")
    out.append("")
    out.append("#ifndef HERMES_NODE_COMPAT_COMPOSITOR_GPU_SGP_RENAME_H")
    out.append("#define HERMES_NODE_COMPAT_COMPOSITOR_GPU_SGP_RENAME_H")
    out.append("")
    out.append("#ifdef SOKOL_GFX_INCLUDED")
    out.append('#error "sgp_rename.h must be included BEFORE sokol_gfx.h"')
    out.append("#endif")
    out.append("")
    for name in names:
        out.append("#define %s %s%s" % (name, args.prefix, name[len("sg_") :]))
    out.append("")
    out.append(
        "/* %d renamed functions, generated from %s. */"
        % (len(names), os.path.basename(args.input))
    )
    out.append("")
    out.append("#endif /* HERMES_NODE_COMPAT_COMPOSITOR_GPU_SGP_RENAME_H */")
    text = "\n".join(out) + "\n"

    # Written only when the content actually changes. CMake reruns this on
    # every build of the target that depends on it; rewriting an identical
    # file would touch its mtime and recompile both ~20k-line sokol_gfx
    # translation units for nothing.
    try:
        with open(args.output, "r", encoding="utf-8") as f:
            if f.read() == text:
                return 0
    except OSError:
        pass

    outdir = os.path.dirname(args.output)
    if outdir:
        os.makedirs(outdir, exist_ok=True)
    with open(args.output, "w", encoding="utf-8") as f:
        f.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
