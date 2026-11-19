# Copyright (c) Tzvetan Mikov.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

"""Run a script with sys.platform forced to 'darwin'.

    run_as_darwin.py SCRIPT [ARG...]

So that utils/with-display.py's macOS branch -- the display lock, the
environment it sets, the time limit it applies -- can be exercised on any
POSIX machine. Everything that branch uses (fcntl.flock, process groups) is
POSIX, so the platform is the only thing being faked. On a real Mac this is
a no-op wrapper.
"""

import runpy
import sys

sys.platform = 'darwin'
script = sys.argv[1]
sys.argv = sys.argv[1:]
runpy.run_path(script, run_name='__main__')
