# Copyright (c) Tzvetan Mikov.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

"""utils/display_probe.py, with with-display.py's macOS branch forced.

    probe_check.py SOURCE_DIR WORK_DIR

Each case runs in a subprocess of its own (this script, given the case's
name), with its own lock path, so the cases cannot interact; they run
concurrently, and their lines are printed in a fixed order. Most of each
case is waiting out a timeout, so running them one after another cost
the sum of those waits.

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
case = sys.argv[3] if len(sys.argv) > 3 else None
os.makedirs(work, exist_ok=True)
here = os.path.dirname(os.path.abspath(__file__))
with_display = os.path.join(source_dir, 'utils', 'with-display.py')
wrapper = [sys.executable, os.path.join(here, 'run_as_darwin.py'),
           with_display]
spec = importlib.util.spec_from_file_location(
    'display_probe', os.path.join(source_dir, 'utils', 'display_probe.py'))
probe = importlib.util.module_from_spec(spec)
spec.loader.exec_module(probe)

# Test-only: the probe's wait for the wrapper after its SIGTERM (default
# KILL_GRACE_SECONDS + 2 = 7 s). Every case here ends that wait on an
# EVENT -- the wrapper exiting once its command is gone -- or, where the
# command ignores SIGTERM, has nothing to gain from waiting at all; so the
# shorter wait changes no outcome, only how long the ignoring cases idle.
probe.TERM_WAIT_SECONDS = 1

# How long a command that the probe has finished with must stay alive to
# count as a survivor. The direction that matters -- a killed process
# reported gone -- returns as soon as it is gone; only a true survivor
# pays the full window.
SURVIVOR_WINDOW = 2


def env_for(name):
    env = dict(os.environ)
    env['HERMES_NODE_UI_DISPLAY_LOCK'] = os.path.join(work, name + '.lock')
    return env


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


def ignores_term(path):
    return [sys.executable, '-c',
            'import os, signal, sys, time\n'
            'signal.signal(signal.SIGTERM, signal.SIG_IGN)\n'
            'open(sys.argv[1], "w").write(str(os.getpid()))\n'
            'time.sleep(300)\n', path]


def case_defaults():
    print('outer for defaults: %g (lock wait %g + limit %g + grace %g + '
          'margin %g)' % (probe.outer_timeout(), probe.LOCK_WAIT_SECONDS,
                          probe.LIMIT_SECONDS, probe._kill_grace_seconds(),
                          probe.MARGIN_SECONDS))


def case_busy():
    # (1) A lock held longer than the probe's lock-wait budget.
    env = env_for('busy')
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
        status, int(os.path.exists(busy_pid)),
        int(probe.is_busy(reason or ''))))
    print('busy reason: %s' % reason)


def case_outer():
    # (2) The outer timeout fires: nothing survives.
    killed_pid = os.path.join(work, 'killed.pid')
    status, _, _, reason = probe.run(wrapper, pid_writer(killed_pid),
                                     env_for('outer'), lock_wait=5,
                                     limit=60, outer=3)
    pid = int(open(killed_pid).read()) if os.path.exists(killed_pid) else 0
    survived = pid != 0 and not gone_within(pid, SURVIVOR_WINDOW)
    if survived:
        os.kill(pid, 9)
    print('outer status=%s command-started=%d command-survived=%d' % (
        status, int(pid != 0), int(survived)))
    print('outer reason: %s' % reason)


def outer_case(name, command_for, ps):
    """The outer timeout fires; report what survived and the reason."""
    probe.PS = ps
    mark = os.path.join(work, name + '.pid')
    _, _, _, reason = probe.run(wrapper, command_for(mark), env_for(name),
                                lock_wait=5, limit=60, outer=3)
    pid = int(open(mark).read()) if os.path.exists(mark) else 0
    survived = pid != 0 and not gone_within(pid, SURVIVOR_WINDOW)
    if survived:
        os.kill(pid, 9)
    print('%s command-started=%d command-survived=%d unkillable=%d' % (
        name, int(pid != 0), int(survived),
        int(probe.UNKILLABLE in (reason or ''))))


CASES = [
    ('defaults', case_defaults),
    ('busy', case_busy),
    ('outer', case_outer),
    # (3) A command that dies on SIGTERM is gone even when the session
    # cannot be enumerated (ps unavailable): the probe's SIGTERM to the
    # wrapper is forwarded to the command's group. The missing ps is still
    # reported.
    ('term-nops', lambda: outer_case('term-nops', pid_writer,
                                     '/nonexistent/ps')),
    # (4) A command that ignores SIGTERM, in a process group of its own,
    # needs the session enumeration: gone with it, and without it (ps
    # unavailable) it survives and the reason says the session could not
    # be killed.
    ('ignore-ps', lambda: outer_case('ignore-ps', ignores_term, 'ps')),
    ('ignore-nops', lambda: outer_case('ignore-nops', ignores_term,
                                       '/nonexistent/ps')),
]

if case is not None:
    dict(CASES)[case]()
    sys.exit(0)

# All cases at once, each its own process; output in CASES order.
children = [(name, subprocess.Popen(
    [sys.executable, os.path.abspath(__file__), source_dir, work, name],
    stdout=subprocess.PIPE)) for name, _ in CASES]
for name, child in children:
    out, _ = child.communicate()
    sys.stdout.write(out.decode('utf-8', 'replace'))
    if child.returncode != 0:
        print('case %s exited with status %d' % (name, child.returncode))
