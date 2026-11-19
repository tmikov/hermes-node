#!/usr/bin/env python3
# Copyright (c) Tzvetan Mikov.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

"""Run one command with a display to open windows on, and a time limit.

    with-display.py SECONDS COMMAND [ARG...]

What `%with-display` expands to in every window test's RUN line
(test/lit.cfg), and what the `display` feature's own probe runs through, so
that the probe cannot pass by a route the tests do not take. One spelling
for both platforms; what it does differs because what a "display" is
differs.

**Linux** -- `exec`s `timeout SECONDS <python> with-xvfb.py COMMAND...`:
the identical process tree to the `timeout SECONDS %with-xvfb COMMAND...`
the RUN lines spelled before, so nothing about a Linux run changes. The
private Xvfb, its teardown and every property the RUN lines depend on are
with-xvfb.py's; see its docstring.

**macOS** -- there is one real display, shared with whoever is using the
machine, so instead:

1. An exclusive `flock` on `$TMPDIR/hermes-node-ui-display.lock`, taken
   **before** the time limit starts, so that window tests run one at a time
   machine-wide however many lit workers there are. Not optional: Sokol
   centres every window, two tests' windows of the same size cover each
   other exactly, the covered one loses `NSWindowOcclusionStateVisible`,
   its swapchain goes invalid, and its capture never matches. In Python,
   not flock(1), which macOS does not have.
2. `HERMES_NODE_UI_TEST_NO_ACTIVATE=1`, so the window does not take the
   keyboard from whoever is at the Mac, and a unique
   `HERMES_NODE_UI_TEST_PASTEBOARD`, so the suite never overwrites the
   user's clipboard and parallel suite runs do not share one.
3. The command runs in its own process group under the time limit (the
   same code as utils/timeout.py): exit 124 after killing the group on
   expiry, otherwise the command's status.

stdout and stderr are the command's own on both platforms.
"""

import importlib.util
import os
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
OWN_FAILURE = 125


def usage():
    sys.stderr.write('usage: with-display.py SECONDS COMMAND [ARG...]\n')
    return OWN_FAILURE


def load_timeout():
    # By path rather than `import timeout`: this script may be run with -I,
    # which keeps its own directory off sys.path.
    spec = importlib.util.spec_from_file_location(
        'hnui_timeout', os.path.join(HERE, 'timeout.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def run_linux(seconds, command):
    argv = ['timeout', seconds, sys.executable,
            os.path.join(HERE, 'with-xvfb.py')] + command
    try:
        os.execvp(argv[0], argv)
    except OSError as e:
        sys.stderr.write('with-display.py: timeout: %s\n' % e)
        return OWN_FAILURE


def run_darwin(seconds, command):
    import fcntl

    timeout = load_timeout()
    try:
        limit = float(seconds)
    except ValueError:
        return usage()

    lock_dir = os.environ.get('TMPDIR') or tempfile.gettempdir()
    lock_path = os.path.join(lock_dir, 'hermes-node-ui-display.lock')
    try:
        # O_RDONLY is enough for flock, and works on a lock file another
        # user created.
        fd = os.open(lock_path, os.O_RDONLY | os.O_CREAT, 0o666)
    except OSError as e:
        sys.stderr.write('with-display.py: %s: %s\n' % (lock_path, e))
        return OWN_FAILURE
    try:
        # Blocking, and before the limit starts: the time a test spends
        # waiting for the display is not time it spent running.
        fcntl.flock(fd, fcntl.LOCK_EX)

        env = dict(os.environ)
        env['HERMES_NODE_UI_TEST_NO_ACTIVATE'] = '1'
        env['HERMES_NODE_UI_TEST_PASTEBOARD'] = 'hnui-test-%d%d' % (
            os.getpid(), time.time_ns())
        # subprocess closes inherited descriptors by default, so the lock is
        # held by this process alone and goes when it does.
        return timeout.run(limit, command, env=env, foreground=False)
    finally:
        os.close(fd)


def main(argv):
    if len(argv) < 2:
        return usage()
    seconds, command = argv[0], argv[1:]
    if sys.platform == 'darwin':
        return run_darwin(seconds, command)
    return run_linux(seconds, command)


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
