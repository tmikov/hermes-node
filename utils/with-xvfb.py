#!/usr/bin/env python3
# Copyright (c) Tzvetan Mikov.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

"""Run one command against a private Xvfb server, then tear the server down.

    with-xvfb.py COMMAND [ARG...]

The replacement for `xvfb-run -a` in the window tests. `xvfb-run -a` picks a
display by looking for a free lock file and only then starts the server on
it, which is not atomic: two of the suite's 16 parallel runs can be handed
the same number, and one of them then fails with
LINUX_X11_OPEN_DISPLAY_FAILED before any of our code runs -- or, worse,
both succeed against ONE server, which corrupts any test about the
clipboard, because a selection belongs to the server.

`Xvfb -displayfd` closes that race: the server itself claims a free display
(socket bind and lock file, both exclusive) and writes the number it got to
the descriptor once it is ready to accept connections. So the number this
reads is one the server already owns, and reading it is also the readiness
signal -- no polling, no SIGUSR1 handshake.

Properties the RUN lines depend on, each kept on purpose:

- **stdout and stderr are the command's own.** Nothing is merged; `xvfb-run`
  merges the command's stderr into stdout (its last line ends `2>&1`), which
  is why the RUN lines put their redirections inside `sh -c '...'`. Those
  lines keep working unchanged here, and a bare command's streams are now
  separate too. The server's own output goes to a log file that is printed
  only if the server fails to start.
- **The exit status is the command's**, 128+N for a command killed by signal
  N, as a shell reports it. A server that does not start exits 125 with a
  message, so it cannot be mistaken for any status a test asserts.
- **No server outlives the run.** `timeout` (without --foreground) signals
  its whole process group, which includes this script and the server; the
  handlers below forward SIGTERM/SIGINT/SIGHUP to the command, wait for it,
  and stop the server on the way out. If this script is killed outright
  (SIGKILL, which no handler sees), the server still goes: it is started
  with PR_SET_PDEATHSIG, so the kernel signals it when its parent dies.
- **Access is limited to this run**, as `xvfb-run` limits it: a fresh
  MIT-MAGIC-COOKIE-1 is written to a private Xauthority file that the
  server is started with and the command is given through XAUTHORITY.

Linux only, like the `display` lit feature that gates every user of this.
"""

import ctypes
import os
import signal
import subprocess
import sys
import tempfile

# Same screen as xvfb-run's default XVFBARGS, so no test sees a different
# root window from the one it was written against.
SERVER_ARGS = ['-screen', '0', '1280x1024x24', '-nolisten', 'tcp']

PR_SET_PDEATHSIG = 1
START_FAILED = 125


def write_xauthority(path, cookie):
    """One wildcard MIT-MAGIC-COOKIE-1 entry, in the Xauthority format.

    Family 0xFFFF (FamilyWild) with an empty address and an empty display
    number: libXau matches an empty number against every display, which is
    what lets the file be written before the server has chosen one. Each
    field is a big-endian 16-bit length followed by that many bytes.
    """
    def field(b):
        return len(b).to_bytes(2, 'big') + b
    entry = (0xFFFF).to_bytes(2, 'big') + field(b'') + field(b'') + \
        field(b'MIT-MAGIC-COOKIE-1') + field(cookie)
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, 'wb') as f:
        f.write(entry)


def die_with_parent():
    # Runs in the server's child between fork and exec.
    libc = ctypes.CDLL(None, use_errno=True)
    libc.prctl(PR_SET_PDEATHSIG, signal.SIGTERM, 0, 0, 0)
    # The parent may have died before the prctl took effect, in which case
    # no signal would ever come; reparenting is how to tell.
    if os.getppid() == 1:
        os._exit(1)


def read_display(fd):
    data = b''
    while not data.endswith(b'\n'):
        chunk = os.read(fd, 64)
        if not chunk:
            return None
        data += chunk
    text = data.strip()
    return text.decode() if text.isdigit() else None


def stop(proc):
    if proc is None or proc.poll() is not None:
        return
    proc.terminate()
    try:
        proc.wait(timeout=10)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()


def main(argv):
    if not argv:
        sys.stderr.write('usage: with-xvfb.py COMMAND [ARG...]\n')
        return 2

    workdir = tempfile.mkdtemp(prefix='with-xvfb.')
    authfile = os.path.join(workdir, 'Xauthority')
    logpath = os.path.join(workdir, 'xvfb.log')
    write_xauthority(authfile, os.urandom(16))

    server = None
    command = None
    caught = []

    def on_signal(signum, _frame):
        # Record and forward. The command is waited for below, so a
        # signal does not abandon it, and the server is stopped in the
        # `finally` whichever way the wait ends.
        caught.append(signum)
        if command is not None and command.poll() is None:
            try:
                command.send_signal(signum)
            except ProcessLookupError:
                pass

    for s in (signal.SIGTERM, signal.SIGINT, signal.SIGHUP):
        signal.signal(s, on_signal)

    try:
        rfd, wfd = os.pipe()
        with open(logpath, 'wb') as log:
            server = subprocess.Popen(
                ['Xvfb', '-displayfd', str(wfd), '-auth', authfile] +
                SERVER_ARGS,
                stdin=subprocess.DEVNULL, stdout=log, stderr=log,
                pass_fds=(wfd,), preexec_fn=die_with_parent)
        os.close(wfd)
        # EOF here means the server exited (or never started) before it
        # was ready; the write end is held by nothing else.
        display = read_display(rfd)
        os.close(rfd)
        if display is None or caught:
            if not caught:
                sys.stderr.write('with-xvfb: Xvfb failed to start\n')
                with open(logpath, 'r', errors='replace') as log:
                    sys.stderr.write(log.read())
            return START_FAILED if not caught else 128 + caught[0]

        env = dict(os.environ)
        env['DISPLAY'] = ':' + display
        env['XAUTHORITY'] = authfile
        command = subprocess.Popen(argv, env=env)
        if caught:
            # A signal that arrived between the server starting and the
            # command existing had nothing to forward to.
            command.send_signal(caught[0])
        while True:
            try:
                status = command.wait()
                break
            except InterruptedError:
                continue
        return 128 - status if status < 0 else status
    except FileNotFoundError as e:
        sys.stderr.write('with-xvfb: %s\n' % e)
        return START_FAILED
    finally:
        stop(server)
        for name in (authfile, logpath):
            try:
                os.unlink(name)
            except OSError:
                pass
        try:
            os.rmdir(workdir)
        except OSError:
            pass


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
