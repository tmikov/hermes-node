/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <hermes/node-compat/bundle/bundle_generation.h>
#include <hermes/node-compat/bundle/bundle_tools.h>
#include <hermes/node-compat/bytecode-dump/bytecode_dump.h>
#include <hermes/node-compat/runtime/hermes_node_runtime.h>
#include <hermes/node-compat/version.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <optional>
#include <string>

using hermes::node_compat::HermesNodeConfig;
using hermes::node_compat::runHermesNode;

/// The read-only verbs: they describe a file rather than running one.
///
/// Deliberately not fields on HermesNodeConfig. Nothing in the runtime ever
/// sees them, because runToolVerb() below handles them and returns before
/// runHermesNode() is reached.
struct ToolOptions {
  /// --dump: print the tables of the container named by --bundle.
  bool dump = false;
  /// --extract-module=<identity>: write one module's payload out of the
  /// container named by --bundle, to --out. std::nullopt when the verb was
  /// not requested -- an empty *value* ("--extract-module=") is still a
  /// request, just for an identity that will not resolve to anything, and
  /// must not be indistinguishable from the verb never being named at all.
  std::optional<std::string> extractModule;
  /// --dump-bytecode=<file>: disassemble a file of Hermes bytecode.
  /// std::nullopt when the verb was not requested, for the same reason
  /// extractModule is optional: "--dump-bytecode=" is a request naming an
  /// empty path, which fails as a path, and that is not the same thing as
  /// never having named the verb.
  ///
  /// Unlike the two above, this one names its own file rather than reading
  /// --bundle: a bytecode file is not a container, and the tool has no
  /// dependency on the bundle format at all.
  std::optional<std::string> dumpBytecode;
  /// --out=<path>: destination for --extract-module. std::nullopt when the
  /// flag was not given, for the same reason the two above are optional:
  /// the flag matrix rejects both "--out without --extract-module" and
  /// "--out=" with an empty path, and one of those questions cannot be
  /// answered by a plain string that is empty either way.
  std::optional<std::string> out;
};

/// Runs whichever read-only verb the arguments asked for. Returns false if
/// they asked for none, leaving \p exitCode untouched; otherwise runs it and
/// returns true with its exit code in \p exitCode.
///
/// None of these verbs needs a JavaScript runtime, an event loop, or a
/// napi_env, and none of them executes a bundled program. Dispatching here
/// rather than from inside runHermesNode is what keeps that true: a
/// diagnostic tool that booted a runtime first could fail for reasons that
/// have nothing to do with the file being diagnosed, which is the opposite
/// of what a diagnostic tool is for.
///
/// A new verb is one more branch here, each reading its own options and
/// returning true.
///
/// The order of the branches carries no meaning: checkToolOptions() has
/// already rejected any invocation naming more than one verb, so at most one
/// of them can be taken.
static bool runToolVerb(
    const HermesNodeConfig &config,
    const ToolOptions &tools,
    int &exitCode) {
  if (tools.dump) {
    exitCode = hermes::node_compat::dumpBundle(
        config.bundlePath,
        hermes::node_compat::bundleGenerationTag(),
        config.verbose,
        std::cout,
        std::cerr);
    return true;
  }
  if (tools.extractModule.has_value()) {
    exitCode = hermes::node_compat::extractModule(
        config.bundlePath, *tools.extractModule, *tools.out, std::cerr);
    return true;
  }
  if (tools.dumpBytecode.has_value()) {
    exitCode = hermes::node_compat::dumpBytecodeFile(
        *tools.dumpBytecode, config.verbose, std::cout, std::cerr);
    return true;
  }
  return false;
}

/// Validates every combination the read-only verbs take part in, and the
/// flags that only exist to serve them (--out, --verbose). Reports the first
/// problem on stderr and returns false; returns true when the arguments name
/// at most one verb and everything that verb needs.
///
/// Called after the parse loop rather than from inside it, the same way the
/// --optimize and --bundle refusals below are, so that no rule depends on
/// the order the flags were typed in: --dump --bundle=x and
/// --bundle=x --dump are the same invocation and must produce the same
/// answer. Each message names both flags involved, because "invalid
/// arguments" is not something a user can act on.
static bool checkToolOptions(
    const HermesNodeConfig &config,
    const ToolOptions &tools) {
  const bool inspecting = config.inspect || config.inspectBrk;

  // Two verbs in one invocation. Each of these is a different job on a
  // different file, so there is no sensible winner to pick; picking one
  // silently would answer a question the user did not ask.
  if (tools.dump && tools.extractModule.has_value()) {
    std::fprintf(
        stderr, "Error: --dump cannot be combined with --extract-module.\n");
    return false;
  }
  if (tools.dumpBytecode.has_value() && tools.dump) {
    std::fprintf(
        stderr, "Error: --dump-bytecode cannot be combined with --dump.\n");
    return false;
  }
  if (tools.dumpBytecode.has_value() && tools.extractModule.has_value()) {
    std::fprintf(
        stderr,
        "Error: --dump-bytecode cannot be combined with --extract-module.\n");
    return false;
  }

  // At most one verb survives the checks above, so the name of the verb in
  // play is well defined from here on.
  const char *verb = nullptr;
  if (tools.dump)
    verb = "--dump";
  else if (tools.extractModule.has_value())
    verb = "--extract-module";
  else if (tools.dumpBytecode.has_value())
    verb = "--dump-bytecode";

  // --dump-bytecode names its own file. A container is neither an input nor
  // an output of it, so naming one alongside describes two jobs.
  if (tools.dumpBytecode.has_value() && !config.bundlePath.empty()) {
    std::fprintf(
        stderr,
        "Error: --dump-bytecode cannot be combined with --bundle.\n"
        "--dump-bytecode reads a file of bytecode; use --bundle=<file> --dump "
        "to describe a container.\n");
    return false;
  }
  if (tools.dumpBytecode.has_value() && !config.buildBundlePath.empty()) {
    std::fprintf(
        stderr,
        "Error: --dump-bytecode cannot be combined with --build-bundle.\n");
    return false;
  }

  // The other two verbs read a container, which has to be named.
  if (tools.dump && config.bundlePath.empty()) {
    std::fprintf(stderr, "Error: --dump requires --bundle=<file>.\n");
    return false;
  }
  if (tools.extractModule.has_value() && config.bundlePath.empty()) {
    std::fprintf(stderr, "Error: --extract-module requires --bundle=<file>.\n");
    return false;
  }

  // --out is never inferred from the identity and never serves anything
  // else: writing a file the user did not name is how a tool overwrites
  // something it should not.
  if (tools.extractModule.has_value() && !tools.out.has_value()) {
    std::fprintf(stderr, "Error: --extract-module requires --out=<file>.\n");
    return false;
  }
  if (tools.out.has_value() && !tools.extractModule.has_value()) {
    std::fprintf(
        stderr,
        "Error: --out requires --extract-module.\n"
        "Nothing else writes a file, so there would be nothing to put in "
        "it.\n");
    return false;
  }

  // --verbose has exactly three consumers. Anywhere else it promises output
  // that will never appear, which is worse than a refusal.
  if (config.verbose && config.buildBundlePath.empty() && !tools.dump &&
      !tools.dumpBytecode.has_value()) {
    std::fprintf(
        stderr,
        "Error: --verbose requires --build-bundle, --dump or "
        "--dump-bytecode.\n");
    return false;
  }

  // --include only means anything while building a bundle: it seeds the
  // producer's worklist with a module the require() scanner cannot reach on
  // its own. Anywhere else there is no worklist for it to seed.
  if (!config.includeModules.empty() && config.buildBundlePath.empty()) {
    std::fprintf(stderr, "Error: --include requires --build-bundle.\n");
    return false;
  }

  // --preload, likewise: it seeds the producer's worklist and records the
  // module in the container's preload table. Neither means anything without
  // a bundle being built.
  if (!config.preloadModules.empty() && config.buildBundlePath.empty()) {
    std::fprintf(stderr, "Error: --preload requires --build-bundle.\n");
    return false;
  }

  // A bundle carries its own preloads, recorded at build time by --preload
  // and resolved from inside the container. Run-time -r/--require resolved
  // from the real filesystem before the bundle loader was even installed,
  // which made it an injection point into a sealed artifact; refusing the
  // combination removes that by construction. -r with --build-bundle is
  // untouched -- a build runs in the disk world.
  if (!config.bundlePath.empty() && !config.requireModules.empty()) {
    std::fprintf(
        stderr, "Error: --bundle cannot be combined with -r or --require.\n");
    return false;
  }

  // None of the verbs runs a program, so an inspector session would have
  // nothing to attach to. This is a different reason from the --bundle
  // refusal below -- that one is about bytecode compiled without full debug
  // info -- so it is checked first and says so in its own words.
  if (verb && inspecting) {
    std::fprintf(
        stderr,
        "Error: %s cannot be combined with --inspect or --inspect-brk.\n"
        "%s describes a file, it does not run one, so there would be nothing "
        "to inspect.\n",
        verb,
        verb);
    return false;
  }

  // An empty value is a flag naming a file that cannot exist. Letting it
  // through produces a diagnostic with neither a filename nor a flag in it
  // ("error: : No such file or directory").
  if (tools.dumpBytecode.has_value() && tools.dumpBytecode->empty()) {
    std::fprintf(stderr, "Error: --dump-bytecode requires a file path.\n");
    return false;
  }
  if (tools.out.has_value() && tools.out->empty()) {
    std::fprintf(stderr, "Error: --out requires a file path.\n");
    return false;
  }

  return true;
}

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
      "  --include=<specifier>          With --build-bundle, also package a "
      "module the\n"
      "                                 require() scanner cannot discover "
      "(repeatable)\n"
      "  --preload=<specifier>          With --build-bundle, also package a "
      "module and\n"
      "                                 record it to run before the entry "
      "point\n"
      "                                 (repeatable)\n"
      "  --verbose                      With --build-bundle, narrate the "
      "walk to stderr;\n"
      "                                 with --dump, add per-module edge "
      "counts;\n"
      "                                 with --dump-bytecode, add source "
      "locations\n"
      "  --bundle=<file>                Run an application from a bundle file\n"
      "  --dump                         With --bundle, print the container's "
      "tables\n"
      "  --extract-module=<identity>    With --bundle and --out, write one "
      "module's\n"
      "                                 payload to <file>\n"
      "  --out=<file>                   Destination for --extract-module\n"
      "  --dump-bytecode=<file>         Disassemble a Hermes bytecode file "
      "or a\n"
      "                                 compile cache entry\n"
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
  ToolOptions tools;
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
    } else if (std::strcmp(argv[i], "--no-compile-cache") == 0) {
      config.process.disableCompileCache = true;
    } else if (std::strncmp(argv[i], "--build-bundle=", 15) == 0) {
      config.buildBundlePath = argv[i] + 15;
    } else if (std::strncmp(argv[i], "--include=", 10) == 0) {
      config.includeModules.push_back(argv[i] + 10);
    } else if (std::strncmp(argv[i], "--preload=", 10) == 0) {
      config.preloadModules.push_back(argv[i] + 10);
    } else if (std::strcmp(argv[i], "--verbose") == 0) {
      config.verbose = true;
    } else if (std::strncmp(argv[i], "--bundle=", 9) == 0) {
      config.bundlePath = argv[i] + 9;
    } else if (std::strcmp(argv[i], "--dump") == 0) {
      tools.dump = true;
    } else if (std::strncmp(argv[i], "--extract-module=", 17) == 0) {
      tools.extractModule = argv[i] + 17;
    } else if (std::strncmp(argv[i], "--dump-bytecode=", 16) == 0) {
      tools.dumpBytecode = argv[i] + 16;
    } else if (std::strncmp(argv[i], "--out=", 6) == 0) {
      tools.out = argv[i] + 6;
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

  // The read-only verbs and the flags that serve them, as one block, before
  // the refusals that belong to running a program: a verb that never starts
  // the program should not be explained in terms of the debugger.
  if (!checkToolOptions(config, tools))
    return 1;

  // Same reasoning, same shape: a bundle's bytecode was produced by
  // hermes_compile_to_bytecode, which emits DebugInfoSetting::THROWING, and
  // there is no source to recompile from with ALL.
  if (!config.bundlePath.empty() && (config.inspect || config.inspectBrk)) {
    std::fprintf(
        stderr,
        "Error: --bundle cannot be combined with --inspect or --inspect-brk.\n"
        "Bundled code is compiled without the full debug info the debugger "
        "needs to set breakpoints.\n");
    return 1;
  }

  // --bundle runs a container that was already built; --build-bundle builds
  // one. Consuming and producing in the same invocation is not a mode either
  // flag was designed for, and silently picking one would hide the mistake
  // rather than reject it.
  if (!config.bundlePath.empty() && !config.buildBundlePath.empty()) {
    std::fprintf(
        stderr, "Error: --bundle cannot be combined with --build-bundle.\n");
    return 1;
  }

  // --bundle runs the entry module the container was built from; -e/--eval
  // supplies a different program to run instead. There is no entry point
  // left for the bundle to provide once eval code wins, so the two never
  // combine.
  if (!config.bundlePath.empty() && hasEvalCode) {
    std::fprintf(
        stderr, "Error: --bundle cannot be combined with -e or --eval.\n");
    return 1;
  }

  // Before anything that belongs to running a program: the read-only verbs
  // neither need nor start a runtime, so they are answered here and nothing
  // below this point executes for them.
  int toolExitCode = 0;
  if (runToolVerb(config, tools, toolExitCode))
    return toolExitCode;

  // Build process.argv: [binary, script-or-arg1, ...].
  config.argv.push_back(argv[0]);
  // In bundle mode the bundle is the program, so it occupies the slot the
  // script path would: process.argv[1] is the bundle path exactly as given,
  // and anything after `--` follows it the way a script's arguments do.
  if (!config.bundlePath.empty())
    config.argv.push_back(config.bundlePath);
  for (int i = argvStartIndex; i < argc; ++i)
    config.argv.push_back(argv[i]);

  // In bundle mode the bundle supplies the entry point, so a positional
  // argument is one of the program's own arguments rather than a script to
  // run, and there is nothing to start a REPL for.
  if (config.bundlePath.empty()) {
    if (!hasEvalCode && scriptArgIndex < argc) {
      config.scriptPath = argv[scriptArgIndex];
    } else if (!hasEvalCode) {
      config.enableRepl = true;
    }
  }

  return runHermesNode(config);
}
