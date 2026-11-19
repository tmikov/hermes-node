/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ui_host.h"

#include <hermes/node-compat/runtime/hermes_node_runtime.h>
#include <hermes/node-compat/version.h>

// Both unconditionally. `hermes-node:ui` is registered in EVERY build of
// this binary -- see the registration in main() for why -- and which
// implementation answers for it is decided at link time: ui_module.cpp
// where a GPU backend was built, ui_module_nowindow.cpp where none was.
#include "ui_module.h"

#include <hermes/node-compat/runtime/embedder_module_prefix.h>

#ifdef HERMES_NODE_UI_HAVE_WINDOW
#include <hermes/node-compat/process/node_process.h>
#endif

#ifdef HERMES_NODE_UI_HAVE_WINDOW
#include <sokol_log.h>
#endif

#include <cstdio>
#include <cstring>
#include <utility>

using hermes::node_compat::HermesNodeConfig;
using hermes::node_compat::UiHost;

namespace {

void printUsage(const char *argv0) {
  std::fprintf(
      stderr,
      "Usage: %s [options] <script.js> [args...]\n"
      "       %s [options] --bundle=<file> [args...]\n"
      "\n"
      "hermes-node-ui runs the JS runtime on its own thread and keeps this\n"
      "process's main thread free for the window -- see\n"
      "docs/superpowers/specs/2026-09-18-ui-compositor-design.md for the\n"
      "design.\n"
#ifndef HERMES_NODE_UI_HAVE_WINDOW
      // Said here as well as by the rejection createWindow() produces,
      // because --help is where someone looks after being refused. String
      // literals concatenate across the conditional; the preprocessor has
      // already run by then.
      "\n"
      "This build has no window support: no GPU backend was compiled for\n"
      "this platform, so createWindow() rejects. A script that never asks\n"
      "for a window runs to completion exactly as it would under plain\n"
      "hermes-node.\n"
#endif
      "\n"
      "Options:\n"
      "  --bundle=<file>   Run an AOT bundle instead of a script file\n"
      "  -v, --version     Print the hermes-node-ui version and exit\n"
      "  -h, --help        Show this help\n",
      argv0,
      argv0);
}

} // namespace

// Deliberately not hermes-node's own parser (tools/hermes-node/hermes-node.cpp,
// main(), currently at line 636): that one understands two dozen flags this
// v1 binary does not implement yet -- --inspect, --build-bundle, --vm=, -e,
// the REPL, the five read-only diagnostic verbs, --build-exe. Reusing it
// here would mean either implementing all of that today or silently
// accepting flags that do nothing, which is the swallow-and-continue
// pattern this codebase removes on sight. So this is a second, much
// smaller parser that understands exactly what v1 needs -- a script path,
// --bundle=, and --version -- and refuses everything else by name.
int main(int argc, char **argv) {
  HermesNodeConfig config;
  int scriptArgIndex = argc; // no script by default
  int argvStartIndex = argc;
  bool bundleFlagGiven = false;

  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--version") == 0 ||
        std::strcmp(argv[i], "-v") == 0) {
      std::printf("hermes-node-ui %s\n", HERMES_NODE_VERSION_STRING);
#ifdef HERMES_NODE_UI_HAVE_WINDOW
      // A build fact a test needs before it can open a window:
      // test/lit.cfg's `sokol-validation` feature reads this line, since
      // only a validating build turns an addon's malformed pass into a
      // panic.
      std::printf(
          "sokol_gfx validation: %s\n",
          hermes::node_compat::producerSokolValidates() ? "on" : "off");
#endif
      return 0;
    } else if (
        std::strcmp(argv[i], "--help") == 0 ||
        std::strcmp(argv[i], "-h") == 0) {
      printUsage(argv[0]);
      return 0;
    } else if (std::strncmp(argv[i], "--bundle=", 9) == 0) {
      bundleFlagGiven = true;
      config.bundlePath = argv[i] + 9;
    } else if (std::strcmp(argv[i], "--") == 0) {
      argvStartIndex = i + 1;
      // A positional after "--" is the script to run -- unless --bundle=
      // already supplied the program, in which case it is one of the
      // bundled program's own arguments instead (process.argv[2:]), not a
      // second entry point. Mirrors hermes-node.cpp's own
      // --bundle=<f> arg contract: `hermes-node --bundle=app.hbb -- extra`
      // passes "extra" through rather than refusing it.
      if (config.bundlePath.empty() && i + 1 < argc)
        scriptArgIndex = i + 1;
      break;
    } else if (argv[i][0] == '-') {
      std::fprintf(stderr, "Error: unknown option '%s'\n", argv[i]);
      return 1;
    } else {
      // The first non-flag argument. Same reasoning as the "--" case just
      // above: with --bundle= already given, this is the bundled program's
      // own first argument, not a script to run.
      argvStartIndex = i;
      if (config.bundlePath.empty())
        scriptArgIndex = i;
      break;
    }
  }

  // An empty value ("--bundle=") is still a request -- for an empty path,
  // which is not the same thing as --bundle never having been given at
  // all. Checked before the "no script provided" fallback below, which
  // would otherwise report the wrong problem: a flag whose value was left
  // out reads as "no script provided", naming neither the flag nor what is
  // wrong with it, exactly the diagnostic hermes-node.cpp's own
  // checkToolOptions() avoids for --dump-bytecode=, --out=, --build-exe=,
  // --kit= and --cc=.
  if (bundleFlagGiven && config.bundlePath.empty()) {
    std::fprintf(stderr, "Error: --bundle requires a file path.\n");
    return 1;
  }

  if (config.bundlePath.empty() && scriptArgIndex >= argc) {
    std::fprintf(stderr, "Error: no script provided.\n");
    printUsage(argv[0]);
    return 1;
  }

  // process.argv: [binary, script-or-bundle-path, ...rest]. Mirrors
  // hermes-node.cpp's own construction, minus everything this parser
  // does not accept.
  config.argv.push_back(argv[0]);
  if (!config.bundlePath.empty())
    config.argv.push_back(config.bundlePath);
  for (int i = argvStartIndex; i < argc; ++i)
    config.argv.push_back(argv[i]);

  if (config.bundlePath.empty())
    config.scriptPath = argv[scriptArgIndex];

  // **Registered in every build, window support or not.** This used to be
  // guarded by HERMES_NODE_UI_HAVE_WINDOW, which
  // tools/hermes-node-ui/CMakeLists.txt sets only where a GPU backend was
  // built -- so on macOS with the option on, which is a configuration CI
  // builds today, require('hermes-node:ui') failed with MODULE_NOT_FOUND: a
  // script asking for the one feature this binary exists for got the error
  // it would get for a typo. The analogous path in UiHost::run() prints
  // "hermes-node-ui has no window support yet", and the two must agree, so
  // the module is always present and its createWindow is what refuses --
  // see ui_module_nowindow.cpp.
  //
  // Publish `hermes-node:ui` before the runtime starts, because that is the
  // only time it can be published: HermesNodeConfig::embedderModules is read
  // by the module loader during bootstrap, and a module registered after
  // runHermesNode() has the config would be one nothing can ever require.
  //
  // registerEmbedderModule() validates the name and refuses a duplicate,
  // printing why; returning 1 here rather than carrying on is the point of
  // it returning bool at all. A UI binary whose UI module failed to
  // register would otherwise start, run the script, and fail at
  // require('hermes-node:ui') with a module-not-found error naming nothing
  // that went wrong.
  if (!hermes::node_compat::registerEmbedderModule(
          config,
          {std::string(hermes::node_compat::kEmbedderModulePrefix) + "ui",
           hermes::node_compat::initUiModule})) {
    return 1;
  }

  UiHost host(std::move(config));

#ifdef HERMES_NODE_UI_HAVE_WINDOW
  // Both before run(), and therefore before the JS thread exists. That
  // ordering is what makes the module's host pointer safe as a plain
  // process-global: it is written once, by this thread, strictly before the
  // only other thread that reads it is created.
  hermes::node_compat::setUiModuleHost(&host);
  host.setWindowRunner(hermes::node_compat::uiRunPlatformLoop);
  // The window enters CLOSING before any 'exit' listener runs, on every
  // path that emits 'exit'. Set here for the same ordering reason as the
  // host pointer: setProcessExitHooks() is not synchronized, and the JS
  // thread that reads the hooks does not exist yet. The no-window build
  // sets nothing, and its exits are exactly plain hermes-node's.
  hermes::node_compat::ProcessExitHooks exitHooks;
  exitHooks.beforeExitListeners = hermes::node_compat::uiBeforeExitListeners;
  hermes::node_compat::setProcessExitHooks(exitHooks);
#endif

  return host.run();
}
