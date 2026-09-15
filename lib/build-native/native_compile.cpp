/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <hermes/node-compat/build-native/build_native.h>

#include <hermes/node-compat/bundle/cjs_wrapper.h>

namespace hermes {
namespace node_compat {

namespace {

const char *shermesOptFlag(OptLevel opt) {
  switch (opt) {
    case OptLevel::O0:
      return "-O0";
    case OptLevel::O1:
      return "-Og";
    // shermes has four levels and -O is its highest; O2 and O3 differ only
    // on the cc side.
    case OptLevel::O2:
    case OptLevel::O3:
      return "-O";
    case OptLevel::Os:
      return "-Os";
  }
  return "-O";
}

const char *ccOptFlag(OptLevel opt) {
  switch (opt) {
    case OptLevel::O0:
      return "-O0";
    case OptLevel::O1:
      return "-O1";
    case OptLevel::O2:
      return "-O2";
    case OptLevel::O3:
      return "-O3";
    case OptLevel::Os:
      return "-Os";
  }
  return "-O3";
}

} // namespace

std::vector<std::string> buildShermesCommand(
    const std::string &shermesPath,
    const std::string &stagedPath,
    const std::string &outCPath,
    const std::string &sourceName,
    const std::string &unitName,
    bool typeScript,
    OptLevel opt) {
  std::vector<std::string> argv{
      shermesPath,
      "-emit-c",
      // Source locations, which is what puts identity:line:column into a
      // stack trace. At -g0 a native frame prints "(native)" with no
      // location at all.
      "-g2",
      shermesOptFlag(opt),
      // A CommonJS module reports primordials, internalBinding and process
      // as undeclared globals -- three in the first hundred lines of
      // libjs-node/net.js. The bytecode path prints none of them either.
      "-w",
      // Parity, not caution: shermes defaults to -sm-comment=file, so a
      // published package's //# sourceMappingURL= would make it load a map
      // and rewrite the names in the location table, while the bytecode
      // path passes sourceMap="" and ignores the comment.
      "-sm-comment=off",
  };

  // See JSLanguageFlags in cjs_wrapper.h. Absent, these are silent: block
  // scoping off makes every let-in-loop closure capture the wrong binding.
  if (kJSLanguageFlags.es6BlockScoping)
    argv.push_back("-Xes6-block-scoping");
  if (kJSLanguageFlags.asyncGenerators)
    argv.push_back("-Xasync-generators");
  // kJSLanguageFlags.generators has no shermes flag: generators are on
  // unconditionally, in both compilers.
  //
  // Both flags, not -transform-ts alone: shermes.cpp defaults ParseFlow to
  // true whenever neither -parse-flow nor -parse-ts was named explicitly,
  // and that default runs BEFORE the "-transform-ts implies -parse-ts"
  // logic, so -transform-ts alone leaves ParseFlow on too. parseDeclaration
  // then tries Flow's own parser first (it is checked before TS's), and for
  // any TS construct Flow also has a keyword for -- interface and enum, at
  // minimum -- it builds Flow's AST node instead of TS's, which the TS
  // stripping pass that -transform-ts enables does not know how to strip
  // and IRGen rejects with "invalid statement encountered". Naming
  // -parse-ts explicitly keeps ParseFlow off, matching the bytecode
  // compiler's BCProviderFromSrc.cpp, which sets both unconditionally and
  // never goes through shermes's command-line defaulting at all. Found via
  // test/build-native-parity.js's TS row, which failed on `interface`
  // before this fix even with -transform-ts present.
  if (typeScript) {
    argv.push_back("-parse-ts");
    argv.push_back("-transform-ts");
  }

  argv.push_back("-source-name=" + sourceName);
  argv.push_back("-exported-unit=" + unitName);
  argv.push_back("-o");
  argv.push_back(outCPath);
  argv.push_back(stagedPath);
  return argv;
}

std::vector<std::string> buildCompileCommand(
    const KitManifest &manifest,
    const std::string &driver,
    bool driverIsClang,
    const std::string &cPath,
    const std::string &objPath,
    OptLevel opt) {
  std::vector<std::string> argv{driver};
  // The same suppression buildAssembleCommand uses, and for the same
  // reason: the whole driver-flag list is forwarded below, so link-only
  // flags reach a compile that has no use for them. Clang-only; GCC
  // rejects it.
  if (driverIsClang)
    argv.push_back("-Qunused-arguments");
  // Before the input, because -x applies to the files that follow it. The
  // manifest's driver is the C++ LINK driver, which would otherwise compile
  // this .c as C++ and fail.
  argv.push_back("-x");
  argv.push_back("c");
  argv.push_back("-std=gnu11");
  argv.push_back(ccOptFlag(opt));
  argv.push_back("-c");
  // The driver flags SELECT A TARGET: -arch on a universal macOS kit, a
  // --target or -isysroot on a cross-compiling one. Compile without them
  // and the object is host-only, and the link cannot resolve it for the
  // slice it was not built for. The whole list is forwarded rather than a
  // hand-picked subset, for the reason buildAssembleCommand gives: a
  // hand-maintained list of "which flags select a target, per driver" is
  // the thing the manifest exists to abolish.
  for (const std::string &flag : manifest.driverFlags)
    argv.push_back(flag);
  for (const std::string &flag : manifest.ccFlags)
    argv.push_back(flag);
  argv.push_back(cPath);
  argv.push_back("-o");
  argv.push_back(objPath);
  return argv;
}

bool linkResponseFile(
    const std::vector<std::string> &objects,
    std::string *out,
    std::string *error) {
  out->clear();
  for (const std::string &path : objects) {
    // Response-file syntax is neither shell nor assembler: GNU ld and ld64
    // split on whitespace and both honour " quoting and \ escaping. So a
    // path is written quoted, which makes a space harmless -- and it has to
    // be, since checkIncbinPath() permits spaces and a macOS path routinely
    // has them. What it does not permit is refused here too, by calling it
    // directly rather than keeping a second copy of the same four-character
    // check: the two checks were already required to agree about what a
    // usable path is, and a shared function cannot drift from itself.
    if (std::string bad = checkIncbinPath(path); !bad.empty()) {
      *error = "cannot put this path in a linker response file (contains " +
          bad + "): " + path;
      return false;
    }
    out->push_back('"');
    out->append(path);
    out->append("\"\n");
  }
  return true;
}

} // namespace node_compat
} // namespace hermes
