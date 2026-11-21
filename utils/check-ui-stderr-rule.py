#!/usr/bin/env python3
# Copyright (c) Tzvetan Mikov.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
"""Check that every window test excludes Sokol's error lines.

  check-ui-stderr-rule.py <test dir>

A window test is a test/ui-*.js RUN line that runs under %with-display.
Each must send the binary's stderr to a file of its own (`2> %t...`), and
some %FileCheck RUN line of the same test must read that file with

  --implicit-check-not='[sapp:error' --implicit-check-not='[sokol-'

so that a Sokol error printed by a run that otherwise behaves is a
failure. The exceptions provoke a Sokol message on purpose and exclude
everything but the instance they provoke; see NARROWED.

Prints one line per violation and exits 1 if there was any.
"""

import glob
import os
import re
import sys

SAPP = "--implicit-check-not='[sapp:error'"
ALL_GFX = "--implicit-check-not='[sokol-'"
# These two make the PRODUCING sokol_gfx instance panic, so its lines are
# what they assert; the presenting instance must still be silent.
NARROWED = {
    'ui-sokol-panic.js': "--implicit-check-not='[sokol-present'",
    'ui-sokol-validation.js': "--implicit-check-not='[sokol-present'",
}

STDERR_FILE = re.compile(r'2> ?(%t[\w.]*)')


def check(path):
    name = os.path.basename(path)
    gfx = NARROWED.get(name, ALL_GFX)
    with open(path, encoding='utf-8') as f:
        runs = [(n, line) for n, line in enumerate(f, 1)
                if line.startswith('// RUN:')]
    problems = []
    for n, line in runs:
        if '%with-display' not in line:
            continue
        captured = STDERR_FILE.findall(line)
        if not captured:
            problems.append((n, 'runs under %with-display without '
                                'sending stderr to a file (2> %t...)'))
            continue
        for err in captured:
            reader = re.compile(r'< ?' + re.escape(err) + r'(?![\w.])')
            if not any('%FileCheck' in other and reader.search(other) and
                       SAPP in other and gfx in other
                       for _, other in runs):
                problems.append(
                    (n, 'no %FileCheck run reads ' + err + ' with ' +
                        SAPP + ' ' + gfx))
    return [(path, n, what) for n, what in problems]


def main():
    if len(sys.argv) != 2:
        sys.stderr.write(__doc__)
        return 2
    problems = []
    files = sorted(glob.glob(os.path.join(sys.argv[1], 'ui-*.js')))
    for path in files:
        problems.extend(check(path))
    for path, n, what in problems:
        print('%s:%d: %s' % (path, n, what))
    print('ui stderr rule: %d file(s), %d violation(s)' %
          (len(files), len(problems)))
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main())
