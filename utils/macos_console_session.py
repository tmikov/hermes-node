#!/usr/bin/env python3
# Copyright (c) Tzvetan Mikov.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

"""Is the macOS console session usable for a window test?

    macos_console_session.py            query this machine, print the verdict
    macos_console_session.py --self-test  classify built-in samples

Stage 1 of the `display` lit feature on macOS (test/lit.cfg). A `sokol_app`
Metal program gets no frame at all while the console session is locked or
the display is asleep: Sokol starts on a CADisplayLink, which does not fire
then, and moves to its obscured timer only on an occlusion CHANGE, which a
window that was never visible never has. So `init_cb` never runs, and a
probe that runs the binary and waits for a trace line can only reach
"false" through its full timeout, on every lit invocation, without saying
why. This asks first, cheaply and without opening a window.

The source is `ioreg -n Root -d1 -a`, parsed with plistlib: python3 only, no
PyObjC, no third-party module. The first entry of `IOConsoleUsers` carries
`CGSSessionScreenIsLocked` (present only while locked, so an ABSENT key
means not locked) and `kCGSSessionOnConsoleKey`. Anything that cannot be
read -- ioreg missing, exiting non-zero, output that is not a plist or has
no `IOConsoleUsers` -- is "not usable", with a reason, never an exception.

UNVERIFIED on the machine this was written on (Linux): the exact shape of
real `ioreg -a` output. `classify()` accepts the console-user list either at
the top level of the root dictionary or under `IORegistryEntryChildren`, and
a top-level array of dictionaries, which covers the shapes `ioreg -a` is
known to produce; the Mac round confirms it.
"""

import plistlib
import subprocess
import sys

IOREG = ['ioreg', '-n', 'Root', '-d1', '-a']


def _find_console_users(node, depth=0):
    """The `IOConsoleUsers` value in a parsed ioreg plist, or None."""
    if depth > 3:
        return None
    if isinstance(node, list):
        for item in node:
            found = _find_console_users(item, depth + 1)
            if found is not None:
                return found
        return None
    if not isinstance(node, dict):
        return None
    if 'IOConsoleUsers' in node:
        return node['IOConsoleUsers']
    return _find_console_users(node.get('IORegistryEntryChildren'), depth + 1)


def classify(plist_bytes):
    """(usable, reason) for the bytes `ioreg -n Root -d1 -a` printed."""
    try:
        root = plistlib.loads(plist_bytes)
    except Exception:  # plistlib raises several unrelated types
        return False, 'ioreg output could not be parsed as a plist'
    users = _find_console_users(root)
    if users is None:
        return False, 'ioreg output has no IOConsoleUsers'
    if not isinstance(users, list):
        return False, 'IOConsoleUsers in ioreg output is not a list'
    if not users:
        return False, 'no console session (IOConsoleUsers is empty)'
    first = users[0]
    if not isinstance(first, dict):
        return False, 'IOConsoleUsers[0] in ioreg output is not a dictionary'
    if first.get('CGSSessionScreenIsLocked') is True:
        return False, 'console session is locked'
    if first.get('kCGSSessionOnConsoleKey') is not True:
        return False, 'session is not on console'
    return True, 'console session is unlocked and on console'


def query():
    """(usable, reason) for this machine, by running ioreg."""
    try:
        proc = subprocess.run(
            IOREG, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
            timeout=20)
    except FileNotFoundError:
        return False, 'ioreg not found'
    except (OSError, subprocess.TimeoutExpired) as e:
        return False, 'ioreg could not be run (%s)' % e.__class__.__name__
    if proc.returncode != 0:
        return False, 'ioreg exited with status %d' % proc.returncode
    return classify(proc.stdout)


def _sample(users):
    return plistlib.dumps({'IORegistryEntryName': 'Root',
                           'IOConsoleUsers': users})


SELF_TEST = [
    ('locked', _sample([{'CGSSessionScreenIsLocked': True,
                         'kCGSSessionOnConsoleKey': True}])),
    ('usable', _sample([{'kCGSSessionOnConsoleKey': True}])),
    ('usable-nested', plistlib.dumps([{
        'IORegistryEntryName': 'Root',
        'IORegistryEntryChildren': [{
            'IOConsoleUsers': [{'kCGSSessionOnConsoleKey': True}]}]}])),
    ('unlocked-flag-false', _sample([{'CGSSessionScreenIsLocked': False,
                                      'kCGSSessionOnConsoleKey': True}])),
    ('not-on-console', _sample([{'kCGSSessionOnConsoleKey': False}])),
    ('no-users', _sample([])),
    ('no-key', plistlib.dumps({'IORegistryEntryName': 'Root'})),
    ('garbage', b'this is not a plist\n'),
]


def main(argv):
    if argv == ['--self-test']:
        for name, data in SELF_TEST:
            usable, reason = classify(data)
            print('%s: usable=%d reason=%s' % (name, int(usable), reason))
        return 0
    if argv:
        sys.stderr.write(
            'usage: macos_console_session.py [--self-test]\n')
        return 2
    usable, reason = query()
    print('usable=%d reason=%s' % (int(usable), reason))
    return 0 if usable else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
