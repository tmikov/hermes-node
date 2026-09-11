/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <hermes/node-compat/build-exe/build_exe.h>
#include <hermes/node-compat/bundle/atomic_write.h>
#include <hermes/node-compat/bundle/bundle_generation.h>
#include <hermes/node-compat/bundle/bundle_run.h>
#include <hermes/node-compat/bundle/bundle_tools.h>
#include <hermes/node-compat/bytecode-dump/bytecode_dump.h>
#include <hermes/node-compat/compile-cache/cache_tools.h>
#include <hermes/node-compat/compile-cache/compile_cache.h>
#include <hermes/node-compat/runtime/hermes_node_runtime.h>
#include <hermes/node-compat/version.h>
#include <hermes/node-compat/vm-options/vm_options.h>

#include <uv.h>

#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

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
  /// --verify-natives: check the sidecar files of the container named by
  /// --bundle against the lengths and hashes it recorded at build time. A
  /// bool, not an optional<string>, because it takes no value.
  bool verifyNatives = false;
  /// --build-exe=<output>: link a standalone executable from the container
  /// named by the positional argument. std::nullopt when the verb was not
  /// requested; an empty value ("--build-exe=") is still a request, for an
  /// output path that cannot be written, and the two must stay
  /// distinguishable.
  std::optional<std::string> buildExe;
  /// --kit=<dir>: where to find the kit. std::nullopt means "beside this
  /// binary", resolved at use.
  std::optional<std::string> kitDir;
  /// --cc=<compiler>: the toolchain driver to assemble and link with,
  /// overriding every candidate --build-exe would otherwise try. Empty
  /// means "not given"; an empty VALUE ("--cc=") is rejected by
  /// checkToolOptions(), since a driver with no name cannot be run and
  /// naming the flag is more useful than a spawn failure with nothing in
  /// it.
  std::optional<std::string> cc;
  /// --dump-wasm=<file>: print a standalone --record-wasm file's entries.
  /// std::nullopt when the verb was not requested, for the same reason
  /// dumpBytecode is optional: "--dump-wasm=" is a request naming an empty
  /// path, which fails as a path, and that is not the same thing as never
  /// having named the verb. Like --dump-bytecode and unlike --dump, this
  /// names its own file rather than reading --bundle: a --record-wasm file
  /// is not a container.
  std::optional<std::string> dumpWasm;
};

/// Resolves the kit directory for --build-exe. std::nullopt (no --kit)
/// means "beside this binary": the directory of the running executable,
/// plus "kit".
///
/// Found with uv_exepath, not argv[0] -- argv[0] is whatever the caller
/// chose to exec with and need not be a path at all, let alone the real
/// one. Realpath'd so a binary reached through a symlink resolves the kit
/// relative to where it actually lives, not to the symlink's directory.
/// buildExecutable() itself reports a clear error if nothing is there
/// (readKitManifest's "cannot open kit manifest"), so a failure to locate
/// the running binary just falls back to a relative "kit" and lets that
/// same error fire.
static std::string resolveKitDir(const std::optional<std::string> &kitDir) {
  if (kitDir.has_value())
    return *kitDir;

  char execBuf[PATH_MAX];
  size_t execSize = sizeof(execBuf);
  if (uv_exepath(execBuf, &execSize) != 0)
    return "kit";
  std::string exePath(execBuf, execSize);

  char realBuf[PATH_MAX];
  if (const char *resolved = realpath(exePath.c_str(), realBuf))
    exePath = resolved;

  size_t slash = exePath.find_last_of('/');
  std::string dir = (slash == std::string::npos) ? std::string(".")
                                                 : exePath.substr(0, slash);
  return dir + "/kit";
}

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
///
/// \p containerPath is the positional argument, read directly from argv
/// rather than from config.scriptPath: runToolVerb() runs before main()
/// assigns that field (see the comment at its call site), and reading it
/// here would silently see an empty string.
static bool runToolVerb(
    const HermesNodeConfig &config,
    const ToolOptions &tools,
    const std::string &containerPath,
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
  if (tools.verifyNatives) {
    exitCode = hermes::node_compat::verifyNatives(
        config.bundlePath, config.verbose, std::cout, std::cerr);
    return true;
  }
  if (tools.buildExe.has_value()) {
    exitCode = hermes::node_compat::buildExecutable(
        containerPath,
        *tools.buildExe,
        resolveKitDir(tools.kitDir),
        tools.cc.value_or(std::string()),
        config.verbose,
        std::cout,
        std::cerr);
    return true;
  }
  if (tools.dumpWasm.has_value()) {
    exitCode = hermes::node_compat::dumpWasmRecord(
        *tools.dumpWasm,
        HERMES_NODE_VERSION_STRING,
        config.verbose,
        std::cout,
        std::cerr);
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
    const ToolOptions &tools,
    bool hasEvalCode,
    bool recordWasmGiven,
    const std::string &containerPath) {
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
  if (tools.verifyNatives && tools.dump) {
    std::fprintf(
        stderr, "Error: --verify-natives cannot be combined with --dump.\n");
    return false;
  }
  if (tools.verifyNatives && tools.extractModule.has_value()) {
    std::fprintf(
        stderr,
        "Error: --verify-natives cannot be combined with --extract-module.\n");
    return false;
  }
  if (tools.verifyNatives && tools.dumpBytecode.has_value()) {
    std::fprintf(
        stderr,
        "Error: --verify-natives cannot be combined with --dump-bytecode.\n");
    return false;
  }

  // --build-exe is a fifth verb, checked against each of the other four the
  // same way: two jobs on two files, with no sensible winner to pick.
  if (tools.buildExe.has_value() && tools.dump) {
    std::fprintf(
        stderr, "Error: --build-exe cannot be combined with --dump.\n");
    return false;
  }
  if (tools.buildExe.has_value() && tools.extractModule.has_value()) {
    std::fprintf(
        stderr,
        "Error: --build-exe cannot be combined with --extract-module.\n");
    return false;
  }
  if (tools.buildExe.has_value() && tools.dumpBytecode.has_value()) {
    std::fprintf(
        stderr,
        "Error: --build-exe cannot be combined with --dump-bytecode.\n");
    return false;
  }
  if (tools.buildExe.has_value() && tools.verifyNatives) {
    std::fprintf(
        stderr,
        "Error: --build-exe cannot be combined with --verify-natives.\n");
    return false;
  }

  // --dump-wasm is a sixth verb, checked against each of the other five the
  // same way: two jobs on two files, with no sensible winner to pick.
  if (tools.dumpWasm.has_value() && tools.dump) {
    std::fprintf(
        stderr, "Error: --dump-wasm cannot be combined with --dump.\n");
    return false;
  }
  if (tools.dumpWasm.has_value() && tools.extractModule.has_value()) {
    std::fprintf(
        stderr,
        "Error: --dump-wasm cannot be combined with --extract-module.\n");
    return false;
  }
  if (tools.dumpWasm.has_value() && tools.dumpBytecode.has_value()) {
    std::fprintf(
        stderr,
        "Error: --dump-wasm cannot be combined with --dump-bytecode.\n");
    return false;
  }
  if (tools.dumpWasm.has_value() && tools.verifyNatives) {
    std::fprintf(
        stderr,
        "Error: --dump-wasm cannot be combined with --verify-natives.\n");
    return false;
  }
  if (tools.dumpWasm.has_value() && tools.buildExe.has_value()) {
    std::fprintf(
        stderr, "Error: --dump-wasm cannot be combined with --build-exe.\n");
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
  else if (tools.verifyNatives)
    verb = "--verify-natives";
  else if (tools.buildExe.has_value())
    verb = "--build-exe";
  else if (tools.dumpWasm.has_value())
    verb = "--dump-wasm";

  // --vm configures a runtime. None of the read-only verbs creates one,
  // and --build-exe's options belong to the container it reads, not to
  // the command line that links it.
  if (!config.process.vmOptions.empty() && verb != nullptr) {
    std::fprintf(stderr, "Error: --vm cannot be combined with %s.\n", verb);
    return false;
  }
  // --record-wasm asks the run that is about to happen to write a file as it
  // executes. None of the read-only verbs runs anything, so there is nothing
  // for it to record.
  if (!config.recordWasmPath.empty() && verb != nullptr) {
    std::fprintf(
        stderr, "Error: --record-wasm cannot be combined with %s.\n", verb);
    return false;
  }
  if (config.allowVmOptionsOverride && config.buildBundlePath.empty()) {
    std::fprintf(
        stderr,
        "Error: --allow-vm-options-override requires --build-bundle.\n"
        "It records a bit in a container; this run is not building one.\n");
    return false;
  }

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

  // --dump-wasm names its own standalone --record-wasm file, the same way
  // --dump-bytecode names its own bytecode file: a container is neither an
  // input nor an output of it.
  if (tools.dumpWasm.has_value() && !config.bundlePath.empty()) {
    std::fprintf(
        stderr,
        "Error: --dump-wasm cannot be combined with --bundle.\n"
        "--dump-wasm reads a standalone --record-wasm file; a container's "
        "own Wasm table is shown by --bundle=<file> --dump.\n");
    return false;
  }
  if (tools.dumpWasm.has_value() && !config.buildBundlePath.empty()) {
    std::fprintf(
        stderr, "Error: --dump-wasm cannot be combined with --build-bundle.\n");
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
  if (tools.verifyNatives && config.bundlePath.empty()) {
    std::fprintf(stderr, "Error: --verify-natives requires --bundle=<file>.\n");
    return false;
  }

  // --build-exe reads its container from the positional argument, not from
  // --bundle: linking happens before there is a program to run one, and
  // --bundle names a container to run. The two describe different jobs on
  // what could be the same file, so naming both still asks for two jobs at
  // once, the same as the pairs above.
  if (tools.buildExe.has_value() && !config.bundlePath.empty()) {
    std::fprintf(
        stderr, "Error: --build-exe cannot be combined with --bundle.\n");
    return false;
  }
  // --build-bundle produces a container from source; --build-exe consumes
  // an already-built one. Both are producers of a different artifact, and
  // there is no order to run them in within one invocation.
  if (tools.buildExe.has_value() && !config.buildBundlePath.empty()) {
    std::fprintf(
        stderr, "Error: --build-exe cannot be combined with --build-bundle.\n");
    return false;
  }
  // -e/--eval supplies a program to run instead of reading one from disk;
  // --build-exe links a container that was already built. Neither leaves
  // anything for the other to do.
  if (tools.buildExe.has_value() && hasEvalCode) {
    std::fprintf(
        stderr, "Error: --build-exe cannot be combined with -e or --eval.\n");
    return false;
  }
  // The container --build-exe links is its positional argument, the same
  // slot a script path or --bundle's own argument occupies for the other
  // modes -- there is nothing to link without one.
  if (tools.buildExe.has_value() && containerPath.empty()) {
    std::fprintf(
        stderr,
        "Error: --build-exe requires a bundle file argument, e.g. "
        "hermes-node --build-exe=<output> <bundle.hbb>.\n");
    return false;
  }

  // --kit only means anything while linking an executable: it says where
  // the prebuilt kit lives. Anywhere else there is nothing consuming it.
  if (tools.kitDir.has_value() && !tools.buildExe.has_value()) {
    std::fprintf(stderr, "Error: --kit requires --build-exe.\n");
    return false;
  }

  // --cc names the compiler the link runs, so like --kit it has no consumer
  // anywhere else. Every other verb reads a file and runs no toolchain.
  if (tools.cc.has_value() && !tools.buildExe.has_value()) {
    std::fprintf(stderr, "Error: --cc requires --build-exe.\n");
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

  // --verbose has exactly six consumers -- dumpWasmRecord() prints the full
  // 64-character digest under it instead of the truncated 16, the same way
  // dumpBundle() and verifyNatives() add detail under it. Anywhere else it
  // promises output that will never appear, which is worse than a refusal.
  if (config.verbose && config.buildBundlePath.empty() && !tools.dump &&
      !tools.verifyNatives && !tools.dumpBytecode.has_value() &&
      !tools.buildExe.has_value() && !tools.dumpWasm.has_value()) {
    std::fprintf(
        stderr,
        "Error: --verbose requires --build-bundle, --dump, --verify-natives, "
        "--dump-bytecode, --build-exe or --dump-wasm.\n");
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

  // --bake-wasm copies a --record-wasm file's entries into the container
  // being built. Outside bundle producer mode there is no container for it
  // to land in.
  if (!config.bakeWasmPaths.empty() && config.buildBundlePath.empty()) {
    std::fprintf(stderr, "Error: --bake-wasm requires --build-bundle.\n");
    return false;
  }

  // --record-wasm asks the run to write a file as it executes; --build-bundle
  // compiles and never runs, so it can compile no Wasm and the file would
  // always be empty.
  if (!config.recordWasmPath.empty() && !config.buildBundlePath.empty()) {
    std::fprintf(
        stderr,
        "Error: --record-wasm cannot be combined with --build-bundle.\n"
        "The producer compiles and never runs, so it can compile no Wasm; "
        "the file would always be empty.\n");
    return false;
  }

  // The recorder's write is a rename over the destination. A running
  // container keeps the mapping it already has, and so does the interpreter
  // for the script it is executing, so recording onto either would replace
  // the very file the run is reading from while the run continued happily to
  // completion -- the run succeeds and the artifact is destroyed. Compared
  // by (st_dev, st_ino), like --extract-module --out, so a symlink or a hard
  // link to the same file counts too.
  if (!config.recordWasmPath.empty() && !config.bundlePath.empty() &&
      hermes::node_compat::isSameFile(
          config.recordWasmPath, config.bundlePath)) {
    std::fprintf(
        stderr,
        "Error: --record-wasm=%s names the same file as --bundle=%s; "
        "recording onto the running container would replace it.\n",
        config.recordWasmPath.c_str(),
        config.bundlePath.c_str());
    return false;
  }
  if (!config.recordWasmPath.empty() && config.bundlePath.empty() &&
      !containerPath.empty() &&
      hermes::node_compat::isSameFile(config.recordWasmPath, containerPath)) {
    std::fprintf(
        stderr,
        "Error: --record-wasm=%s names the same file as the script being "
        "run (%s).\n",
        config.recordWasmPath.c_str(),
        containerPath.c_str());
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
  if (tools.buildExe.has_value() && tools.buildExe->empty()) {
    std::fprintf(stderr, "Error: --build-exe requires a file path.\n");
    return false;
  }
  if (tools.kitDir.has_value() && tools.kitDir->empty()) {
    std::fprintf(stderr, "Error: --kit requires a directory path.\n");
    return false;
  }
  if (tools.cc.has_value() && tools.cc->empty()) {
    std::fprintf(stderr, "Error: --cc requires a compiler name or path.\n");
    return false;
  }
  if (tools.dumpWasm.has_value() && tools.dumpWasm->empty()) {
    std::fprintf(stderr, "Error: --dump-wasm requires a file path.\n");
    return false;
  }
  // recordWasmPath itself cannot distinguish "given an empty value" from
  // "never given" -- both leave it as an empty string -- so the parse loop
  // passes recordWasmGiven alongside it, the same way hasEvalCode lets
  // checkToolOptions ask the same question about evalCode.
  if (recordWasmGiven && config.recordWasmPath.empty()) {
    std::fprintf(stderr, "Error: --record-wasm requires a file path.\n");
    return false;
  }
  // --bake-wasm is repeatable, so each occurrence can be checked the same
  // way --vm's loop below checks each of its own repeated values.
  for (const std::string &path : config.bakeWasmPaths) {
    if (path.empty()) {
      std::fprintf(stderr, "Error: --bake-wasm requires a file path.\n");
      return false;
    }
  }
  // --vm takes a flag name rather than a path, but empty is the same
  // mistake and belongs in the same place: checked here, both spellings
  // ("--vm=" and "--vm ''") reach one message, where the parse loop could
  // only see the first. Letting an empty value through instead would
  // report "unknown VM option ''", which names neither the flag nor what
  // is wrong with it.
  for (const std::string &opt : config.process.vmOptions) {
    if (opt.empty()) {
      std::fprintf(stderr, "Error: --vm requires a value\n");
      return false;
    }
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
      "                                 (see `cache` below to manage it)\n"
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
      "  --record-wasm=<file>           Record every WebAssembly module this "
      "run\n"
      "                                 compiles to <file>, for a later "
      "--bake-wasm\n"
      "  --bake-wasm=<file>             With --build-bundle, bake a "
      "--record-wasm\n"
      "                                 file's Wasm entries into the "
      "container\n"
      "                                 (repeatable)\n"
      "  --dump-wasm=<file>             Print a standalone --record-wasm "
      "file's\n"
      "                                 entries\n"
      "  --vm=<flag>, --vm <flag>       Hermes VM option (repeatable); with\n"
      "                                 --build-bundle, record it in the "
      "container\n"
      "                                 instead of applying it to this run\n"
      "  --allow-vm-options-override    With --build-bundle, let the "
      "container's\n"
      "                                 VM options be overridden at run time\n"
      "  --vm-help                      List the supported VM options\n"
      "  --verbose                      With --build-bundle, narrate the "
      "walk to stderr;\n"
      "                                 with --dump, add per-module edge "
      "counts;\n"
      "                                 with --verify-natives, add expected "
      "and actual\n"
      "                                 hashes; with --dump-bytecode, add "
      "source locations;\n"
      "                                 with --build-exe, narrate the link\n"
      "  --bundle=<file>                Run an application from a bundle file\n"
      "  --dump                         With --bundle, print the container's "
      "tables\n"
      "  --extract-module=<identity>    With --bundle and --out, write one "
      "module's\n"
      "                                 payload to <file>\n"
      "  --out=<file>                   Destination for --extract-module\n"
      "  --verify-natives               With --bundle, check the native "
      "addons\n"
      "                                 shipped beside it (audit, not "
      "enforcement)\n"
      "  --dump-bytecode=<file>         Disassemble a Hermes bytecode file "
      "or a\n"
      "                                 compile cache entry\n"
      "  --build-exe=<output>           Link a standalone executable from "
      "the container\n"
      "                                 named by the positional argument\n"
      "  --kit=<dir>                    With --build-exe, the link kit "
      "directory\n"
      "                                 (default: beside this binary)\n"
      "  --cc=<compiler>                With --build-exe, the C++ driver to "
      "assemble\n"
      "                                 and link with (default: the kit's, "
      "else c++)\n"
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

/// HERMES_NODE_VM_OPTIONS, split the same way bundle_main.cpp splits it for
/// a linked executable -- see splitVmOptionsEnv()'s doc comment for why the
/// splitter is shared rather than reimplemented in each translation unit.
static std::vector<std::string> envVmOptions() {
  return hermes::node_compat::splitVmOptionsEnv(
      std::getenv("HERMES_NODE_VM_OPTIONS"));
}

/// Usage for the `cache` subcommand, printed by `cache --help` and by any
/// malformed invocation of it.
static void printCacheUsage(const char *argv0) {
  std::printf(
      "Usage: %s cache <action> [options]\n"
      "\n"
      "Inspect and manage the on-disk compile cache.\n"
      "\n"
      "Actions:\n"
      "  info      Show the cache root, its configuration, and what each\n"
      "            generation holds\n"
      "  prune     Drop unreachable generations, apply max_wasm_bytes to\n"
      "            what remains, and reap abandoned temp files\n"
      "  clean     Delete the cache\n"
      "\n"
      "Options:\n"
      "  --compile-cache=<dir>  Act on this cache instead of the default\n"
      "  --generation           clean only: delete just the current\n"
      "                         generation, leaving the others and the\n"
      "                         configuration file\n"
      "  --verbose              info only: list generations that hold\n"
      "                         nothing as well\n"
      "\n"
      "The cache directory is --compile-cache=<dir>, else\n"
      "HERMES_NODE_COMPILE_CACHE, else the default root -- the same order a\n"
      "normal run uses.\n"
      "\n"
      "Note: `%s cache` always means this subcommand. To run a script named\n"
      "`cache`, write `%s ./cache`.\n",
      argv0,
      argv0,
      argv0);
}

/// Handles `hermes-node cache <action> [options]`.
///
/// Dispatched from main() BEFORE the ordinary parse loop, and parsing its own
/// arguments rather than sharing that loop. The reason is the invariant the
/// loop rests on: everything after the first positional belongs to the
/// program being run, which is what keeps process.argv.slice(2) meaning what
/// it means under Node. A subcommand is the one thing that has to read the
/// first positional itself, so it sits beside that grammar instead of inside
/// it, and the loop is left exactly as it was.
///
/// Runs no runtime, event loop or napi_env -- like the other tool verbs, and
/// for the same reason: a tool that describes a directory must not fail for
/// reasons belonging to a runtime it never needed.
static int runCacheSubcommand(int argc, char **argv) {
  const char *action = nullptr;
  std::string cacheDir;
  bool verbose = false;
  bool generationOnly = false;

  for (int i = 2; i < argc; ++i) {
    const char *arg = argv[i];
    if (std::strcmp(arg, "--help") == 0 || std::strcmp(arg, "-h") == 0) {
      printCacheUsage(argv[0]);
      return 0;
    } else if (std::strncmp(arg, "--compile-cache=", 16) == 0) {
      cacheDir = arg + 16;
      if (cacheDir.empty()) {
        std::fprintf(stderr, "Error: --compile-cache= requires a directory\n");
        return 1;
      }
    } else if (std::strcmp(arg, "--verbose") == 0) {
      verbose = true;
    } else if (std::strcmp(arg, "--generation") == 0) {
      generationOnly = true;
    } else if (arg[0] == '-') {
      std::fprintf(stderr, "Error: unknown option '%s' for 'cache'\n", arg);
      printCacheUsage(argv[0]);
      return 1;
    } else if (action == nullptr) {
      action = arg;
    } else {
      std::fprintf(
          stderr,
          "Error: 'cache' takes one action, got '%s' and '%s'\n",
          action,
          arg);
      return 1;
    }
  }

  if (action == nullptr) {
    std::fprintf(stderr, "Error: 'cache' requires an action\n");
    printCacheUsage(argv[0]);
    return 1;
  }

  // Same precedence a normal run uses, so the tool and the runtime always
  // act on the same directory.
  std::string root = cacheDir;
  if (root.empty()) {
    if (const char *fromEnv = ::getenv("HERMES_NODE_COMPILE_CACHE"))
      root = fromEnv;
  }
  if (root.empty())
    root = hermes::node_compat::compileCacheDefaultRoot();
  if (root.empty()) {
    std::fprintf(
        stderr,
        "Error: no cache directory: neither --compile-cache, "
        "HERMES_NODE_COMPILE_CACHE, XDG_CACHE_HOME nor HOME is set\n");
    return 1;
  }

  const std::string current =
      hermes::node_compat::compileCacheCurrentGenerationName();

  if (std::strcmp(action, "info") == 0) {
    if (generationOnly) {
      std::fprintf(
          stderr, "Error: --generation applies to 'cache clean', not 'info'\n");
      return 1;
    }
    hermes::node_compat::cacheToolsPrintInfo(
        hermes::node_compat::cacheToolsScan(root, current), verbose, std::cout);
    return 0;
  }
  if (std::strcmp(action, "prune") == 0) {
    if (generationOnly) {
      std::fprintf(
          stderr,
          "Error: --generation applies to 'cache clean', not 'prune'\n");
      return 1;
    }
    hermes::node_compat::CacheInfo info =
        hermes::node_compat::cacheToolsScan(root, current);
    std::cout << "compile cache: " << root << "\n";
    if (!info.exists) {
      std::cout << "  (does not exist)\n";
      return 0;
    }
    hermes::node_compat::cacheToolsPrintChange(
        "prune",
        hermes::node_compat::cacheToolsPrune(root, current, info.config),
        std::cout);
    return 0;
  }
  if (std::strcmp(action, "clean") == 0) {
    std::cout << "compile cache: " << root << "\n";
    hermes::node_compat::CacheCleanScope scope = generationOnly
        ? hermes::node_compat::CacheCleanScope::kCurrentGeneration
        : hermes::node_compat::CacheCleanScope::kAll;
    hermes::node_compat::cacheToolsPrintChange(
        "clean",
        hermes::node_compat::cacheToolsClean(root, current, scope),
        std::cout);
    return 0;
  }

  std::fprintf(stderr, "Error: unknown 'cache' action '%s'\n", action);
  printCacheUsage(argv[0]);
  return 1;
}

int main(int argc, char **argv) {
  // The one subcommand, recognised before anything else parses. Always the
  // subcommand when it is argv[1], never conditional on whether a file of
  // that name exists: a grammar that changed meaning with the contents of
  // the current directory would be a worse surprise than the shadowing it
  // avoided. `./cache` runs a script of that name.
  if (argc > 1 && std::strcmp(argv[1], "cache") == 0)
    return runCacheSubcommand(argc, argv);

  HermesNodeConfig config;
  ToolOptions tools;
  int scriptArgIndex = argc; // no script by default
  int argvStartIndex = argc;
  bool hasEvalCode = false;
  // Tracks whether --record-wasm was named at all, since config.recordWasmPath
  // is a plain string on HermesNodeConfig and so cannot itself distinguish
  // "given an empty value" from "never given" -- the same reason
  // hasEvalCode exists alongside config.evalCode.
  bool recordWasmGiven = false;

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
    } else if (std::strncmp(argv[i], "--bake-wasm=", 12) == 0) {
      config.bakeWasmPaths.push_back(argv[i] + 12);
    } else if (std::strncmp(argv[i], "--record-wasm=", 14) == 0) {
      config.recordWasmPath = argv[i] + 14;
      recordWasmGiven = true;
    } else if (std::strncmp(argv[i], "--vm=", 5) == 0) {
      // Never split on whitespace: -Xperf-prof-dir=<dir> proves a value
      // can legitimately contain a space. One flag per occurrence. An
      // empty value is caught by checkToolOptions(), where every other
      // empty-value flag is caught, so that "--vm=" and "--vm ''" give
      // the same message rather than two different ones.
      config.process.vmOptions.push_back(argv[i] + 5);
    } else if (std::strcmp(argv[i], "--vm") == 0) {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "Error: --vm requires a value\n");
        return 1;
      }
      config.process.vmOptions.push_back(argv[++i]);
    } else if (std::strcmp(argv[i], "--vm-help") == 0) {
      std::printf(
          "Hermes VM options, passed one per --vm=<flag>:\n\n%s",
          hermes::node_compat::vmOptionsHelpText().c_str());
      return 0;
    } else if (std::strcmp(argv[i], "--allow-vm-options-override") == 0) {
      config.allowVmOptionsOverride = true;
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
    } else if (std::strncmp(argv[i], "--dump-wasm=", 12) == 0) {
      tools.dumpWasm = argv[i] + 12;
    } else if (std::strncmp(argv[i], "--out=", 6) == 0) {
      tools.out = argv[i] + 6;
    } else if (std::strcmp(argv[i], "--verify-natives") == 0) {
      tools.verifyNatives = true;
    } else if (std::strncmp(argv[i], "--build-exe=", 12) == 0) {
      tools.buildExe = argv[i] + 12;
    } else if (std::strncmp(argv[i], "--cc=", 5) == 0) {
      tools.cc = argv[i] + 5;
    } else if (std::strncmp(argv[i], "--kit=", 6) == 0) {
      tools.kitDir = argv[i] + 6;
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

  // The positional argument, read directly from argv rather than from
  // config.scriptPath: that field is not assigned until after the read-only
  // verbs are dispatched (see below, where it is set from this same
  // scriptArgIndex), and --build-exe's container -- the positional argument
  // in its invocation -- is validated and consumed before that point.
  const std::string containerPath = (!hasEvalCode && scriptArgIndex < argc)
      ? std::string(argv[scriptArgIndex])
      : std::string();

  // The read-only verbs and the flags that serve them, as one block, before
  // the refusals that belong to running a program: a verb that never starts
  // the program should not be explained in terms of the debugger.
  if (!checkToolOptions(
          config, tools, hasEvalCode, recordWasmGiven, containerPath))
    return 1;

  // Everything after the first positional belongs to the program being run,
  // so the parse loop stops there -- ordinary CLI convention, and every
  // other verb takes its container through --bundle= and never meets it.
  // --build-exe is the one whose input IS the positional, and it runs no
  // program, so a flag typed after the container is not the program's
  // either: it is simply dropped. `--build-exe=out app.hbb --verbose`
  // narrated nothing and `... --kit=/other` used the default kit, both in
  // silence. Refuse instead, naming the flag. Checked here, after
  // checkToolOptions(), so that the flag-conflict messages keep their
  // precedence, and outside the parse loop for the reason that whole matrix
  // is: flag order must not decide which error comes out.
  if (tools.buildExe.has_value() && scriptArgIndex < argc) {
    for (int i = scriptArgIndex + 1; i < argc; ++i) {
      if (argv[i][0] == '-') {
        std::fprintf(
            stderr,
            "Error: '%s' appears after the bundle file '%s'; options must "
            "come before it.\n",
            argv[i],
            argv[scriptArgIndex]);
        return 1;
      }
    }
  }

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
  if (runToolVerb(config, tools, containerPath, toolExitCode))
    return toolExitCode;

  // A container's baked options decide how the runtime is built, but the
  // run path opens the container only after the runtime exists (runBundle
  // takes a napi_env), so this reads the container a second time, purely
  // for its VM options, before anything is built. The final list is the
  // container's options first, then HERMES_NODE_VM_OPTIONS, then --vm=
  // from this command line -- later occurrences of a repeated flag win
  // (buildVmRuntimeConfig dedupes, keeping the last), so appending in this
  // order is precedence: no merge logic here, just the order things are
  // appended in.
  //
  // The same order applies with no container in play, minus the first
  // step: HERMES_NODE_VM_OPTIONS, then --vm=. It applied to a --bundle run
  // only until 2026-09-02, on the argument that a plain script already has
  // --vm= on its own command line -- which made
  // `HERMES_NODE_VM_OPTIONS=... hermes-node app.js` a silent no-op on a VM
  // setting, exactly the failure shape the comments in this file invoke
  // three times over as the reason for refusing rather than ignoring.
  //
  // --build-bundle is deliberately not included: there, --vm= is *recorded*
  // into the container rather than applied (buildBundle() reads
  // config.process.vmOptions), so folding the environment in would bake a
  // build machine's ambient variable into a shipped artifact.
  if (!config.bundlePath.empty()) {
    hermes::node_compat::BundleVmOptions bundleVm;
    std::string error;
    if (!hermes::node_compat::readBundleVmOptions(
            config.bundlePath, &bundleVm, &error)) {
      std::fprintf(stderr, "error: %s\n", error.c_str());
      return 1;
    }
    std::vector<std::string> runtimeVm = envVmOptions();
    const bool hasCliVm = !config.process.vmOptions.empty();
    // Locked is the default: an override attempt is refused outright
    // rather than quietly doing nothing, naming which source tried it, so
    // this never becomes the swallow-and-continue pattern the rest of this
    // codebase has spent several rounds removing.
    if (!bundleVm.allowOverride && (hasCliVm || !runtimeVm.empty())) {
      std::fprintf(
          stderr,
          "Error: this bundle's VM options are locked and cannot be "
          "overridden.\n"
          "       %s\n"
          "       Rebuild with: --build-bundle --allow-vm-options-override\n",
          hasCliVm ? "--vm was given on the command line."
                   : "HERMES_NODE_VM_OPTIONS is set in the environment.");
      return 1;
    }
    std::vector<std::string> merged = bundleVm.options;
    merged.insert(merged.end(), runtimeVm.begin(), runtimeVm.end());
    merged.insert(
        merged.end(),
        config.process.vmOptions.begin(),
        config.process.vmOptions.end());
    config.process.vmOptions = std::move(merged);
  } else if (config.buildBundlePath.empty()) {
    // A plain script, -e, or the REPL. The environment is applied first so
    // that an explicit --vm= on the command line still wins, which is the
    // same precedence a container run gives the two.
    std::vector<std::string> merged = envVmOptions();
    if (!merged.empty()) {
      merged.insert(
          merged.end(),
          config.process.vmOptions.begin(),
          config.process.vmOptions.end());
      config.process.vmOptions = std::move(merged);
    }
  }

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
