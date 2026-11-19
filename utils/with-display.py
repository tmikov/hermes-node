#!/usr/bin/env python3
# Copyright (c) Tzvetan Mikov.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

"""Run one command with a display to open windows on, and a time limit.

    with-display.py SECONDS COMMAND [ARG...]
    with-display.py --print-lock-path

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
   The file is created mode 0666 (forced past the umask, and only when this
   run created it) so that another user of the machine can take it too; an
   existing one is opened read-only, which is all `flock` needs. A path that
   is a symlink, a FIFO or anything but a regular file is REFUSED: exit 125
   with one line naming the path and the problem, without running the
   command -- see open_lock() for why, and for why a hard link is safe.
   Never stale: an `flock` belongs to the open file, and the kernel drops it
   when the last descriptor closes, which happens when this process exits
   however it exits, SIGKILL included. The command does not inherit the
   descriptor (subprocess closes it), so the lock is exactly this wrapper's
   lifetime. Waiting for it is NOT charged to the time limit: the queue for
   the display is a property of the machine (every window test of every lit
   worker queues on this one lock), and a test's limit exists to bound the
   test, so SECONDS starts once the lock is held and bounds only the
   command. The wait has its own budget, `HERMES_NODE_UI_DISPLAY_LOCK_WAIT`
   seconds (default 1800); a run that exhausts it exits 125 -- never 124,
   which would read as the test timing out -- with one stderr line naming
   the lock, and does not run its command. Under
   `HERMES_NODE_DEBUG_NATIVE=UI` a run reports how long it waited, on this
   wrapper's own stderr, which no RUN line captures. In
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

import errno
import importlib.util
import math
import os
import stat
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
OWN_FAILURE = 125
LOCK_PATH = '/tmp/hermes-node-ui-display.lock'
# How long a run may queue for the display before giving up, unless
# HERMES_NODE_UI_DISPLAY_LOCK_WAIT says otherwise. Long, because the queue
# is every window test of every lit worker on the machine.
LOCK_WAIT_SECONDS = 1800


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


def lock_path():
    """The display lock's path: the override, else the fixed default."""
    return os.environ.get('HERMES_NODE_UI_DISPLAY_LOCK') or LOCK_PATH


def open_lock(path):
    """(fd, None) on the lock file, or (None, what is wrong with it).

    Hardened against another local user, since the default path is in
    world-writable /tmp and macOS has nothing like Linux's
    protected_symlinks:

    - Created with O_CREAT|O_EXCL|O_NOFOLLOW, so a planted symlink -- to a
      file of ours, or to one that does not exist -- is never followed and
      nothing is created through it. Only a file THIS call created is
      fchmod'ed to 0666 (to get past the umask, so another user can take
      the lock too). A file that already existed is never chmod'ed.
    - An existing file is opened O_RDONLY|O_NOFOLLOW|O_NONBLOCK: a symlink
      fails with ELOOP, a FIFO cannot block the open, and fstat then refuses
      anything that is not a regular file. flock needs only an open
      descriptor, whatever its access mode.
    - A hard link to a file of ours cannot be told apart from an ordinary
      lock file, and does not need to be: nothing is ever written to the
      file and its mode is changed only when this process created it, so
      locking through a hard link changes nothing. Do not "fix" this by
      adding a chmod of an existing file -- that is exactly the hole.
    """
    flags = getattr(os, 'O_CLOEXEC', 0) | os.O_NOFOLLOW
    for _ in range(3):
        try:
            fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | flags,
                         0o666)
        except FileExistsError:
            pass
        except OSError as e:
            return None, 'cannot be created (%s)' % e.strerror
        else:
            try:
                os.fchmod(fd, 0o666)
            except OSError:
                pass
            return fd, None
        try:
            fd = os.open(path, os.O_RDONLY | os.O_NONBLOCK | flags)
        except FileNotFoundError:
            # Removed between the two opens: another run's create lost to
            # an unlink. Try creating it again.
            continue
        except OSError as e:
            if e.errno == errno.ELOOP:
                return None, 'is a symbolic link'
            return None, 'cannot be opened (%s)' % e.strerror
        mode = os.fstat(fd).st_mode
        if not stat.S_ISREG(mode):
            os.close(fd)
            return None, 'is not a regular file'
        return fd, None
    return None, 'keeps disappearing between create and open'


def run_darwin(seconds, command):
    import fcntl

    timeout = load_timeout()
    try:
        limit = float(seconds)
    except ValueError:
        return usage()

    path = lock_path()
    budget_text = os.environ.get('HERMES_NODE_UI_DISPLAY_LOCK_WAIT') or ''
    try:
        budget = float(budget_text) if budget_text else LOCK_WAIT_SECONDS
    except ValueError:
        budget = float('nan')
    if not math.isfinite(budget) or budget < 0:
        sys.stderr.write(
            'with-display.py: HERMES_NODE_UI_DISPLAY_LOCK_WAIT must be a '
            'finite, non-negative number of seconds, not %r\n' % budget_text)
        return OWN_FAILURE
    fd, problem = open_lock(path)
    if fd is None:
        # Neither running unserialised nor hanging is acceptable.
        sys.stderr.write(
            'with-display.py: refusing the display lock %s: it %s\n' %
            (path, problem))
        return OWN_FAILURE
    try:
        # Polled rather than blocking, because a blocking flock() cannot be
        # given a deadline. The budget is the queue's, not the test's.
        started = time.monotonic()
        while True:
            try:
                fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
                break
            except BlockingIOError:
                pass
            if time.monotonic() - started >= budget:
                sys.stderr.write(
                    'with-display.py: gave up waiting for the display lock '
                    '%s after %s s\n' % (path, budget_text or
                                         '%g' % LOCK_WAIT_SECONDS))
                return OWN_FAILURE
            time.sleep(0.05)
        # Not on the normal path: only when the UI trace is on, and on this
        # wrapper's own stderr, which no RUN line captures -- every window
        # test redirects the binary's stderr INSIDE `sh -c`, below this
        # process.
        if 'UI' in os.environ.get('HERMES_NODE_DEBUG_NATIVE', ''):
            sys.stderr.write(
                'with-display.py: waited %.1f s for the display lock\n' %
                (time.monotonic() - started))

        env = dict(os.environ)
        env['HERMES_NODE_UI_TEST_NO_ACTIVATE'] = '1'
        env['HERMES_NODE_UI_TEST_PASTEBOARD'] = 'hnui-test-%d%d' % (
            os.getpid(), time.time_ns())
        # subprocess closes inherited descriptors by default, so the lock is
        # held by this process alone and goes when it does.
        # The test's limit starts now, and bounds only the command.
        return timeout.run(limit, command, env=env, foreground=False)
    finally:
        os.close(fd)


def main(argv):
    if argv == ['--print-lock-path']:
        # The path the macOS branch would lock, on any platform. Exists so
        # test/with-display-lock.js can pin the DEFAULT without taking the
        # machine's real lock.
        print(lock_path())
        return 0
    if len(argv) < 2:
        return usage()
    seconds, command = argv[0], argv[1:]
    if sys.platform == 'darwin':
        return run_darwin(seconds, command)
    return run_linux(seconds, command)


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
