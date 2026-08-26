#!/usr/bin/env python3
# Copyright (c) Tzvetan Mikov.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
"""RULE_LAUNCH_LINK launcher that cuts the kit instead of linking a binary.

CMake hands us the complete link command it generated for the probe target.
That command is the only place the real closure and the real system-library
list exist -- deriving them any other way means maintaining a second copy
that drifts.  So we take it apart here:

  *.a  ........... merged into one archive, except the force-loaded one
  the force-loaded archive ... copied out whole, kept separate, because
                   merging it would make -force_load pull duplicate symbols
                   the real link never pulls both of
  *.o  ........... dropped; entry objects must not live in the archive or
                   main would be pulled from it
  everything else . recorded in kit.manifest, in order

The `-o` path is not linked, but it IS created, empty, as the last thing we
do.  Without it the probe target has no output, so the build system would
re-cut the kit on every single build; with it, the kit is rebuilt exactly
when one of the archives it is cut from changes.  It lives in the kit
directory itself, so that deleting the kit also deletes the stamp and the
next build cuts it again -- the three real outputs are invisible to the
build system, and a stamp somewhere else would let them stay deleted.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys

FORCE_FLAGS_PRE = ("-force_load", "-Wl,--whole-archive")
FORCE_FLAGS_POST = ("-Wl,--no-whole-archive",)

KIT_ARCHIVE_NAME = "libhermes-node-kit.a"
KIT_MANIFEST_NAME = "kit.manifest"

# libfoo.so, libfoo.so.74, libfoo.so.74.2, libfoo.dylib, libfoo.tbd.  A bare
# ".so" test is not enough: an ICU installed as a runtime package only is
# spelled libicui18n.so.74, and macOS's find_library routinely yields absolute
# .dylib and .tbd paths.  Anything this misses is classified as a driver flag
# and hoisted to the FRONT of the command, which is precisely the ordering
# failure the syslibs list exists to prevent.  Matched against the basename,
# so that a directory component cannot decide the answer.
SHARED_LIB_RE = re.compile(r"\.(so|dylib|tbd)(\.\d+)*$")

# A shared-library basename that can be spelled -l<name> instead: lib<name>
# followed by one extension and nothing after it.
#
# A versioned soname deliberately does NOT match. `libfoo.so.5` would become
# -lfoo, which the linker resolves through the libfoo.so development
# symlink -- and a machine carrying only the runtime package does not have
# that symlink, so -lfoo would fail where the absolute path succeeded.
# Those stay absolute, and the manifest stays host-specific for them.
LINKABLE_LIB_RE = re.compile(r"^lib(.+)\.(so|dylib|tbd)$")


def as_link_flags(arg):
    """("-L<dir>", "-l<name>") for an absolute shared library, else None.

    An absolute path records where the library sat on the machine that cut
    the kit, which says nothing about the machine that will use it: the link
    then fails there naming a directory its user never chose. `-l<name>`
    asks the linker to find it wherever libraries live on the machine doing
    the link.

    The directory is kept as a `-L` rather than dropped, and it costs
    nothing to keep. Measured: a `-L` naming a directory that does not exist
    is ignored silently, exit 0 and no warning, so the flag helps on a
    machine laid out like the build machine -- including one whose ICU was
    under a custom prefix, the one case where the absolute path carried real
    information -- and is inert everywhere else, where plain `-l` finds the
    system copy through the linker's default search path.
    """
    if not os.path.isabs(arg):
        return None
    match = LINKABLE_LIB_RE.match(os.path.basename(arg))
    if not match:
        return None
    return ("-L" + os.path.dirname(arg), "-l" + match.group(1))

# What GNU ar's MRI parser accepts in a filename (binutils arlex.l).  It has
# no quoting -- verified: `create "/tmp/a b/out.a"` is a syntax error, the
# quotes are taken literally -- and whitespace, '#', '*', ';', ',', '&' and
# '(' all terminate or misparse the token.  So an unrepresentable path has to
# be reported rather than escaped.
MRI_SAFE_RE = re.compile(r"^[A-Za-z0-9_./+$:\\-]+$")


def parse_version(header_path):
    """Read HERMES_NODE_VERSION_STRING out of the generated version header.

    The version is derived at build time into a header, not available as a
    CMake variable, which is why this reads the header rather than taking
    the value as an argument.
    """
    text = open(header_path).read()
    m = re.search(r'#define\s+HERMES_NODE_VERSION_STRING\s+"([^"]*)"', text)
    if not m:
        sys.exit(f"make-kit: no HERMES_NODE_VERSION_STRING in {header_path}")
    return m.group(1)


def mri_path(path):
    """Spell `path` so an `ar -M` script can name it, or give up loudly.

    The relative form is tried first and is what normally wins: CMake hands
    us archive paths already relative to the build directory, so a build
    directory whose own name has a space still works -- only a component
    BELOW it can make a path unrepresentable.
    """
    candidates = []
    try:
        candidates.append(os.path.relpath(path))
    except ValueError:
        pass  # different drive on Windows; the absolute form is all there is
    candidates.append(path)
    for cand in candidates:
        if MRI_SAFE_RE.match(cand):
            return cand
    sys.exit(
        "make-kit: cannot name %r in an `ar -M` script.\n"
        "make-kit: ar's script parser has no quoting, and this path contains\n"
        "make-kit: a character outside [A-Za-z0-9_./+$:\\-] (a space, '#',\n"
        "make-kit: '*', ';', ',', '&' or '(' will do it). Build in a\n"
        "make-kit: directory whose path avoids them." % path
    )


def multi_arch_warning(driverflags, kit_dir):
    """Loudly report a link line that names more than one architecture.

    A universal kit has never been cut, and the pieces that would cut one
    are the pieces nobody has run: the `libtool -static` branch below has
    not executed on any machine this was developed on (the macOS spike
    predates this script), and `libtool`, `ld -r` and `lipo` all emit a
    valid-looking EMPTY slice for an architecture whose inputs are missing
    -- warnings only, exit 0, and `lipo -info` reports the slice as real.
    That failure does not surface here; it surfaces as an undefined symbol
    at an app's final link, on the machine of whoever was handed the kit.

    A warning rather than an error, deliberately.  The one pipeline that
    cuts a universal kit today is the macOS release build, which cuts it
    only because `check-hermes-node` depends on the kit target -- so
    failing here would block every macOS release to report something that
    may well be fine.  What it must not do is happen in silence.
    """
    archs = [
        driverflags[i + 1]
        for i, a in enumerate(driverflags)
        if a == "-arch" and i + 1 < len(driverflags)
    ]
    if len(set(archs)) < 2:
        return
    for line in (
        "WARNING: this link line names %d architectures (%s)."
        % (len(set(archs)), ", ".join(sorted(set(archs)))),
        "A universal kit is UNVERIFIED: no two-slice kit has ever been cut",
        "or linked against, and libtool/ld -r/lipo all emit a valid-looking",
        "EMPTY slice for an architecture whose inputs are missing -- warnings",
        "only, exit 0, and lipo -info reports it as real. The failure then",
        "surfaces as an undefined symbol at an app's final link, not here.",
        "Before trusting this kit, check every slice by symbol count, e.g.",
        "  lipo -info %s" % os.path.join(kit_dir, KIT_ARCHIVE_NAME),
        "  nm -arch <arch> %s | wc -l   # once per architecture"
        % os.path.join(kit_dir, KIT_ARCHIVE_NAME),
    ):
        print("make-kit: %s" % line, file=sys.stderr)


def merge_archives(archives, out_path):
    """One archive from many, without linking.

    libtool/ar concatenate members; they do not resolve symbols and do not
    coalesce atoms, so MH_SUBSECTIONS_VIA_SYMBOLS survives on every member
    and the final link folds and dead-strips normally.  This is the whole
    reason the kit is an archive rather than an `ld -r` object.
    """
    if os.path.exists(out_path):
        os.remove(out_path)
    if sys.platform == "darwin":
        # libtool takes an argv, so no quoting question arises here.  Note
        # that this branch has never executed: the kit was developed on
        # Linux and the macOS spike predates this script.  The first machine
        # to run it will be a macOS CI runner.
        subprocess.check_call(["libtool", "-static", "-o", out_path] + archives)
    else:
        script = "create %s\n" % mri_path(out_path)
        script += "".join("addlib %s\n" % mri_path(a) for a in archives)
        script += "save\nend\n"
        subprocess.run(["ar", "-M"], input=script, text=True, check=True)


def parse_link_line(rest):
    """Sort the link command's arguments into the four buckets the kit needs.

    Returns (archives, linkargs, syslibs, driverflags, force_loaded, stamp).
    """
    archives, linkargs, syslibs, driverflags = [], [], [], []
    force_loaded = None
    stamp = None
    i = 0
    while i < len(rest):
        a = rest[i]
        if a == "-o":
            if i + 1 >= len(rest):
                sys.exit("make-kit: -o at the end of the link line")
            stamp = rest[i + 1]
            i += 2
            continue
        if a in FORCE_FLAGS_PRE:
            # Record the flag spelling verbatim so the consumer needs no
            # platform knowledge.  The archive must be the very next token:
            # scanning forward for the first .a instead would silently drop
            # whatever sat between, and would accept a group holding two
            # archives, of which only the first would be whole-archived --
            # the second would land in the merged kit archive instead, losing
            # its static constructors and self-registrations with nothing
            # reported anywhere.  hermes_node_link_setup() is thirty lines of
            # editable CMake, so that is a real thing to guard.
            linkargs.append(a)
            if i + 1 >= len(rest):
                sys.exit("make-kit: %s with no archive after it" % a)
            lib = rest[i + 1]
            if not lib.endswith(".a"):
                sys.exit(
                    "make-kit: expected an archive right after %s, found %r.\n"
                    "make-kit: the kit whole-archives exactly one library and\n"
                    "make-kit: merges the rest; this link line does something\n"
                    "make-kit: else and would be cut wrong." % (a, lib)
                )
            if force_loaded is not None and force_loaded != lib:
                sys.exit(
                    "make-kit: two different whole-archived libraries (%s and\n"
                    "make-kit: %s). The manifest names one." % (force_loaded, lib)
                )
            force_loaded = lib
            linkargs.append("{kit}/" + os.path.basename(lib))
            i += 2
            if a == "-Wl,--whole-archive" and (
                i >= len(rest) or rest[i] not in FORCE_FLAGS_POST
            ):
                sys.exit(
                    "make-kit: more than one archive inside %s.\n"
                    "make-kit: only the first would be whole-archived; the\n"
                    "make-kit: rest would be merged into the kit archive and\n"
                    "make-kit: lose their constructors." % a
                )
            continue
        if a in FORCE_FLAGS_POST:
            linkargs.append(a)
            i += 1
            continue
        if a.endswith(".a"):
            archives.append(a)
        elif a.endswith(".o"):
            pass  # entry object -- see the module docstring
        elif (
            a.startswith("-l")
            or a == "-framework"
            or SHARED_LIB_RE.search(os.path.basename(a))
        ):
            # A list of their own, deliberately. CMake emits system
            # libraries interleaved with archives -- on this tree
            # -lpthread -ldl -lrt -lm sit in the middle of the archive list
            # and the ICU paths sit before libdtoa.a -- but every one of them
            # must end up AFTER every archive, or lazy resolution finds
            # nothing and the final link fails with undefined symbols that
            # look like a broken kit.
            #
            # An absolute shared library is rewritten to -L<dir> -l<name>
            # here (see as_link_flags); one -L per directory, emitted before
            # the first library that needs it, since a -L only applies to
            # the -l flags after it. Repeats are preserved: ICU is listed
            # twice on purpose, for the static interdependency, and
            # collapsing that would break the link it exists to make work.
            flags = as_link_flags(a)
            if flags:
                dir_flag, lib_flag = flags
                if dir_flag not in syslibs:
                    syslibs.append(dir_flag)
                syslibs.append(lib_flag)
            else:
                syslibs.append(a)
            if a == "-framework":
                if i + 1 >= len(rest):
                    sys.exit("make-kit: -framework with no name after it")
                i += 1
                syslibs.append(rest[i])
        else:
            driverflags.append(a)
        i += 1
    return archives, linkargs, syslibs, driverflags, force_loaded, stamp


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--kit-dir", required=True)
    ap.add_argument("--version-header", required=True)
    ap.add_argument("argv", nargs=argparse.REMAINDER)
    args = ap.parse_args()

    # argparse.REMAINDER keeps the "--" separator rather than consuming it,
    # so the compiler would otherwise be mistaken for a link argument.
    argv = args.argv
    if argv and argv[0] == "--":
        argv = argv[1:]
    if not argv:
        sys.exit("make-kit: no link command given")
    cc, rest = argv[0], argv[1:]

    (archives, linkargs, syslibs, driverflags, force_loaded,
     stamp) = parse_link_line(rest)

    if force_loaded is None:
        sys.exit("make-kit: no force-loaded archive found in the link line")
    # The force-loaded archive is named separately above; it must not also be
    # merged, or its members would appear twice.  CMake emits it a second time
    # as a plain dependency, so this drops that occurrence too.
    archives = [a for a in archives if a != force_loaded]
    # An archive CMake listed more than once is one archive.  ar's `addlib`
    # would happily concatenate its members twice, which doubles the kit's
    # size in duplicate members that no link can ever pull.
    seen = set()
    archives = [a for a in archives if not (a in seen or seen.add(a))]

    # Before anything is written, and before the version-agreement check the
    # consumer does: a universal link line cuts a kit nothing has ever
    # verified, and the release pipeline must not be the first place that is
    # noticed.
    multi_arch_warning(driverflags, args.kit_dir)

    os.makedirs(args.kit_dir, exist_ok=True)
    kit_archive = os.path.join(args.kit_dir, KIT_ARCHIVE_NAME)
    merge_archives(archives, kit_archive)
    shutil.copy2(force_loaded,
                 os.path.join(args.kit_dir, os.path.basename(force_loaded)))
    # Final order: the force-load flag and its archive (appended in the
    # loop), then the merged archive, then every system library in the order
    # CMake emitted them.
    linkargs.append("{kit}/" + KIT_ARCHIVE_NAME)
    linkargs.extend(syslibs)

    with open(os.path.join(args.kit_dir, KIT_MANIFEST_NAME), "w") as f:
        f.write("# hermes-node kit manifest -- generated by "
                "utils/make-kit.py, do not edit\n")
        f.write("version: %s\n" % parse_version(args.version_header))
        f.write("cc: %s\n" % cc)
        for d in driverflags:
            f.write("driverflag: %s\n" % d)
        for a in linkargs:
            f.write("linkarg: %s\n" % a)
    if stamp:
        # See the module docstring: this stands in for the binary we did not
        # link, and is what makes the probe target incremental.
        os.makedirs(os.path.dirname(os.path.abspath(stamp)), exist_ok=True)
        open(stamp, "w").close()
    print("make-kit: %d archives merged into %s" % (len(archives), kit_archive),
          file=sys.stderr)


if __name__ == "__main__":
    main()
