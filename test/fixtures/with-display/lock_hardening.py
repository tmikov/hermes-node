# Copyright (c) Tzvetan Mikov.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

"""utils/with-display.py's lock file cannot be turned against its user.

    lock_hardening.py WITH_DISPLAY WORK_DIR

The default lock path is in world-writable /tmp, where another local user
can plant whatever they like. Each case points HERMES_NODE_UI_DISPLAY_LOCK
at what such a user could plant, runs the wrapper with the darwin branch
forced, and prints one line: the wrapper's status, whether its command ran,
and what happened to the planted thing. Then the budget values that must be
refused, and the default path, which no override may stand in for.
"""

import os
import stat
import subprocess
import sys
import time

with_display, work = sys.argv[1], sys.argv[2]
here = os.path.dirname(os.path.abspath(__file__))
darwin = [sys.executable, os.path.join(here, 'run_as_darwin.py'), with_display]
os.makedirs(work, exist_ok=True)


def fresh(name):
    path = os.path.join(work, name)
    if os.path.lexists(path):
        os.unlink(path)
    return path


def wrap(lock, limit='10', command=None, env_extra=None, umask=None,
         timeout=20):
    """(status, ran, stderr) of one wrapper run; status None if it hung."""
    marker = fresh('ran-marker')
    env = dict(os.environ)
    env['HERMES_NODE_UI_DISPLAY_LOCK'] = lock
    env.update(env_extra or {})
    cmd = command or [sys.executable, '-c',
                      'import sys; open(sys.argv[1], "w").close()', marker]
    preexec = (lambda: os.umask(umask)) if umask is not None else None
    proc = subprocess.Popen(darwin + [limit] + cmd, env=env,
                            stderr=subprocess.PIPE, preexec_fn=preexec)
    try:
        _, err = proc.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.communicate()
        return None, os.path.exists(marker), ''
    return (proc.returncode, os.path.exists(marker),
            err.decode('utf-8', 'replace').strip())


def mode(path):
    return '%o' % stat.S_IMODE(os.stat(path).st_mode)


# (1) A symlink to a file of ours: not followed, the file's mode untouched.
target = fresh('victim')
with open(target, 'w') as f:
    f.write('mine')
os.chmod(target, 0o600)
link = fresh('link-to-victim')
os.symlink(target, link)
status, ran, err = wrap(link)
print('symlink status=%s ran=%d target-mode=%s' % (status, ran, mode(target)))
print('symlink said: ' + err)

# (2) A symlink to nothing: refused, and nothing created through it.
missing = fresh('missing-target')
dangling = fresh('dangling-link')
os.symlink(missing, dangling)
status, ran, err = wrap(dangling)
print('dangling status=%s ran=%d target-created=%d' % (
    status, ran, int(os.path.lexists(missing))))

# (3) A FIFO: refused promptly rather than blocking the open forever.
fifo = fresh('fifo')
os.mkfifo(fifo)
started = time.monotonic()
status, ran, err = wrap(fifo, timeout=10)
print('fifo status=%s ran=%d prompt=%d' % (
    status, ran, int(time.monotonic() - started < 5)))
print('fifo said: ' + err)

# (4) An existing lock file we cannot write: still serialises, through the
# read-only open. (The test cannot make it someone else's; 0444 is the part
# of that it can reproduce.)
readonly = fresh('readonly.lock')
open(readonly, 'w').close()
os.chmod(readonly, 0o444)
a_mark = fresh('A.start'), fresh('A.end')
hold = [sys.executable, '-c',
        'import sys, time\n'
        'open(sys.argv[1], "w").write(repr(time.time()))\n'
        'time.sleep(1.5)\n'
        'open(sys.argv[2], "w").write(repr(time.time()))\n'] + list(a_mark)
env = dict(os.environ)
env['HERMES_NODE_UI_DISPLAY_LOCK'] = readonly
holder = subprocess.Popen(darwin + ['30'] + hold, env=env)
deadline = time.monotonic() + 20
while not os.path.exists(a_mark[0]) and time.monotonic() < deadline:
    time.sleep(0.02)
b_mark = fresh('B.start')
# Launched only once A's command has started (A holds the lock); proves
# the wait only if it was also launched before A's command ended.
b_launched = time.time()
status, _, _ = wrap(readonly, command=[
    sys.executable, '-c',
    'import sys, time; open(sys.argv[1], "w").write(repr(time.time()))',
    b_mark])
holder.wait()
with open(b_mark) as f:
    b_start = float(f.read())
with open(a_mark[1]) as f:
    a_end = float(f.read())
print('readonly status=%s serialised=%d mode=%s' % (
    status, int(b_launched < a_end <= b_start), mode(readonly)))

# (5) A file the wrapper creates is 0666 whatever the umask.
for umask in (0o022, 0o077):
    created = fresh('created-%03o.lock' % umask)
    status, ran, _ = wrap(created, umask=umask)
    print('umask %03o status=%s ran=%d mode=%s' % (
        umask, status, ran, mode(created)))

# Lock-wait budgets that are not a finite, non-negative number of seconds.
plain = fresh('plain.lock')
for value in ('abc', 'nan', 'inf', '-1'):
    status, ran, err = wrap(plain, env_extra={
        'HERMES_NODE_UI_DISPLAY_LOCK_WAIT': value})
    print('budget %s status=%s ran=%d said: %s' % (value, status, ran, err))

# The DEFAULT path, with no override, whatever TMPDIR says.
for tmp in ('one', 'two'):
    env = {k: v for k, v in os.environ.items()
           if k != 'HERMES_NODE_UI_DISPLAY_LOCK'}
    env['TMPDIR'] = os.path.join(work, 'tmp-' + tmp)
    out = subprocess.run(darwin + ['--print-lock-path'], env=env,
                         stdout=subprocess.PIPE).stdout.decode().strip()
    print('default (TMPDIR=%s): %s' % (tmp, out))
