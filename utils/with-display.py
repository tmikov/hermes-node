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

1. An exclusive `flock` on `/tmp/hermes-node-ui-display.lock` -- a FIXED,
   machine-wide path, overridable only by `HERMES_NODE_UI_DISPLAY_LOCK`
   (which exists for test/with-display-lock.js) -- so that window tests run
   one at a time however many lit workers, lit invocations or hand runs
   there are. Not optional: Sokol centres every window, two tests' windows
   of the same size cover each other exactly, the covered one loses
   `NSWindowOcclusionStateVisible`, its swapchain goes invalid, and its
   capture never matches. Not under `$TMPDIR`: lit points TMPDIR at a fresh
   `lit_tmp_*` directory per invocation, and on macOS TMPDIR is per-user
   anyway, so a lock there serialised only the tests of one lit run.
   The file is created mode 0666 (forced past the umask) so that another
   user of the machine can take it too; a lock file that exists and cannot
   be opened for writing -- another user created it under a stricter umask
   before this rule -- is opened read-only, which is all `flock` needs.
   Never stale: an `flock` belongs to the open file, and the kernel drops it
   when the last descriptor closes, which happens when this process exits
   however it exits, SIGKILL included. The command does not inherit the
   descriptor (subprocess closes it), so the lock is exactly this wrapper's
   lifetime. Waiting for it COUNTS AGAINST the time limit rather than being
   added to it: a run that cannot get the display within its limit exits
   124 saying so, and a run that waited is given only what is left. In
   Python, not flock(1), which macOS does not have.
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
import time

HERE = os.path.dirname(os.path.abspath(__file__))
OWN_FAILURE = 125
LOCK_PATH = '/tmp/hermes-node-ui-display.lock'


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


def open_lock(path):
    """A descriptor on the lock file, creating it world-accessible; None
    (with a message) if it can be opened neither way."""
    try:
        fd = os.open(path, os.O_RDWR | os.O_CREAT, 0o666)
        try:
            # Past the umask, so another user of the machine can open it.
            # Fails harmlessly if the file is someone else's.
            os.fchmod(fd, 0o666)
        except OSError:
            pass
        return fd
    except PermissionError:
        pass
    except OSError as e:
        sys.stderr.write('with-display.py: %s: %s\n' % (path, e))
        return None
    try:
        # Exists, owned by another user, not writable by us: flock needs
        # only an open descriptor, whatever its access mode.
        return os.open(path, os.O_RDONLY)
    except OSError as e:
        sys.stderr.write('with-display.py: %s: %s\n' % (path, e))
        return None


def run_darwin(seconds, command):
    import fcntl

    timeout = load_timeout()
    try:
        limit = float(seconds)
    except ValueError:
        return usage()

    deadline = time.monotonic() + limit
    lock_path = os.environ.get('HERMES_NODE_UI_DISPLAY_LOCK') or LOCK_PATH
    fd = open_lock(lock_path)
    if fd is None:
        return OWN_FAILURE
    try:
        # Bounded by the limit: polled rather than blocking, because a
        # blocking flock() cannot be given a deadline.
        while True:
            try:
                fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
                break
            except BlockingIOError:
                pass
            if time.monotonic() >= deadline:
                sys.stderr.write(
                    'with-display.py: timed out after %s s waiting for the '
                    'display lock %s\n' % (seconds, lock_path))
                return timeout.TIMED_OUT
            time.sleep(0.05)
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            return timeout.TIMED_OUT

        env = dict(os.environ)
        env['HERMES_NODE_UI_TEST_NO_ACTIVATE'] = '1'
        env['HERMES_NODE_UI_TEST_PASTEBOARD'] = 'hnui-test-%d%d' % (
            os.getpid(), time.time_ns())
        # subprocess closes inherited descriptors by default, so the lock is
        # held by this process alone and goes when it does.
        return timeout.run(remaining, command, env=env, foreground=False)
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
