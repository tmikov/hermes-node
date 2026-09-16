#!/usr/bin/env python3
# Copyright (c) Tzvetan Mikov.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

"""Stand in for %hermes-node in the native corpus suite.

Builds the named script into a standalone executable with `hermes-node
build-native`, runs it, and forwards stdout, stderr and the exit status, so a
test written as `%hermes-node %s | %FileCheck %s` exercises natively compiled
built-in modules without its RUN line changing.

Usage:
  run-native.py --hermes-node H --kit K --work W --source-root S -- SCRIPT [ARG...]
"""

import argparse
import os
import shutil
import signal
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from corpus_util import slug

# A produced executable ignores its script argument -- every argument belongs
# to the program -- so a test that re-spawns itself re-runs its own entry
# instead of a child script, and the re-spawn recurses. This bounds the one
# nobody has found yet; test/native/excluded.txt names the thirteen known to
# do it. test-child-process-exec-timeout.js reached roughly 6,000 live
# processes, growing at about 600 a second, before it was killed by hand.
#
# Three things make the bound hold, and the third is the one usually left
# out. start_new_session=True puts the child and everything below it in one
# process group, so killpg reaps the tree rather than the root, whose death
# would otherwise just orphan the rest. The count is polled, because a
# deadline alone is far too coarse a bound: at 600 a second even a 60-second
# one admits tens of thousands. And the poll backs off from 250 ms, so a bomb
# is caught inside its first few hundred processes while a test that honestly
# takes ten seconds costs six pgrep calls.
#
# lit_config.maxIndividualTestTime is deliberately not the backstop. Its
# setter calls LitConfig.fatal() when psutil is absent, which aborts the
# entire run rather than the one test, and psutil is not a dependency of this
# suite or of hermes-lit -- so on a machine without it, asking for that
# backstop costs the suite instead of bounding it. Both of this wrapper's own
# subprocesses are bounded here instead, which needs nothing installed.
#
# Sixty-four has measured headroom, not assumed headroom: instrumenting
# group_size() to log every poll across a full 112-test run put the maximum
# at 3 (a build's shermes and cc) and at 1 for every artifact. So the ceiling
# sits about twenty times above anything the corpus does, and far below a
# number that troubles the process table. The timeout is the branch that
# fires in practice, and it has: one suite run in about thirty had two
# artifacts hang past it under load, which without this wrapper would have
# hung lit indefinitely rather than failing two tests.
RUN_TIMEOUT_S = 120
BUILD_TIMEOUT_S = 900
MAX_GROUP_PROCESSES = 64
FIRST_POLL_S = 0.25
MAX_POLL_S = 2.0


def group_size(pgid):
    """How many live processes share PGID, or None if that cannot be told.

    pgrep rather than psutil, for the reason above. Exit 1 is pgrep reporting
    an empty group, which is a count of zero and not an error.
    """
    try:
        r = subprocess.run(["pgrep", "-g", str(pgid)],
                           capture_output=True, text=True)
    except OSError:
        return None
    if r.returncode not in (0, 1):
        return None
    return len(r.stdout.split())


def kill_group(p, pgid):
    """SIGKILL the whole group, falling back to the root alone.

    Safe to call more than once: a group that is already gone raises
    ProcessLookupError, which is the success case arriving late.
    """
    try:
        os.killpg(pgid, signal.SIGKILL)
    except (ProcessLookupError, PermissionError):
        try:
            p.kill()
        except OSError:
            pass


def run_guarded(argv, what, timeout, capture=False):
    """Run ARGV in a session of its own, bounded in time and in processes.

    Returns (status, stdout, stderr); the latter two are None unless CAPTURE.
    A breach kills the whole process group and returns a non-zero status, so
    the test fails rather than the machine does.
    """
    pipes = subprocess.PIPE if capture else None
    p = subprocess.Popen(argv, start_new_session=True,
                         stdout=pipes, stderr=pipes, text=True)
    try:
        pgid = os.getpgid(p.pid)
    except ProcessLookupError:
        # Exited between spawn and here. Nothing to guard.
        out, err = p.communicate()
        return p.returncode, out, err

    # The session that lets killpg reap the tree is also what stops anyone
    # else reaping it: the group is no longer lit's, so if this wrapper dies
    # the artifact survives with no bound at all. Every exit from here
    # therefore goes through the kill, including the exits we do not choose.
    # A Ctrl-C during a suite run raises KeyboardInterrupt in this loop, and
    # main() turns SIGTERM into SystemExit; both unwind through the finally.
    # Without it, interrupting a run left a bomb growing unwatched -- which
    # is precisely the case this whole function exists for.
    #
    # SIGKILL of the wrapper is the one hole, and no process can close it
    # from inside: the group is orphaned and only the user can reap it. That
    # is a strictly smaller hole than the one before start_new_session,
    # because SIGKILL is not what a teardown or an impatient Ctrl-C sends.
    try:
        deadline = time.monotonic() + timeout
        interval = FIRST_POLL_S
        while True:
            try:
                # Retrying communicate() after TimeoutExpired loses no output.
                out, err = p.communicate(timeout=interval)
                return p.returncode, out, err
            except subprocess.TimeoutExpired:
                pass
            count = group_size(pgid)
            if count is not None and count > MAX_GROUP_PROCESSES:
                reason = "spawned %d processes (limit %d)" % (
                    count, MAX_GROUP_PROCESSES)
            elif time.monotonic() >= deadline:
                reason = "ran longer than %g s" % timeout
            else:
                interval = min(interval * 2, MAX_POLL_S)
                continue
            kill_group(p, pgid)
            out, err = p.communicate()
            print("run-native: %s %s; killed process group %d" % (
                what, reason, pgid), file=sys.stderr)
            return 1, out, err
    finally:
        if p.poll() is None:
            kill_group(p, pgid)


def fixtures_for(rel):
    """Which fixtures tree belongs beside the artifact for this test.

    A bundled module's __dirname roots at the EXECUTABLE's directory
    (rootDirectoryFor(), lib/bundle/bundle_run.cpp), and the producer packages
    code, not assets -- so a test that reads a data file finds nothing unless
    the right tree sits beside the artifact. Which tree differs by suite,
    because the bundle root does: a top-level test roots at test/, while a
    node-ported test at node-tests/parallel/x.js requires ../common and so
    roots at test/node-tests/, where common/fixtures.js resolves
    common/../fixtures.
    """
    if rel.startswith("node-tests/"):
        return os.path.join("test", "node-tests", "fixtures")
    return os.path.join("test", "fixtures")


def main():
    # SIGINT already unwinds as KeyboardInterrupt; these do not, and lit's
    # teardown sends SIGTERM. Turning them into SystemExit is what gets
    # run_guarded's finally run, so the guarded group dies with us instead of
    # outliving us in a session nothing else can reach.
    for sig in (signal.SIGTERM, signal.SIGHUP):
        signal.signal(sig, lambda n, _f: sys.exit(128 + n))

    ap = argparse.ArgumentParser()
    ap.add_argument("--hermes-node", required=True)
    ap.add_argument("--kit", required=True)
    ap.add_argument("--work", required=True)
    ap.add_argument("--source-root", required=True,
                    help="the repository root, not the test directory")
    ap.add_argument("rest", nargs=argparse.REMAINDER)
    args = ap.parse_args()

    rest = args.rest
    if rest and rest[0] == "--":
        rest = rest[1:]
    if not rest:
        print("run-native: no script given", file=sys.stderr)
        return 2
    script = os.path.abspath(rest[0])
    script_args = rest[1:]

    test_dir = os.path.join(args.source_root, "test")
    rel = os.path.relpath(script, test_dir).replace(os.sep, "/")
    work = os.path.join(args.work, slug(rel))
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work, exist_ok=True)

    exe = os.path.join(work, "app")
    # --kit= with an equals sign: build-native's parser matches "--kit=" by
    # prefix and sends a bare "--kit" to its unknown-option branch.
    #
    # --jobs=1 because lit already runs one worker per CPU. Without it every
    # wrapper would start hardware_concurrency() compiler jobs of its own and
    # the suite would oversubscribe the machine quadratically. These are one-
    # and two-module programs, so serial compilation costs almost nothing.
    status, build_out, build_err = run_guarded(
        [args.hermes_node, "build-native", script, "-o", exe,
         "--kit=" + args.kit, "--jobs=1"],
        "build-native", BUILD_TIMEOUT_S, capture=True)
    if status != 0:
        # To stderr, both streams: the test's own stdout is what FileCheck
        # reads, and build chatter there would be checked as program output.
        sys.stderr.write(build_out or "")
        sys.stderr.write(build_err or "")
        print("run-native: build-native failed for %s" % script,
              file=sys.stderr)
        return 1

    src = os.path.join(args.source_root, fixtures_for(rel))
    dst = os.path.join(work, "fixtures")
    if os.path.isdir(src) and not os.path.exists(dst):
        os.symlink(src, dst)

    status, _, _ = run_guarded([exe] + script_args, "the artifact",
                               RUN_TIMEOUT_S)
    return status


if __name__ == "__main__":
    sys.exit(main())
