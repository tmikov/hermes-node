# Copyright (c) Tzvetan Mikov.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

"""A display lock file unlinked under its holder (macOS's tmp_cleaner).

    cleaner_check.py WITH_DISPLAY WORK_DIR

A acquires the lock and holds it. W starts and is provably waiting on the
SAME (old) file -- it says so on stderr under HERMES_NODE_DEBUG_NATIVE=UI.
The test then unlinks the lock path, as the cleaner would. B starts,
creates a new file and runs: it overlaps A, which is unavoidable once the
file is gone, and is asserted as such. C starts while B holds the new file
and must serialise behind B. And the regression the inode check is for: W,
which held a descriptor on the old file, must NOT run when A releases the
old inode while B holds the new one -- it must reopen and queue behind B.
Finally, a lock file with times 10 days old is refreshed by its holder.
"""

import os
import subprocess
import sys
import time

with_display, work = sys.argv[1], sys.argv[2]
here = os.path.dirname(os.path.abspath(__file__))
darwin = [sys.executable, os.path.join(here, 'run_as_darwin.py'), with_display]
os.makedirs(work, exist_ok=True)
lock = os.path.join(work, 'display.lock')
for name in os.listdir(work):
    os.unlink(os.path.join(work, name))

env = dict(os.environ)
env['HERMES_NODE_UI_DISPLAY_LOCK'] = lock
env['HERMES_NODE_DEBUG_NATIVE'] = 'UI'


def mark(name):
    return os.path.join(work, name)


def holder(name, hold):
    return [sys.executable, '-c',
            'import sys, time\n'
            'open(sys.argv[1] + ".start", "w").write(repr(time.time()))\n'
            'time.sleep(float(sys.argv[2]))\n'
            'open(sys.argv[1] + ".end", "w").write(repr(time.time()))\n',
            mark(name), str(hold)]


def wait_for(path, seconds=30):
    deadline = time.monotonic() + seconds
    while not os.path.exists(path):
        if time.monotonic() > deadline:
            return False
        time.sleep(0.02)
    return True


def read(name):
    with open(mark(name)) as f:
        return float(f.read())


def start(name, hold, stderr=None):
    return subprocess.Popen(darwin + ['60'] + holder(name, hold), env=env,
                            stderr=stderr)


a = start('A', 4)
wait_for(mark('A.start'))
w_err = open(mark('W.err'), 'w')
w = start('W', 0, stderr=w_err)
# W has opened the old file and is polling flock on it.
deadline = time.monotonic() + 30
while 'waiting for the display lock' not in open(mark('W.err')).read():
    if time.monotonic() > deadline:
        break
    time.sleep(0.02)
os.unlink(lock)
b = start('B', 4)
wait_for(mark('B.start'))
c_launched = time.time()
c = start('C', 0)
for p in (a, w, b, c):
    p.wait()
w_err.close()
a_end, b_start, b_end = read('A.end'), read('B.start'), read('B.end')
print('A-B overlap=%d (the unlinked file: unavoidable)' % int(b_start < a_end))
print('W after-B=%d (W status %d)' % (int(read('W.start') >= b_end),
                                      w.returncode))
print('W reopened=%d' % int('was replaced while waiting' in
                            open(mark('W.err')).read()))
print('C after-B=%d launched-while-B=%d (C status %d)' % (
    int(read('C.start') >= b_end), int(c_launched < b_end), c.returncode))

# A lock file 10 days old is refreshed by its holder.
old = time.time() - 10 * 86400
os.utime(lock, (old, old))
subprocess.run(darwin + ['30', 'true'], env=env, stderr=subprocess.DEVNULL)
print('refreshed=%d' % int(os.stat(lock).st_mtime > time.time() - 600))
