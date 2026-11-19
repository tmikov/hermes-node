# Copyright (c) Tzvetan Mikov.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

"""Exercise utils/timeout.py and print one line per property.

    check_timeout.py TIMEOUT_PY WORK_DIR

Each case runs timeout.py in a subprocess with this interpreter, so what is
measured is the script's exit status and its effect on processes, the way a
RUN line sees them. POSIX shell and sleep only, so it runs on macOS's BSD
userland as well as on Linux.
"""

import os
import subprocess
import sys
import time

timeout_py, work = sys.argv[1], sys.argv[2]
os.makedirs(work, exist_ok=True)


def run(args):
    started = time.monotonic()
    proc = subprocess.run([sys.executable, timeout_py] + args,
                          stderr=subprocess.DEVNULL)
    return proc.returncode, time.monotonic() - started


def alive(pid):
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    # A zombie still answers kill(0); give init a moment to reap it.
    return True


def gone_within(pid, seconds):
    deadline = time.monotonic() + seconds
    while alive(pid):
        if time.monotonic() > deadline:
            return False
        time.sleep(0.05)
    return True


status, elapsed = run(['1', 'sleep', '30'])
print('expiry status=%d bounded=%d' % (status, int(elapsed < 10)))

status, _ = run(['10', 'sh', '-c', 'exit 7'])
print('exit status=%d' % status)

status, _ = run(['10', 'sh', '-c', 'kill -ABRT $$'])
print('signal status=%d' % status)

# A child that ignores SIGTERM: killed by the escalation, after the grace.
# The loop ends by itself after 30 s, so a timeout.py that never escalated
# still finishes -- and fails the bound -- rather than hanging the suite.
status, elapsed = run(['1', 'sh', '-c',
                       'trap "" TERM; i=0; while [ $i -lt 30 ]; do '
                       'sleep 1; i=$((i+1)); done'])
print('ignores-term status=%d escalated=%d bounded=%d' % (
    status, int(elapsed >= 5), int(elapsed < 20)))

# The whole process group goes, a grandchild included.
pidfile = os.path.join(work, 'grandchild.pid')
if os.path.exists(pidfile):
    os.unlink(pidfile)
status, _ = run(['1', 'sh', '-c',
                 'sleep 300 & echo $! > "$0"; wait', pidfile])
with open(pidfile) as f:
    grandchild = int(f.read())
print('group status=%d grandchild-gone=%d' % (
    status, int(gone_within(grandchild, 5))))
if alive(grandchild):
    os.kill(grandchild, 9)

status, _ = run(['5', os.path.join(work, 'no-such-command')])
print('missing status=%d' % status)
