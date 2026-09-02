#!/usr/bin/env python3
# Copyright (c) Tzvetan Mikov.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

"""Run a script on a pseudo-terminal and report whether it restored the tty.

A program that calls stdin.setRawMode(true) and then exits must leave the
terminal as it found it, or the shell that spawned it stops echoing what the
user types. Checking that needs a real terminal -- setRawMode does not exist
on a pipe -- so this gives the program a pty, waits for it to exit, and then
reads the pty's termios flags back.

The read happens in the child, not the parent, and that is not incidental: on
macOS the slave fd is revoked once the session leader exits, so a parent
holding it gets ENOTTY. The child is the session leader and is still alive
after the program it spawned has exited, so fd 0 is still its controlling
terminal there. The verdict travels back over a pipe of its own, which also
keeps it out of whatever the program drew on the terminal.

ECHO and ICANON are read through Python's termios constants rather than by
parsing stty(1): the bit values differ between platforms (ICANON is 0x100 on
macOS and 0x2 on Linux) and stty's own output format differs with them.

Usage: run-on-pty.py <program> [args...]
Prints one RESULT: line to stdout; the program's terminal output goes to
stderr.
"""
import os
import pty
import select
import struct
import subprocess
import sys
import termios
import fcntl
import time

if len(sys.argv) < 2:
    sys.stderr.write(__doc__.strip() + '\n')
    sys.exit(2)

argv = sys.argv[1:]

master, slave = pty.openpty()
fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack('HHHH', 30, 100, 0, 0))

# The child reports through this, so the verdict cannot be confused with
# anything the program drew on the terminal.
report_r, report_w = os.pipe()


def flags(fd):
    lflag = termios.tcgetattr(fd)[3]
    return (bool(lflag & termios.ECHO), bool(lflag & termios.ICANON))


pid = os.fork()
if pid == 0:
    os.close(report_r)
    os.setsid()
    fcntl.ioctl(slave, termios.TIOCSCTTY, 0)
    for fd in (0, 1, 2):
        os.dup2(slave, fd)
    os.close(master)
    os.close(slave)
    os.environ.setdefault('TERM', 'xterm-256color')
    before = flags(0)
    try:
        status = subprocess.call(argv)
    except OSError as exc:
        os.write(report_w, ('SPAWN-FAILED %s\n' % exc).encode())
        os._exit(127)
    after = flags(0)
    os.write(report_w, ('%d %d %d %d %d\n' % (
        status, before[0], before[1], after[0], after[1])).encode())
    os._exit(0)

os.close(slave)
os.close(report_w)

# Drain the terminal while waiting, or a program that writes more than a pipe
# buffer blocks forever.
out = bytearray()
deadline = time.time() + 30
report = b''
while time.time() < deadline:
    readable, _, _ = select.select([master, report_r], [], [], 0.1)
    if master in readable:
        try:
            chunk = os.read(master, 65536)
        except OSError:
            chunk = b''
        if chunk:
            out += chunk
    if report_r in readable:
        chunk = os.read(report_r, 4096)
        if not chunk:
            break
        report += chunk
        break

try:
    os.waitpid(pid, 0)
except OSError:
    pass

sys.stderr.buffer.write(bytes(out))
sys.stderr.flush()

fields = report.decode(errors='replace').split()
if len(fields) != 5:
    print('RESULT: HARNESS-FAILED %s' % (report.decode(errors='replace').strip()
                                         or 'no report from child'))
    sys.exit(2)

status, be, bc, ae, ac = (int(x) for x in fields)
print('exit status: %d' % status)
print('before: ECHO=%s ICANON=%s' % ('on' if be else 'off', 'on' if bc else 'off'))
print('after:  ECHO=%s ICANON=%s' % ('on' if ae else 'off', 'on' if ac else 'off'))
if (be, bc) == (ae, ac):
    print('RESULT: RESTORED')
else:
    print('RESULT: LEFT-IN-RAW-MODE')
