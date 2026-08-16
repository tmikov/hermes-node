/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <hermes/node-compat/runtime/hermes_node_runtime.h>
#include <hermes/node-compat/version.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

using hermes::node_compat::HermesNodeConfig;
using hermes::node_compat::runHermesNode;

static void printUsage(const char *argv0) {
  std::fprintf(
      stderr,
      "Usage: %s [options] [script.js] [-- script-args...]\n"
      "\n"
      "Options:\n"
      "  -e, --eval <code>              Evaluate code\n"
      "  --inspect[=[host:]port]        Enable inspector (default 127.0.0.1:9229)\n"
      "  --inspect-brk[=[host:]port]    Enable inspector, break before user code\n"
      "  --compile-cache=<dir>          Bytecode cache directory\n"
      "  --no-compile-cache             Disable the bytecode cache\n"
      "  --build-bundle=<file>          Compile the script and its requires "
      "into <file>\n"
      "  --verbose                      With --build-bundle, narrate the "
      "walk to stderr\n"
      "  --optimize=<default|on|off>    Optimize compiled code. default is on\n"
      "                                 with the cache, off without it\n"
      "  --inspect-open                 Open the DevTools URL in the system browser\n"
      "  --node-version <version>       Override process.version (e.g. v24.13.0)\n"
      "  -r, --require <module>         Preload a module before the script (repeatable)\n"
      "  -v, --version                  Print the hermes-node version and exit\n"
      "  -h, --help                     Show this help\n",
      argv0);
}

/// Parse an optional [host:]port value for --inspect/--inspect-brk.
/// \p value is the part after '=' (may be empty if no '=' was present).
/// Returns true on success, false on parse error.
static bool parseInspectHostPort(const char *value, HermesNodeConfig &config) {
  if (!value || value[0] == '\0')
    return true; // use defaults

  // Check if it's just a port number (all digits).
  const char *p = value;
  bool allDigits = true;
  while (*p) {
    if (*p < '0' || *p > '9') {
      allDigits = false;
      break;
    }
    ++p;
  }

  if (allDigits) {
    long port = std::strtol(value, nullptr, 10);
    if (port < 0 || port > 65535) {
      std::fprintf(stderr, "Error: invalid port number '%s'\n", value);
      return false;
    }
    config.inspectPort = static_cast<int>(port);
    return true;
  }

  // Look for the last ':' to split host:port.
  const char *lastColon = std::strrchr(value, ':');
  if (!lastColon || lastColon == value) {
    std::fprintf(stderr, "Error: invalid inspect address '%s'\n", value);
    return false;
  }

  config.inspectHost = std::string(value, lastColon - value);

  const char *portStr = lastColon + 1;
  if (*portStr == '\0') {
    std::fprintf(stderr, "Error: missing port in '%s'\n", value);
    return false;
  }
  long port = std::strtol(portStr, nullptr, 10);
  if (port < 0 || port > 65535) {
    std::fprintf(stderr, "Error: invalid port number '%s'\n", portStr);
    return false;
  }
  config.inspectPort = static_cast<int>(port);
  return true;
}

int main(int argc, char **argv) {
  HermesNodeConfig config;
  int scriptArgIndex = argc; // no script by default
  int argvStartIndex = argc;
  bool hasEvalCode = false;

  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--version") == 0 ||
        std::strcmp(argv[i], "-v") == 0) {
      std::printf("hermes-node %s\n", HERMES_NODE_VERSION_STRING);
      return 0;
    } else if (
        std::strcmp(argv[i], "--help") == 0 ||
        std::strcmp(argv[i], "-h") == 0) {
      printUsage(argv[0]);
      return 0;
    } else if (
        std::strcmp(argv[i], "-e") == 0 ||
        std::strcmp(argv[i], "--eval") == 0) {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "Error: %s requires a value\n", argv[i]);
        return 1;
      }
      config.evalCode = argv[++i];
      hasEvalCode = true;
    } else if (std::strncmp(argv[i], "--eval=", 7) == 0) {
      config.evalCode = argv[i] + 7;
      hasEvalCode = true;
    } else if (std::strcmp(argv[i], "--inspect") == 0) {
      config.inspect = true;
    } else if (std::strncmp(argv[i], "--inspect=", 10) == 0) {
      config.inspect = true;
      if (!parseInspectHostPort(argv[i] + 10, config))
        return 1;
    } else if (std::strcmp(argv[i], "--inspect-brk") == 0) {
      config.inspect = true;
      config.inspectBrk = true;
    } else if (std::strncmp(argv[i], "--inspect-brk=", 14) == 0) {
      config.inspect = true;
      config.inspectBrk = true;
      if (!parseInspectHostPort(argv[i] + 14, config))
        return 1;
    } else if (std::strcmp(argv[i], "--inspect-open") == 0) {
      config.inspect = true;
      config.inspectOpen = true;
    } else if (std::strncmp(argv[i], "--compile-cache=", 16) == 0) {
      config.process.compileCacheDir = argv[i] + 16;
    } else if (std::strncmp(argv[i], "--build-bundle=", 15) == 0) {
      config.buildBundlePath = argv[i] + 15;
    } else if (std::strcmp(argv[i], "--verbose") == 0) {
      config.verbose = true;
    } else if (std::strcmp(argv[i], "--no-compile-cache") == 0) {
      config.process.disableCompileCache = true;
    } else if (std::strncmp(argv[i], "--optimize=", 11) == 0) {
      const char *value = argv[i] + 11;
      if (std::strcmp(value, "default") == 0) {
        config.process.optimize = hermes::node_compat::OptimizeMode::kDefault;
      } else if (std::strcmp(value, "on") == 0) {
        config.process.optimize = hermes::node_compat::OptimizeMode::kOn;
      } else if (std::strcmp(value, "off") == 0) {
        config.process.optimize = hermes::node_compat::OptimizeMode::kOff;
      } else {
        std::fprintf(
            stderr,
            "Error: --optimize expects default, on or off (got '%s')\n",
            value);
        return 1;
      }
    } else if (std::strcmp(argv[i], "--node-version") == 0) {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "Error: --node-version requires a value\n");
        return 1;
      }
      config.process.nodeVersion = argv[++i];
    } else if (
        std::strcmp(argv[i], "-r") == 0 ||
        std::strcmp(argv[i], "--require") == 0) {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "Error: %s requires a value\n", argv[i]);
        return 1;
      }
      config.requireModules.push_back(argv[++i]);
    } else if (std::strcmp(argv[i], "--") == 0) {
      if (i + 1 < argc && !hasEvalCode) {
        scriptArgIndex = i + 1;
      }
      argvStartIndex = i + 1;
      break;
    } else if (argv[i][0] == '-') {
      std::fprintf(stderr, "Error: unknown option '%s'\n", argv[i]);
      return 1;
    } else {
      if (!hasEvalCode)
        scriptArgIndex = i;
      argvStartIndex = i;
      break;
    }
  }

  // Validated after the loop, not inside it, so the two flags can appear in
  // either order.
  //
  // Optimizing requires the compile API, which emits
  // DebugInfoSetting::THROWING; the debugger needs ALL to set breakpoints
  // anywhere. Rather than silently degrade debugging, refuse the pair. The
  // default mode already resolves to off under --inspect, because --inspect
  // disables the cache, so only an explicit --optimize=on lands here.
  if (config.process.optimize == hermes::node_compat::OptimizeMode::kOn &&
      (config.inspect || config.inspectBrk)) {
    std::fprintf(
        stderr,
        "Error: --optimize=on cannot be combined with --inspect or "
        "--inspect-brk.\n"
        "Optimized code is compiled without the full debug info the debugger "
        "needs to set breakpoints.\n");
    return 1;
  }

  // Build process.argv: [binary, script-or-arg1, ...].
  config.argv.push_back(argv[0]);
  for (int i = argvStartIndex; i < argc; ++i)
    config.argv.push_back(argv[i]);

  if (!hasEvalCode && scriptArgIndex < argc) {
    config.scriptPath = argv[scriptArgIndex];
  } else if (!hasEvalCode) {
    config.enableRepl = true;
  }

  return runHermesNode(config);
}
