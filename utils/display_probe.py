#!/usr/bin/env python3
# Copyright (c) Tzvetan Mikov.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

"""Run one `display` lit-feature probe through utils/with-display.py.

Called from test/lit.cfg; a module of its own so test/display-probe.js can
exercise it on Linux with with-display.py's macOS branch forced.

Two properties the probe must have, both about with-display.py's macOS
branch, where the probe queues on the same machine-wide display lock as
every window test:

1. **It never reports "no display" because the display was busy.** It
   passes its own lock-wait budget (HERMES_NODE_UI_DISPLAY_LOCK_WAIT); a
   wrapper that gave up waiting exits 125 and says so, and that is reported
   as `display busy: another run held the display lock for N s` -- a reason
   lit.cfg turns into a RED test, never into UNSUPPORTED.
2. **The wrapper is never killed from outside while its command may be
   alive.** The outer timeout is derived from the inner ones:

       outer > lock wait + wrapper limit + kill escalation + margin

   so the wrapper always gets to apply its own limit and kill its own
   command's process group. If the outer timeout fires anyway, everything
   in the wrapper's SESSION is killed -- the wrapper is started as a session
   leader, and the command, which with-display.py puts in a process group
   of its own, is still in that session -- so nothing with a window
   survives lit's configuration step.
"""

import importlib.util
import os
import signal
import subprocess

HERE = os.path.dirname(os.path.abspath(__file__))

# How long the probe queues for the display before calling it busy.
LOCK_WAIT_SECONDS = 120
# with-display.py's own limit on the probe command.
LIMIT_SECONDS = 60
# Slack for the wrapper's start-up and teardown.
MARGIN_SECONDS = 15


def _kill_grace_seconds():
    spec = importlib.util.spec_from_file_location(
        'hnui_timeout', os.path.join(HERE, 'timeout.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.KILL_GRACE_SECONDS


def outer_timeout(lock_wait=LOCK_WAIT_SECONDS, limit=LIMIT_SECONDS):
    """The subprocess timeout around the wrapper (see the docstring)."""
    return lock_wait + limit + _kill_grace_seconds() + MARGIN_SECONDS


# The pkill this uses; a module variable so test/display-probe.js can make
# it unavailable.
PKILL = 'pkill'
# How long to wait for the wrapper to be reaped after SIGKILL.
REAP_SECONDS = 10
UNKILLABLE = "the probe's session could not be killed; a window may survive"


def _kill_session(proc):
    """End the wrapper and everything it started; '' or a problem to add
    to the reason.

    (a) SIGTERM to the wrapper alone first. While its command runs,
        with-display.py (timeout.run) forwards SIGTERM to the command's
        process group and waits for it; while it is still queueing for the
        lock there is no command, and SIGTERM ends it. So the ordinary case
        needs no external tool.
    (b) Then, to be sure nothing of the session is left -- a command that
        ignores SIGTERM, a wrapper that did not exit -- `pkill -KILL -s
        <sid>` (the wrapper is the session leader, so its pid is the sid;
        the command, in a process group of its own, is still in the
        session), and killpg of the wrapper's own group. killpg alone
        cannot reach the command's group, so a pkill that could not run
        (missing, or exit 2/3: usage or internal error on procps and BSD
        alike) is REPORTED, never swallowed.
    (c) The final reap is bounded.
    """
    problems = []
    try:
        proc.send_signal(signal.SIGTERM)
    except ProcessLookupError:
        pass
    try:
        proc.wait(timeout=_kill_grace_seconds() + 2)
    except subprocess.TimeoutExpired:
        pass
    pkill_ok = True
    try:
        result = subprocess.run([PKILL, '-KILL', '-s', str(proc.pid)],
                                stdout=subprocess.DEVNULL,
                                stderr=subprocess.DEVNULL, timeout=10)
        if result.returncode not in (0, 1):
            pkill_ok = False
    except (OSError, subprocess.TimeoutExpired):
        pkill_ok = False
    if not pkill_ok:
        problems.append(UNKILLABLE)
    try:
        os.killpg(proc.pid, signal.SIGKILL)
    except (ProcessLookupError, PermissionError):
        pass
    try:
        proc.wait(timeout=REAP_SECONDS)
    except subprocess.TimeoutExpired:
        problems.append('the wrapper did not exit after SIGKILL')
    return '; '.join(problems)


def _wrapper_line(stderr):
    """with-display.py's own last line on stderr, or ''."""
    lines = [l for l in stderr.decode('utf-8', 'replace').splitlines()
             if l.startswith('with-display.py:')]
    return lines[-1] if lines else ''


def run(wrapper, command, env=None, lock_wait=LOCK_WAIT_SECONDS,
        limit=LIMIT_SECONDS, outer=None):
    """Run `wrapper LIMIT command...`.

    Returns (status, stdout, stderr, problem): problem is None when the
    wrapper returned and its status means the command ran to an answer,
    else a sentence for `%display-reason`. `outer` overrides the derived
    outer timeout, for the test of the session kill only.
    """
    env = dict(os.environ if env is None else env)
    env['HERMES_NODE_UI_DISPLAY_LOCK_WAIT'] = '%g' % lock_wait
    if outer is None:
        outer = outer_timeout(lock_wait, limit)
    try:
        proc = subprocess.Popen(
            list(wrapper) + [str(limit)] + list(command), env=env,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            start_new_session=True)
    except OSError as e:
        return None, b'', b'', 'display probe could not start (%s)' % e
    try:
        out, err = proc.communicate(timeout=outer)
    except subprocess.TimeoutExpired:
        problem = _kill_session(proc)
        reason = ('display probe killed after %g s; the wrapper never '
                  'returned' % outer)
        if problem:
            reason += '; ' + problem
        return None, b'', b'', reason
    status = proc.returncode
    if status == 125:
        line = _wrapper_line(err)
        if 'gave up waiting for the display lock' in line:
            return status, out, err, (
                'display busy: another run held the display lock for %g s' %
                lock_wait)
        return status, out, err, (
            'display probe wrapper failed: ' + (line or 'status 125'))
    if status == 124:
        return status, out, err, (
            'display probe timed out after %g s' % limit)
    return status, out, err, None


# What a reason may contain once it is a lit substitution. The two tests
# that print %display-reason do so inside single quotes on a shell line, and
# lit itself expands `%` sequences in RUN lines, so anything outside this
# conservative set -- quotes, backslash, `$`, backquote, `%`, newline -- is
# replaced by `?`: visibly sanitised, never executed.
_REASON_SAFE = set('abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ'
                   '0123456789 _.,:;()/=+-')


def sanitize_reason(reason):
    return ''.join(c if c in _REASON_SAFE else '?' for c in reason)


def is_busy(reason):
    return reason.startswith('display busy:')


def window_probe(wrapper, binary, script, env=None, **kw):
    """(ok, reason): the macOS window probe -- a window opened, was visible,
    and yielded a drawable."""
    env = dict(os.environ if env is None else env)
    env['HERMES_NODE_UI_TEST_HOOKS'] = '1'
    env['HERMES_NODE_DEBUG_NATIVE'] = 'UI'
    status, out, err, problem = run(wrapper, [binary, script], env, **kw)
    if problem:
        return False, problem
    if status != 0:
        return False, 'window probe exited with status %d' % status
    if b'DISPLAY_PROBE_OK' not in out:
        return False, 'window probe printed no sentinel'
    if b'present: drawable acquired' not in err:
        return False, 'window probe acquired no drawable'
    return True, 'a window was drawn'


def xvfb_probe(wrapper, env=None, **kw):
    """(ok, reason): the Linux probe -- an Xvfb started and DISPLAY reached
    the command. The sentinel is assembled by the child shell, so nothing
    that merely echoes its argv can satisfy it."""
    status, out, _, problem = run(
        wrapper, ['sh', '-c', 'test -n "$DISPLAY" && echo "XVFB_""OK"'],
        env, **kw)
    if problem:
        return False, problem
    if status == 0 and b'XVFB_OK' in out:
        return True, 'Xvfb started'
    return False, 'Xvfb could not be started (status %d)' % status
