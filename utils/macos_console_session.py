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
PyObjC, no third-party module. `IOConsoleUsers` has one entry per GUI
session; ours is the one whose `kCGSSessionUserIDKey` is our uid (else the
one marked `kCGSSessionOnConsoleKey`). It carries `CGSSessionScreenIsLocked`
(present only while locked, so an ABSENT key means not locked) and
`kCGSSessionOnConsoleKey`. Parsing the plist into that list is
`_find_console_users()`, kept small so a different real shape changes one
function. Anything that cannot be
read -- ioreg missing, exiting non-zero, output that is not a plist or has
no `IOConsoleUsers` -- is "not usable", with a reason, never an exception.

UNVERIFIED on the machine this was written on (Linux): the exact shape of
real `ioreg -a` output. `classify()` accepts the console-user list either at
the top level of the root dictionary or under `IORegistryEntryChildren`, and
a top-level array of dictionaries, which covers the shapes `ioreg -a` is
known to produce; the Mac round confirms it.
"""

import os
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


def _select_session(users, uid):
    """(entry, None) for this user's console-session entry, or (None, why).

    One entry per GUI session: with fast user switching the console one need
    not be first. Ours is the one whose `kCGSSessionUserIDKey` is our uid;
    failing that, the one entry marked on console. Anything ambiguous is
    refused with a reason rather than guessed at.
    """
    entries = [u for u in users if isinstance(u, dict)]
    if len(entries) != len(users):
        return None, 'IOConsoleUsers in ioreg output holds a non-dictionary'
    mine = [u for u in entries if u.get('kCGSSessionUserIDKey') == uid]
    if len(mine) == 1:
        return mine[0], None
    if len(mine) > 1:
        return None, '%d console sessions for uid %d' % (len(mine), uid)
    on_console = [u for u in entries if u.get('kCGSSessionOnConsoleKey')]
    if len(on_console) == 1:
        return on_console[0], None
    return None, ('no console session for uid %d, and %d sessions on '
                  'console' % (uid, len(on_console)))


def classify(plist_bytes, uid=None):
    """(usable, reason) for the bytes `ioreg -n Root -d1 -a` printed.

    `uid` defaults to this process's (os.getuid()); the self-test fixes it.
    Flags are read for truth, not identity: a lock flag of an unexpected
    type (an integer 1 rather than a boolean) reads as LOCKED -- the cost of
    that mistake is a skipped test, the cost of the other an unbounded wait.
    """
    if uid is None:
        uid = os.getuid()
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
    session, why = _select_session(users, uid)
    if session is None:
        return False, why
    if session.get('CGSSessionScreenIsLocked'):
        return False, 'console session is locked'
    if not session.get('kCGSSessionOnConsoleKey'):
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


# The uid the self-test classifies as, so its answers do not depend on who
# runs it.
SELF_TEST_UID = 501


def _me(**flags):
    entry = {'kCGSSessionUserIDKey': SELF_TEST_UID}
    entry.update(flags)
    return entry


SELF_TEST = [
    ('locked', _sample([_me(CGSSessionScreenIsLocked=True,
                            kCGSSessionOnConsoleKey=True)])),
    ('usable', _sample([_me(kCGSSessionOnConsoleKey=True)])),
    ('usable-nested', plistlib.dumps([{
        'IORegistryEntryName': 'Root',
        'IORegistryEntryChildren': [{
            'IOConsoleUsers': [_me(kCGSSessionOnConsoleKey=True)]}]}])),
    ('unlocked-flag-false', _sample([_me(CGSSessionScreenIsLocked=False,
                                         kCGSSessionOnConsoleKey=True)])),
    ('not-on-console', _sample([_me(kCGSSessionOnConsoleKey=False)])),
    # Fast user switching: another user's session first, ours second.
    ('console-user-second', _sample([
        {'kCGSSessionUserIDKey': 502, 'kCGSSessionOnConsoleKey': False,
         'CGSSessionScreenIsLocked': True},
        _me(kCGSSessionOnConsoleKey=True)])),
    # Ours is second and locked; the first is not locked: ours decides.
    ('mine-locked-other-not', _sample([
        {'kCGSSessionUserIDKey': 502, 'kCGSSessionOnConsoleKey': False},
        _me(CGSSessionScreenIsLocked=True, kCGSSessionOnConsoleKey=True)])),
    # No uid match: the single on-console entry is taken.
    ('no-uid-one-on-console', _sample([
        {'kCGSSessionUserIDKey': 502, 'kCGSSessionOnConsoleKey': False},
        {'kCGSSessionUserIDKey': 503, 'kCGSSessionOnConsoleKey': True}])),
    ('no-uid-none-on-console', _sample([
        {'kCGSSessionUserIDKey': 502, 'kCGSSessionOnConsoleKey': False}])),
    ('two-entries-for-uid', _sample([_me(kCGSSessionOnConsoleKey=True),
                                     _me(kCGSSessionOnConsoleKey=True)])),
    # A lock flag of an unexpected type reads as locked.
    ('integer-lock-flag', _sample([_me(CGSSessionScreenIsLocked=1,
                                       kCGSSessionOnConsoleKey=True)])),
    ('no-users', _sample([])),
    ('no-key', plistlib.dumps({'IORegistryEntryName': 'Root'})),
    ('garbage', b'this is not a plist\n'),
]


def main(argv):
    if argv == ['--self-test']:
        for name, data in SELF_TEST:
            usable, reason = classify(data, uid=SELF_TEST_UID)
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
