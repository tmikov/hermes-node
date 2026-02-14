/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <cstdio>

int main(int argc, char **argv) {
  if (argc < 2) {
    std::fprintf(stderr, "Usage: hermes-node <script.js>\n");
    return 0;
  }
  std::fprintf(stderr, "hermes-node: not yet implemented\n");
  return 1;
}
