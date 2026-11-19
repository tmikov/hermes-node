# Copyright (c) Tzvetan Mikov.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

"""Do two with-display.py runs serialise on the display lock?

    lock_check.py WITH_DISPLAY LOCK_PATH WORK_DIR

Starts run A (a command that records its start, holds for a while, records
its end), waits until A's command has started, then starts run B (which
records its start). Each run is given a DIFFERENT TMPDIR, as two concurrent
lit invocations get (lit points TMPDIR at a fresh lit_tmp_* directory per
run), so a lock derived from TMPDIR would not serialise them.

HERMES_NODE_UI_DISPLAY_LOCK points both at LOCK_PATH, so this test never
touches the machine's real lock. Prints `serialised` if B's command started
after A's ended, `OVERLAP` otherwise, then a line per run with its status.
Also checks that B, given a limit shorter than A's hold, gives up with 124
rather than waiting past its own limit.
"""

import os
import subprocess
import sys
import time

with_display, lock_path, work = sys.argv[1], sys.argv[2], sys.argv[3]
here = os.path.dirname(os.path.abspath(__file__))
darwin = [sys.executable, os.path.join(here, 'run_as_darwin.py'), with_display]
os.makedirs(work, exist_ok=True)


def env_for(name):
    tmp = os.path.join(work, 'tmpdir-' + name)
    os.makedirs(tmp, exist_ok=True)
    env = dict(os.environ)
    env['TMPDIR'] = tmp
    env['HERMES_NODE_UI_DISPLAY_LOCK'] = lock_path
    return env


def recorder(path, hold):
    return [sys.executable, '-c',
            'import sys, time\n'
            'open(sys.argv[1] + ".start", "w").write(repr(time.time()))\n'
            'time.sleep(float(sys.argv[2]))\n'
            'open(sys.argv[1] + ".end", "w").write(repr(time.time()))\n',
            path, str(hold)]


def read(path):
    with open(path) as f:
        return float(f.read())


def wait_for(path, seconds):
    deadline = time.time() + seconds
    while not os.path.exists(path):
        if time.time() > deadline:
            return False
        time.sleep(0.02)
    return True


a_mark = os.path.join(work, 'A')
b_mark = os.path.join(work, 'B')
c_mark = os.path.join(work, 'C')
for p in (a_mark, b_mark, c_mark):
    for suffix in ('.start', '.end'):
        if os.path.exists(p + suffix):
            os.unlink(p + suffix)

a = subprocess.Popen(darwin + ['30'] + recorder(a_mark, 2.0), env=env_for('A'))
if not wait_for(a_mark + '.start', 20):
    print('A never started')
    sys.exit(1)
b = subprocess.Popen(darwin + ['30'] + recorder(b_mark, 0), env=env_for('B'))
a_status = a.wait()
b_status = b.wait()
print('serialised' if read(b_mark + '.start') >= read(a_mark + '.end')
      else 'OVERLAP')
print('A status=%d' % a_status)
print('B status=%d' % b_status)

# (a) A test limit SHORTER than the wait, a lock-wait budget longer: the
# wait is not charged to the test's limit, so C still runs and succeeds.
# A's marks from the first round are removed first, or the wait below
# would return before the new A holds the lock.
def hold_and_run(name, limit, extra_env):
    for p in (a_mark, c_mark):
        for suffix in ('.start', '.end'):
            if os.path.exists(p + suffix):
                os.unlink(p + suffix)
    holder = subprocess.Popen(darwin + ['30'] + recorder(a_mark, 3.0),
                              env=env_for('A'))
    wait_for(a_mark + '.start', 20)
    env = env_for('C')
    env.update(extra_env)
    started = time.time()
    c = subprocess.run(darwin + [limit] + recorder(c_mark, 0), env=env,
                       stderr=subprocess.PIPE)
    waited = time.time() - started
    holder.wait()
    return c, waited


c, waited = hold_and_run('C', '1', {})
print('C status=%d ran=%d waited-past-limit=%d' % (
    c.returncode, int(os.path.exists(c_mark + '.start')), int(waited > 1.5)))

# (b) A lock-wait budget smaller than the hold: D gives up with 125 -- not
# 124, which would read as the test timing out -- without running, and
# says so.
d, waited = hold_and_run('D', '30', {'HERMES_NODE_UI_DISPLAY_LOCK_WAIT': '1'})
print('D status=%d ran=%d bounded=%d' % (
    d.returncode, int(os.path.exists(c_mark + '.start')), int(waited < 2.5)))
print('D said: ' + d.stderr.decode('utf-8', 'replace').strip())
