# Copyright (c) Tzvetan Mikov.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

"""utils/display_probe.py, with with-display.py's macOS branch forced.

    probe_check.py SOURCE_DIR WORK_DIR

(1) A lock held longer than the probe's lock-wait budget: the probe says
    "display busy", and its command never started.
(2) The outer timeout forced below the wrapper's own (the one case it can
    fire): the probe kills the wrapper's whole session, so the command --
    in a process group of its own, as with-display.py makes it -- does not
    survive.
Also prints the derived outer timeout for the default budgets.
"""

import importlib.util
import os
import subprocess
import sys
import time

source_dir, work = sys.argv[1], sys.argv[2]
os.makedirs(work, exist_ok=True)
here = os.path.dirname(os.path.abspath(__file__))
with_display = os.path.join(source_dir, 'utils', 'with-display.py')
wrapper = [sys.executable, os.path.join(here, 'run_as_darwin.py'),
           with_display]
spec = importlib.util.spec_from_file_location(
    'display_probe', os.path.join(source_dir, 'utils', 'display_probe.py'))
probe = importlib.util.module_from_spec(spec)
spec.loader.exec_module(probe)

lock = os.path.join(work, 'display.lock')
env = dict(os.environ)
env['HERMES_NODE_UI_DISPLAY_LOCK'] = lock


def alive(pid):
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    return True


def gone_within(pid, seconds):
    deadline = time.monotonic() + seconds
    while alive(pid):
        if time.monotonic() > deadline:
            return False
        time.sleep(0.05)
    return True


def pid_writer(path):
    # Writes its pid, then sleeps far longer than any limit here.
    return [sys.executable, '-c',
            'import os, sys, time\n'
            'open(sys.argv[1], "w").write(str(os.getpid()))\n'
            'time.sleep(300)\n', path]


print('outer for defaults: %g (lock wait %g + limit %g + grace %g + margin %g)'
      % (probe.outer_timeout(), probe.LOCK_WAIT_SECONDS, probe.LIMIT_SECONDS,
         probe._kill_grace_seconds(), probe.MARGIN_SECONDS))

# (1) Busy.
started_mark = os.path.join(work, 'holder.started')
holder = subprocess.Popen(wrapper + ['30', sys.executable, '-c',
                                     'import sys, time\n'
                                     'open(sys.argv[1], "w").close()\n'
                                     'time.sleep(4)\n', started_mark],
                          env=env)
deadline = time.monotonic() + 20
while not os.path.exists(started_mark) and time.monotonic() < deadline:
    time.sleep(0.02)
busy_pid = os.path.join(work, 'busy.pid')
status, _, _, reason = probe.run(wrapper, pid_writer(busy_pid), env,
                                 lock_wait=1, limit=10)
holder.wait()
print('busy status=%s started=%d is_busy=%d' % (
    status, int(os.path.exists(busy_pid)), int(probe.is_busy(reason or ''))))
print('busy reason: %s' % reason)

# (2) The outer timeout fires: nothing survives.
killed_pid = os.path.join(work, 'killed.pid')
status, _, _, reason = probe.run(wrapper, pid_writer(killed_pid), env,
                                 lock_wait=5, limit=60, outer=3)
pid = int(open(killed_pid).read()) if os.path.exists(killed_pid) else 0
survived = pid != 0 and not gone_within(pid, 5)
if survived:
    os.kill(pid, 9)
print('outer status=%s command-started=%d command-survived=%d' % (
    status, int(pid != 0), int(survived)))
print('outer reason: %s' % reason)


def ignores_term(path):
    return [sys.executable, '-c',
            'import os, signal, sys, time\n'
            'signal.signal(signal.SIGTERM, signal.SIG_IGN)\n'
            'open(sys.argv[1], "w").write(str(os.getpid()))\n'
            'time.sleep(300)\n', path]


def outer_case(name, command_for, pkill):
    """The outer timeout fires; report what survived and the reason."""
    probe.PKILL = pkill
    mark = os.path.join(work, name + '.pid')
    _, _, _, reason = probe.run(wrapper, command_for(mark), env,
                                lock_wait=5, limit=60, outer=3)
    pid = int(open(mark).read()) if os.path.exists(mark) else 0
    survived = pid != 0 and not gone_within(pid, 5)
    if survived:
        os.kill(pid, 9)
    probe.PKILL = 'pkill'
    print('%s command-started=%d command-survived=%d unkillable=%d' % (
        name, int(pid != 0), int(survived),
        int(probe.UNKILLABLE in (reason or ''))))


# (3) A command that dies on SIGTERM is gone even with no pkill at all: the
# probe's SIGTERM to the wrapper is forwarded to the command's group. The
# missing pkill is still reported.
outer_case('term-nopkill', pid_writer, '/nonexistent/pkill')
# (4) A command that ignores SIGTERM needs pkill: gone with it, and without
# it the reason says the session could not be killed.
outer_case('ignore-pkill', ignores_term, 'pkill')
outer_case('ignore-nopkill', ignores_term, '/nonexistent/pkill')
