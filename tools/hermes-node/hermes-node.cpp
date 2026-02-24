/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <hermes/node-compat/runtime/hermes_node_runtime.h>

#include <cstdio>
#include <cstring>

using hermes::node_compat::HermesNodeConfig;
using hermes::node_compat::runHermesNode;

static void printUsage(const char *argv0) {
  std::fprintf(
      stderr,
      "Usage: %s [options] [script.js] [-- script-args...]\n"
      "\n"
      "Options:\n"
      "  --node-version <version>  Override process.version (e.g. v24.13.0)\n"
      "  -h, --help                Show this help\n",
      argv0);
}

int main(int argc, char **argv) {
  HermesNodeConfig config;
  int scriptArgIndex = argc; // no script by default

  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--help") == 0 ||
        std::strcmp(argv[i], "-h") == 0) {
      printUsage(argv[0]);
      return 0;
    } else if (std::strcmp(argv[i], "--node-version") == 0) {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "Error: --node-version requires a value\n");
        return 1;
      }
      config.nodeVersion = argv[++i];
    } else if (argv[i][0] == '-') {
      std::fprintf(stderr, "Error: unknown option '%s'\n", argv[i]);
      return 1;
    } else {
      scriptArgIndex = i;
      break;
    }
  }

  // Build process.argv: [binary, script, ...script-args].
  config.argv.push_back(argv[0]);
  for (int i = scriptArgIndex; i < argc; ++i)
    config.argv.push_back(argv[i]);

  if (scriptArgIndex < argc) {
    config.scriptPath = argv[scriptArgIndex];
  } else {
    config.enableRepl = true;
  }

  return runHermesNode(config);
}
