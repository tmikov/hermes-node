#!/usr/bin/env python3
# Copyright (c) Tzvetan Mikov.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

"""Wrap a JS module in the standard Node CJS wrapper.

Usage: wrap-cjs.py <input.js> <module_id> <output.js>

The wrapper puts the opening function header on the SAME line as the
first line of source so that bytecode line numbers match the original
file (line 1 of the module = line 1 of the output).

Output:
  (function(exports, require, module, __filename, __dirname) {<source>
  });
  //# sourceURL=<module_id>
"""

import sys

def main():
    if len(sys.argv) != 4:
        print("Usage: wrap-cjs.py <input.js> <module_id> <output.js>",
              file=sys.stderr)
        sys.exit(1)

    input_path = sys.argv[1]
    module_id = sys.argv[2]
    output_path = sys.argv[3]

    with open(input_path, "r", encoding="utf-8") as f:
        source = f.read()

    prefix = "(function(exports, require, module, __filename, __dirname) {"
    suffix = "\n});\n//# sourceURL=" + module_id + "\n"

    with open(output_path, "w", encoding="utf-8") as f:
        f.write(prefix)
        f.write(source)
        f.write(suffix)


if __name__ == "__main__":
    main()
