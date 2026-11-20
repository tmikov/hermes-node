# Copyright (c) Tzvetan Mikov.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

"""Do two with-display.py runs serialise on the display lock?

    lock_check.py WITH_DISPLAY LOCK_PATH WORK_DIR

Starts run A (a command that records its start, holds for a while, records
its end), waits until A's command has started -- so A holds the lock --
then starts run B (which records its start). Serialised means B was
launched before A's command ended and B's command started after it. Each run is given a DIFFERENT TMPDIR, as two concurrent
lit invocations get (lit points TMPDIR at a fresh lit_tmp_* directory per
run), so a lock derived from TMPDIR would not serialise them.

HERMES_NODE_UI_DISPLAY_LOCK points both at LOCK_PATH, so this test never
touches the machine's real lock.

The three rounds -- serialise, C, D -- are independent, so each runs in a
subprocess of its own (this script, given the round's name) with its own
lock file (LOCK_PATH's name, in a directory per round) and its own marks,
all at once; their lines are printed in a fixed order. Prints `serialised` if B's command started
after A's ended, `OVERLAP` otherwise, then a line per run with its status.
Then two cases of the wait's own budget: C, whose TEST limit is shorter
than A's hold, still runs and succeeds (the limit starts once the lock is
held); D, whose lock-wait budget (HERMES_NODE_UI_DISPLAY_LOCK_WAIT) is
shorter than the hold, exits 125 without running its command.
"""

import os
import subprocess
import sys
import time

with_display, lock_path, work = sys.argv[1], sys.argv[2], sys.argv[3]
round_name = sys.argv[4] if len(sys.argv) > 4 else None
here = os.path.dirname(os.path.abspath(__file__))
darwin = [sys.executable, os.path.join(here, 'run_as_darwin.py'), with_display]
ROUNDS = ['serialise', 'C', 'D']

if round_name is None:
    children = [(r, subprocess.Popen(
        [sys.executable, os.path.abspath(__file__), with_display, lock_path,
         work, r], stdout=subprocess.PIPE)) for r in ROUNDS]
    for r, child in children:
        out, _ = child.communicate()
        sys.stdout.write(out.decode('utf-8', 'replace'))
        if child.returncode != 0:
            print('round %s exited with status %d' % (r, child.returncode))
    sys.exit(0)

work = os.path.join(work, round_name)
lock_path = os.path.join(os.path.dirname(lock_path), round_name,
                         os.path.basename(lock_path))
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


def round_serialise():
    a = subprocess.Popen(darwin + ['30'] + recorder(a_mark, 2.0),
                         env=env_for('A'))
    if not wait_for(a_mark + '.start', 20):
        print('A never started')
        sys.exit(1)
    # B is launched only once A's COMMAND has started, so A provably holds
    # the lock. Serialised means B was launched while A still held it AND
    # B's command started only after A's ended: a B that merely started
    # late would satisfy the second half alone.
    b_launched = time.time()
    b = subprocess.Popen(darwin + ['30'] + recorder(b_mark, 0),
                         env=env_for('B'))
    a_status = a.wait()
    b_status = b.wait()
    a_end = read(a_mark + '.end')
    if not b_launched < a_end:
        print('B launched too late to prove anything')
    elif read(b_mark + '.start') >= a_end:
        print('serialised')
    else:
        print('OVERLAP')
    print('A status=%d' % a_status)
    print('B status=%d' % b_status)


def hold_and_run(limit, extra_env):
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


def round_c():
    # (a) A test limit SHORTER than the wait, a lock-wait budget longer: the
    # wait is not charged to the test's limit, so C still runs and succeeds.
    c, waited = hold_and_run('1', {})
    print('C status=%d ran=%d waited-past-limit=%d' % (
        c.returncode, int(os.path.exists(c_mark + '.start')),
        int(waited > 1.5)))


def round_d():
    # (b) A lock-wait budget smaller than the hold: D gives up with 125 --
    # not 124, which would read as the test timing out -- without running,
    # and says so.
    d, waited = hold_and_run('30', {'HERMES_NODE_UI_DISPLAY_LOCK_WAIT': '1'})
    print('D status=%d ran=%d bounded=%d' % (
        d.returncode, int(os.path.exists(c_mark + '.start')),
        int(waited < 2.5)))
    print('D said: ' + d.stderr.decode('utf-8', 'replace').strip())


{'serialise': round_serialise, 'C': round_c, 'D': round_d}[round_name]()
