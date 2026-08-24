#!/usr/bin/env python3
"""Run a command on a pseudo-terminal and print what it draws.

A TUI refuses to start without a terminal -- tetris calls
stdin.setRawMode() on its fifth line, and blessed needs a window size -- so
checking one from a script means giving it a real pty. `script(1)` would do
it but its arguments differ between Linux and macOS, and these examples run
on both. Python's pty module is the portable option, and python3 is already
required to build this project.

Usage: pty-run.py <seconds> <cols> <rows> [--send KEYS] -- <command>...
Exit status is the command's, or 0 if it was still running at the deadline
(which for a TUI is the normal case).
"""
import fcntl, os, pty, select, signal, struct, sys, termios, time

argv = sys.argv[1:]
# The docstring above is the usage; print it rather than letting the unpack
# below fail with a traceback, which is what a missing argument used to get.
if argv[:1] in (['-h'], ['--help']):
    print(__doc__.strip())
    sys.exit(0)
if len(argv) < 3:
    sys.stderr.write(__doc__.strip() + '\n')
    sys.exit(2)
secs, cols, rows = float(argv[0]), int(argv[1]), int(argv[2])
argv = argv[3:]
send = b''
if argv and argv[0] == '--send':
    send = argv[1].encode().decode('unicode_escape').encode('latin1')
    argv = argv[2:]
if argv and argv[0] == '--':
    argv = argv[1:]

master, slave = pty.openpty()
# Size before exec: blessed reads it at startup and lays out once.
fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack('HHHH', rows, cols, 0, 0))

pid = os.fork()
if pid == 0:
    os.setsid()
    fcntl.ioctl(slave, termios.TIOCSCTTY, 0)
    for fd in (0, 1, 2):
        os.dup2(slave, fd)
    os.close(master); os.close(slave)
    os.environ.setdefault('TERM', 'xterm-256color')
    os.execvp(argv[0], argv)
    os._exit(127)

os.close(slave)
out = bytearray()
deadline = time.time() + secs
sent = False
while time.time() < deadline:
    r, _, _ = select.select([master], [], [], 0.1)
    if r:
        try:
            chunk = os.read(master, 65536)
        except OSError:
            break
        if not chunk:
            break
        out += chunk
    if send and not sent and len(out) > 0:
        os.write(master, send)
        sent = True

status = 0
try:
    os.kill(pid, signal.SIGTERM)
    _, st = os.waitpid(pid, 0)
    if os.WIFEXITED(st):
        status = os.WEXITSTATUS(st)
except OSError:
    pass
sys.stdout.buffer.write(bytes(out))
sys.exit(0 if status in (0, 143) else status)
