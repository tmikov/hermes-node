#!/usr/bin/env python3
# Copyright (c) Tzvetan Mikov.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

"""Run a command with a time limit, like GNU `timeout`.

    timeout.py [--foreground] SECONDS COMMAND [ARG...]

What `%timeout` expands to on macOS (test/lit.cfg), where `/usr/bin/timeout`
exists only on recent releases, so that the suite does not depend on which
one the machine has. On Linux `%timeout` is GNU `timeout` itself; this file
implements the subset of it the RUN lines use, with the same answers:

- **Exit 124 when the limit expired**, whatever the command then did. Any
  other status is the command's own, 128+N for a command killed by signal N,
  as a shell reports it. As GNU `timeout`: 127 when the command does not
  exist, 126 when it exists and cannot be executed, 125 for this script's
  own failure (bad arguments, any other start-up error).
- **Without --foreground the command runs in a new process group, and
  expiry signals the whole group**, so a shell's children go with it, as
  they do under GNU `timeout`. SIGTERM first; SIGKILL if the group is still
  there after a short grace, so a command that ignores SIGTERM cannot
  outlive its limit.
- **With --foreground neither happens**: the command stays in the caller's
  process group -- so it can read the terminal it was started on, which is
  what test/ui-exit-tty.js needs under its pty -- and expiry signals the
  command alone.
- SIGTERM, SIGINT and SIGHUP sent to this script are forwarded to the
  command (to its group without --foreground), and this script then waits
  for it, so nothing is abandoned.

`run()` is also what utils/with-display.py uses on macOS.
"""

import os
import signal
import subprocess
import sys

TIMED_OUT = 124
OWN_FAILURE = 125
CANNOT_EXECUTE = 126
NOT_FOUND = 127

# How long a command gets between SIGTERM and SIGKILL after expiry.
KILL_GRACE_SECONDS = 5.0


def _signal(proc, own_group, signum):
    try:
        if own_group:
            os.killpg(proc.pid, signum)
        else:
            proc.send_signal(signum)
    except (ProcessLookupError, PermissionError):
        pass


def _status(returncode):
    return 128 - returncode if returncode < 0 else returncode


def run(seconds, command, env=None, foreground=False):
    """Run `command` for at most `seconds`; return the status as above."""
    own_group = not foreground
    try:
        proc = subprocess.Popen(
            command,
            env=env,
            preexec_fn=os.setpgrp if own_group else None)
    except FileNotFoundError as e:
        sys.stderr.write('timeout.py: %s: %s\n' % (command[0], e))
        return NOT_FOUND
    except PermissionError as e:
        sys.stderr.write('timeout.py: %s: %s\n' % (command[0], e))
        return CANNOT_EXECUTE
    except OSError as e:
        sys.stderr.write('timeout.py: %s: %s\n' % (command[0], e))
        return OWN_FAILURE

    def forward(signum, _frame):
        _signal(proc, own_group, signum)

    previous = {}
    for s in (signal.SIGTERM, signal.SIGINT, signal.SIGHUP):
        previous[s] = signal.signal(s, forward)
    try:
        try:
            return _status(proc.wait(timeout=seconds))
        except subprocess.TimeoutExpired:
            pass
        _signal(proc, own_group, signal.SIGTERM)
        try:
            proc.wait(timeout=KILL_GRACE_SECONDS)
        except subprocess.TimeoutExpired:
            _signal(proc, own_group, signal.SIGKILL)
            proc.wait()
        if own_group:
            # The command may have gone at SIGTERM while a child of its
            # ignored it; the group is what the limit applies to.
            _signal(proc, True, signal.SIGKILL)
        return TIMED_OUT
    finally:
        for s, handler in previous.items():
            signal.signal(s, handler)


def parse(argv):
    """[--foreground] SECONDS COMMAND... -> (foreground, seconds, command)."""
    foreground = False
    if argv and argv[0] == '--foreground':
        foreground = True
        argv = argv[1:]
    if len(argv) < 2:
        return None
    try:
        seconds = float(argv[0])
    except ValueError:
        return None
    if seconds <= 0:
        return None
    return foreground, seconds, argv[1:]


def main(argv):
    parsed = parse(argv)
    if parsed is None:
        sys.stderr.write(
            'usage: timeout.py [--foreground] SECONDS COMMAND [ARG...]\n')
        return OWN_FAILURE
    foreground, seconds, command = parsed
    return run(seconds, command, foreground=foreground)


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
