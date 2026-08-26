/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// The producer for --build-exe: validate a container, generate one object
// carrying its bytes, and link that object plus the kit's app entry
// against the kit's archives. No Hermes runtime is involved -- the
// container arrives already compiled -- which is what keeps this library
// free of the VM.

#include <hermes/node-compat/build-exe/build_exe.h>

#include <hermes/node-compat/bundle/atomic_write.h>
#include <hermes/node-compat/bundle/bundle_generation.h>
#include <hermes/node-compat/bundle/bundle_reader.h>
#include <hermes/node-compat/bundle/mapped_file.h>
#include <hermes/node-compat/version.h>

#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

extern char **environ;

namespace hermes {
namespace node_compat {

namespace fs = std::filesystem;

namespace {

/// The app entry object the kit carries, compiled from
/// tools/hermes-node/bundle_main.cpp. A loose .o rather than a member of
/// the kit archive, deliberately: main() must be linked unconditionally,
/// and a main() living in an archive is pulled only if something already
/// referenced it.
constexpr const char *kEntryObjectName = "hermes-node-bundle-main.o";

/// The last driver candidate: the POSIX-conventional name for "the C++
/// compiler on this system". A Debian-ish Linux points it at g++ through
/// the alternatives system; macOS points it at clang++. Both were measured
/// to assemble the generated payload and link the result correctly, which
/// is what makes this a usable fallback rather than a guess.
constexpr const char *kPortableDriverName = "c++";

/// Characters a POSIX shell leaves alone, so an argument made only of these
/// can be printed bare. Everything else -- spaces above all, but also the
/// globbing, redirection and expansion characters -- gets quoted.
bool needsShellQuoting(const std::string &arg) {
  if (arg.empty())
    return true; // an empty argument disappears if it is not quoted
  for (char c : arg) {
    bool safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9') || c == '_' || c == '@' || c == '%' ||
        c == '+' || c == '=' || c == ':' || c == ',' || c == '.' || c == '/' ||
        c == '-';
    if (!safe)
      return true;
  }
  return false;
}

/// Why a candidate was offered, in the words the user needs to act on it.
/// Printed for every candidate when none works, and for the winner under
/// --verbose, because "which compiler did it actually run" is the first
/// question when a link fails on a machine that did not cut the kit.
const char *driverSourceName(const DriverCandidate &candidate) {
  switch (candidate.source) {
    case DriverSource::Override:
      return "--cc";
    case DriverSource::ManifestPath:
      return "recorded in the kit";
    case DriverSource::ManifestName:
      return "the kit's compiler, from PATH";
    case DriverSource::Fallback:
      return "portable fallback";
  }
  return "unknown";
}

/// Runs `<driver> --version` and returns what it printed, or nullopt if it
/// could not be run to a clean exit.
///
/// This is the single probe behind both questions asked of a candidate
/// driver: whether it can be executed at all (which is what "usable" means
/// -- an absolute path that no longer exists, or a name not on PATH, fails
/// to spawn) and whether it is Clang. One subprocess answers both, and in
/// the ordinary case the first candidate answers them immediately.
///
/// The child's output is captured rather than inherited, unlike
/// runCommand's: this is a probe, and a candidate that is merely absent
/// must not print anything. Real diagnostics still reach the user, from
/// the assemble and link that follow.
std::optional<std::string> captureDriverVersion(const std::string &driver) {
  int fds[2];
  if (pipe(fds) != 0)
    return std::nullopt;

  posix_spawn_file_actions_t actions;
  if (posix_spawn_file_actions_init(&actions) != 0) {
    close(fds[0]);
    close(fds[1]);
    return std::nullopt;
  }
  // Both streams into the pipe: a driver is free to print its banner on
  // either, and for a substring test the interleaving does not matter.
  posix_spawn_file_actions_addclose(&actions, fds[0]);
  posix_spawn_file_actions_adddup2(&actions, fds[1], STDOUT_FILENO);
  posix_spawn_file_actions_adddup2(&actions, fds[1], STDERR_FILENO);
  posix_spawn_file_actions_addclose(&actions, fds[1]);

  std::string versionFlag = "--version";
  char *raw[] = {
      const_cast<char *>(driver.c_str()),
      const_cast<char *>(versionFlag.c_str()),
      nullptr};

  pid_t pid = 0;
  int rc = posix_spawnp(&pid, raw[0], &actions, nullptr, raw, environ);
  posix_spawn_file_actions_destroy(&actions);
  close(fds[1]);
  if (rc != 0) {
    close(fds[0]);
    return std::nullopt;
  }

  // Drain before waiting: a driver whose banner outgrows the pipe buffer
  // would otherwise block forever writing while we block waiting.
  std::string output;
  char buf[4096];
  for (;;) {
    ssize_t n = read(fds[0], buf, sizeof(buf));
    if (n > 0) {
      output.append(buf, static_cast<size_t>(n));
      continue;
    }
    if (n == 0 || errno != EINTR)
      break;
  }
  close(fds[0]);

  int status = 0;
  while (waitpid(pid, &status, 0) < 0) {
    if (errno != EINTR)
      return std::nullopt;
  }
  // Deliberately NOT "exited 0". The question here is whether the driver
  // can be run at all, and --version's exit status does not answer it:
  // requiring success would reject a working compiler that happens to
  // report its version oddly, and fall back to a different one behind the
  // user's back. What does mean "not runnable" is 127, the conventional
  // command-not-found status, which is how exec failure reaches us on
  // platforms where posix_spawnp reports it through the child rather than
  // through its own return value (glibc returns ENOENT above instead).
  if (WIFEXITED(status) && WEXITSTATUS(status) == 127)
    return std::nullopt;
  return output;
}

/// Runs \p argv to completion, reporting anything but a clean exit 0 on
/// \p err together with the command itself.
///
/// The child inherits this process's stdout and stderr, so the compiler's
/// and the linker's own diagnostics go to fd 2 and NOT to \p err -- see
/// the note on buildExecutable() in the header for why that is deliberate.
/// What goes to \p err is the summary line and the command to rerun, which
/// formatCommandLine() renders so it can be pasted into a shell.
///
/// posix_spawnp rather than system(): the container's path is interpolated
/// into a quoted assembler string in the file this hands the assembler,
/// and a shell in the middle would re-open exactly the quoting question
/// checkIncbinPath() exists to close -- with the shell's own metacharacter
/// set on top of the assembler's. An argv never has that problem, because
/// nothing re-parses it.
bool runCommand(const std::vector<std::string> &argv, std::ostream &err) {
  std::vector<char *> raw;
  raw.reserve(argv.size() + 1);
  for (const std::string &arg : argv)
    raw.push_back(const_cast<char *>(arg.c_str()));
  raw.push_back(nullptr);

  pid_t pid = 0;
  int rc = posix_spawnp(&pid, raw[0], nullptr, nullptr, raw.data(), environ);
  if (rc != 0) {
    err << "error: cannot run " << argv[0] << ": " << std::strerror(rc) << "\n";
    err << "  command: " << formatCommandLine(argv) << "\n";
    return false;
  }

  int status = 0;
  while (waitpid(pid, &status, 0) < 0) {
    if (errno != EINTR) {
      err << "error: waiting for " << argv[0] << ": " << std::strerror(errno)
          << "\n";
      err << "  command: " << formatCommandLine(argv) << "\n";
      return false;
    }
  }

  if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
    return true;

  if (WIFSIGNALED(status))
    err << "error: " << argv[0] << " was killed by signal " << WTERMSIG(status)
        << "\n";
  else
    err << "error: " << argv[0] << " failed with exit status "
        << WEXITSTATUS(status) << "\n";
  err << "  command: " << formatCommandLine(argv) << "\n";
  return false;
}

/// Why \p path cannot be named inside the assembler's quoted string, or ""
/// if it can.
///
/// GAS processes C-style escapes inside a quoted string, so a backslash is
/// as unrepresentable as a quote is: `.incbin "a\tb"` names a path with a
/// tab in it. Rejecting all three with the reason is the honest answer --
/// escaping them instead would mean maintaining a second model of the
/// assembler's string lexer, and getting it wrong writes the wrong file
/// into the executable rather than failing.
std::string checkIncbinPath(const std::string &path) {
  for (char c : path) {
    if (c == '"')
      return "a double quote";
    if (c == '\\')
      return "a backslash";
    if (c == '\n' || c == '\r')
      return "a newline";
  }
  return "";
}

/// Removes the temporaries when the call leaves, however it leaves. A
/// failed link must not leave a multi-megabyte .s and .o beside the
/// output the user did not get.
struct TempFiles {
  std::vector<std::string> paths;
  ~TempFiles() {
    std::error_code ec;
    for (const std::string &p : paths)
      fs::remove(p, ec);
  }
};

} // namespace

std::string payloadAssembly(
    const std::string &bundlePath,
    ObjectFormat format) {
  std::ostringstream os;

  // .incbin rather than a generated C array: a multi-megabyte initializer
  // would explode compile time for nothing. .p2align 4 is 16 bytes,
  // comfortably above the format's kBundlePayloadAlign of 8; payload
  // offsets inside the container are relative to its start, so aligning
  // the start is what lets bytecode be executed in place. It is not
  // decoration -- openEmbeddedBundle() refuses to run a payload whose base
  // is misaligned, so this directive is what makes the produced binary
  // work at all.
  //
  // Both spellings are compiled on every host (see ObjectFormat in the
  // header): the branch for the platform you are not on is the one a typo
  // survives in, and an #ifdef here would keep it out of the test binary
  // as well as out of this one.
  if (format == ObjectFormat::MachO) {
    os << "\t.section __DATA,__const\n"
       << "\t.p2align 4\n"
       << "\t.globl _hermesNodeBundleStart\n"
       << "_hermesNodeBundleStart:\n"
       << "\t.incbin \"" << bundlePath << "\"\n"
       << "\t.globl _hermesNodeBundleEnd\n"
       << "_hermesNodeBundleEnd:\n";
  } else {
    os << "\t.section .rodata\n"
       << "\t.p2align 4\n"
       << "\t.globl hermesNodeBundleStart\n"
       << "hermesNodeBundleStart:\n"
       << "\t.incbin \"" << bundlePath << "\"\n"
       << "\t.globl hermesNodeBundleEnd\n"
       << "hermesNodeBundleEnd:\n"
       // Without this, the linker cannot tell that an object assembled
       // from hand-written source needs no executable stack, so it
       // conservatively marks the whole program's GNU_STACK segment RWE --
       // measured with readelf, and GNU ld additionally warns that the
       // behavior is deprecated. Every executable this feature produces
       // would carry it. A security regression introduced by our own code
       // generator is not something to leave to whoever reads the linker's
       // warnings. Mach-O has no equivalent: its stack is non-executable
       // unless the load command says otherwise.
       << "\t.section .note.GNU-stack,\"\",@progbits\n";
  }

  return os.str();
}

std::vector<DriverCandidate> driverCandidates(
    const std::string &ccOverride,
    const std::string &manifestCc) {
  std::vector<DriverCandidate> candidates;
  auto add = [&candidates](std::string driver, DriverSource source) {
    if (driver.empty())
      return;
    for (const DriverCandidate &existing : candidates)
      if (existing.driver == driver)
        return;
    candidates.push_back({std::move(driver), source});
  };
  // An explicit --cc replaces the list rather than heading it. If the
  // compiler the user named cannot be run, linking with a different one
  // and saying nothing would be the worst outcome available: the
  // executable gets built, by something else, silently.
  if (!ccOverride.empty()) {
    add(ccOverride, DriverSource::Override);
    return candidates;
  }
  add(manifestCc, DriverSource::ManifestPath);
  if (!manifestCc.empty())
    add(fs::path(manifestCc).filename().string(), DriverSource::ManifestName);
  add(kPortableDriverName, DriverSource::Fallback);
  return candidates;
}

std::optional<DriverCandidate> resolveDriver(
    const std::string &ccOverride,
    const std::string &manifestCc,
    const std::function<bool(const DriverCandidate &)> &usable) {
  for (const DriverCandidate &candidate :
       driverCandidates(ccOverride, manifestCc))
    if (usable(candidate))
      return candidate;
  return std::nullopt;
}

std::string formatCommandLine(const std::vector<std::string> &argv) {
  std::string joined;
  for (const std::string &arg : argv) {
    if (!joined.empty())
      joined += ' ';
    if (!needsShellQuoting(arg)) {
      joined += arg;
      continue;
    }
    // Single quotes, because inside them a shell expands nothing at all.
    // The one character they cannot carry is a single quote, which is
    // closed, escaped and reopened in the usual way.
    joined += '\'';
    for (char c : arg) {
      if (c == '\'')
        joined += "'\\''";
      else
        joined += c;
    }
    joined += '\'';
  }
  return joined;
}

bool versionOutputIsClang(const std::string &versionOutput) {
  return versionOutput.find("clang") != std::string::npos;
}

bool recordedDriverWasRejected(DriverSource source) {
  return source == DriverSource::ManifestName ||
      source == DriverSource::Fallback;
}

std::vector<std::string> buildAssembleCommand(
    const KitManifest &manifest,
    const std::string &driver,
    bool driverIsClang,
    const std::string &asmPath,
    const std::string &objPath) {
  std::vector<std::string> cmd;
  cmd.push_back(driver);
  // Forwarding the link's driver flags to a compile means forwarding flags
  // a compile has no use for, and clang says so once per flag. Suppress
  // exactly that, and nothing else: a real assembler diagnostic about the
  // file we generated still has to be visible. See the header for why the
  // whole list is forwarded rather than a target-selecting subset.
  //
  // Only for Clang: the flag is a Clang spelling, and g++ rejects an
  // unrecognized option outright rather than ignoring it, so adding it
  // unconditionally is what confined this feature to Clang.
  if (driverIsClang)
    cmd.push_back("-Qunused-arguments");
  for (const std::string &flag : manifest.driverFlags)
    cmd.push_back(flag);
  cmd.push_back("-c");
  cmd.push_back(asmPath);
  cmd.push_back("-o");
  cmd.push_back(objPath);
  return cmd;
}

std::vector<std::string> buildLinkCommand(
    const KitManifest &manifest,
    const std::string &driver,
    const std::string &blobObject,
    const std::string &outPath) {
  std::vector<std::string> cmd;
  cmd.push_back(driver);
  // The driver's own flags first: some of them (a target triple, a
  // sysroot, an -arch) decide how everything after them is interpreted.
  for (const std::string &flag : manifest.driverFlags)
    cmd.push_back(flag);
  // Both objects before every archive. An archive contributes only the
  // members that resolve something already undefined, so an archive listed
  // before the object that needs it contributes nothing at all.
  cmd.push_back(blobObject);
  cmd.push_back((fs::path(manifest.kitDir) / kEntryObjectName).string());
  // The manifest's order is the link line CMake generated, with system
  // libraries already hoisted past every archive by make-kit.py.
  for (const std::string &arg : manifest.linkArgs)
    cmd.push_back(arg);
  cmd.push_back("-o");
  cmd.push_back(outPath);
  return cmd;
}

int buildExecutable(
    const std::string &bundlePath,
    const std::string &outPath,
    const std::string &kitDir,
    const std::string &ccOverride,
    bool verbose,
    std::ostream &out,
    std::ostream &err) {
  // 1. The container has to exist and be mappable before anything else is
  // worth doing.
  std::string error;
  std::optional<MappedFile> file = MappedFile::open(bundlePath, &error);
  if (!file) {
    err << "error: " << error << "\n";
    return 1;
  }

  // 2. ... and be a container this binary would run. Building an
  // executable around bytecode this build cannot execute would produce a
  // binary that fails at startup, with the mismatch reported by a copy of
  // the runtime rather than by the tool that had the container in hand.
  std::optional<BundleReader> reader = BundleReader::open(
      file->data(), file->size(), bundleGenerationTag(), &error);
  if (!reader) {
    err << "error: " << bundlePath << ": " << error << "\n";
    return 1;
  }

  // 3. The kit. Its absence is the common case for a binary installed
  // without one, so say where it looked and how to point elsewhere.
  std::optional<KitManifest> manifest = readKitManifest(kitDir, &error);
  if (!manifest) {
    err << "error: " << error << "\n";
    err << "note: --build-exe needs a link kit; --kit=<dir> names one.\n";
    return 1;
  }

  // 4. A kit cut from a different build than this producer links archives
  // whose generation tag, bytecode version and runtime this binary has no
  // reason to match. The generation check above already covers the
  // container; this covers the halves of the *link*.
  if (manifest->version != HERMES_NODE_VERSION_STRING) {
    err << "error: kit " << manifest->kitDir << " was cut from hermes-node "
        << manifest->version << ", but this is hermes-node "
        << HERMES_NODE_VERSION_STRING << "\n";
    // The remedy is not guessable: hermes-node-kit is EXCLUDE_FROM_ALL (the
    // merged archive is a full copy of every archive in the closure, too
    // much to add to every incremental build), so an ordinary
    // `cmake --build` did not rebuild the kit and the two versions will
    // usually differ only in the git-describe suffix.
    err << "note: re-cut the kit with: cmake --build <build dir> --target "
           "hermes-node-kit\n";
    return 1;
  }

  // The path goes into a quoted assembler string, so it must be absolute
  // (the assembler resolves a relative .incbin against its own include
  // path, not against our working directory) and free of what the
  // assembler's string lexer would reinterpret.
  std::error_code ec;
  fs::path absBundle = fs::canonical(bundlePath, ec);
  if (ec) {
    err << "error: cannot resolve " << bundlePath << ": " << ec.message()
        << "\n";
    return 1;
  }
  std::string absBundleStr = absBundle.string();
  if (std::string bad = checkIncbinPath(absBundleStr); !bad.empty()) {
    err << "error: the bundle's path contains " << bad
        << ", which cannot be named in the generated assembly: " << absBundleStr
        << "\n";
    err << "note: move or rename the container, or build it somewhere with a "
           "plainer path.\n";
    return 1;
  }

  // The link writes outPath; if that is the container, the input is gone
  // and the diagnosis is a missing file the user is sure they created.
  // Cheap to refuse, and it costs nothing when outPath does not yet exist.
  if (isSameFile(absBundleStr, outPath)) {
    err << "error: --build-exe=" << outPath
        << " names the same file as the bundle " << bundlePath << "\n";
    return 1;
  }

  // 4b. Which compiler to run. The manifest's cc: is an absolute path
  // recorded on the machine that cut the kit, so it is the first candidate
  // rather than the answer -- see driverCandidates().
  std::string versionOutput;
  auto usable = [&versionOutput](const DriverCandidate &candidate) {
    std::optional<std::string> banner = captureDriverVersion(candidate.driver);
    if (!banner)
      return false;
    versionOutput = *banner;
    return true;
  };
  std::optional<DriverCandidate> driver =
      resolveDriver(ccOverride, manifest->cc, usable);
  if (!driver) {
    err << "error: no usable C++ driver found. Tried, in order:\n";
    for (const DriverCandidate &candidate :
         driverCandidates(ccOverride, manifest->cc))
      err << "  " << candidate.driver << " (" << driverSourceName(candidate)
          << ")\n";
    // Telling someone who just passed --cc to pass --cc is noise; what
    // they need to know is that the name they gave is what failed.
    if (ccOverride.empty())
      err << "note: pass --cc=<compiler> to name one.\n";
    else
      err << "note: --cc names the only driver tried; nothing is "
             "substituted for it.\n";
    return 1;
  }
  bool driverIsClang = versionOutputIsClang(versionOutput);

  if (verbose) {
    err << "kit: " << manifest->kitDir << " (hermes-node " << manifest->version
        << ")\n";
    err << "cc: " << driver->driver << " (" << driverSourceName(*driver)
        << (driverIsClang ? ", clang" : ", not clang") << ")\n";
    if (recordedDriverWasRejected(driver->source))
      err << "note: the kit recorded " << manifest->cc
          << ", which was not usable here.\n";
    err << "bundle: " << absBundleStr << " (" << file->size() << " bytes, "
        << reader->moduleCount()
        << (reader->moduleCount() == 1 ? " module)\n" : " modules)\n");
  }

  // 5. The payload object. The temporaries go beside the output rather
  // than in /tmp: the .o is a copy of the whole container plus headers, and
  // the directory the user chose for a multi-megabyte executable is the one
  // known to have room for it.
  fs::path outDir = fs::path(outPath).parent_path();
  if (outDir.empty())
    outDir = ".";
  std::string stem = fs::path(outPath).filename().string() + ".hnexe." +
      std::to_string(static_cast<long>(getpid()));
  // Lowercase ".s", and it is load-bearing: clang runs the C preprocessor
  // over ".S" and does not over ".s". Since the assemble step now forwards
  // the kit's whole driverflag list -- which carries -D flags -- an
  // uppercase extension would preprocess a file that contains an arbitrary
  // user-supplied path inside a quoted .incbin, and a '#' anywhere in it
  // would become a directive rather than data. Do not "tidy" this to ".S".
  std::string asmPath = (outDir / (stem + ".s")).string();
  std::string objPath = (outDir / (stem + ".o")).string();
  TempFiles temps{{asmPath, objPath}};

  std::string source = payloadAssembly(absBundleStr);
  if (verbose)
    err << "payload assembly (" << asmPath << "):\n" << source;

  {
    std::ofstream asmFile(asmPath, std::ios::binary | std::ios::trunc);
    asmFile << source;
    asmFile.close();
    if (!asmFile) {
      err << "error: cannot write " << asmPath << "\n";
      return 1;
    }
  }

  // With the manifest's driver flags, because some of them select the
  // target this object has to be built for -- see buildAssembleCommand().
  // An earlier revision left them out, on the reasoning that a
  // target-selecting kit did not exist yet. It did: release CI configures
  // macOS with CMAKE_OSX_ARCHITECTURES="x86_64;arm64" and then runs
  // check-hermes-node, which depends on the kit target, so the two-slice
  // kit is cut in the one pipeline whose failure blocks shipping.
  std::vector<std::string> assembleCmd = buildAssembleCommand(
      *manifest, driver->driver, driverIsClang, asmPath, objPath);
  if (verbose)
    err << "assemble: " << formatCommandLine(assembleCmd) << "\n";
  if (!runCommand(assembleCmd, err))
    return 1;

  // 6. The link.
  std::vector<std::string> linkCmd =
      buildLinkCommand(*manifest, driver->driver, objPath, outPath);
  if (verbose)
    err << "link: " << formatCommandLine(linkCmd) << "\n";
  if (!runCommand(linkCmd, err))
    return 1;

  // 7. The temporaries go with TempFiles. Report what was produced, in the
  // shape the other producers report it.
  uintmax_t outSize = fs::file_size(outPath, ec);
  if (ec) {
    err << "error: " << outPath << " was not produced: " << ec.message()
        << "\n";
    return 1;
  }
  out << "wrote " << outPath << " (" << outSize << " bytes)\n";

  // 8. What the artifact is, when it is more than one file. --build-bundle
  // says this when it copies an addon's sidecar beside the container; the
  // executable is where it matters more, because linking one MOVES the
  // place the sidecars have to be. "Alongside" was the container's
  // directory and is now this executable's, so a container that
  // --verify-natives passed, linked into a directory of its own and
  // shipped, throws MODULE_NOT_FOUND on the customer's first run with
  // nothing having warned anybody. Same stream and same wording as
  // --build-bundle's, after the `wrote` line rather than before it,
  // because the second note names the file that line just reported.
  const uint32_t nativeCount = reader->nativeCount();
  for (uint32_t i = 0; i < nativeCount; ++i) {
    BundleReader::NativeView native = reader->native(i);
    out << "native: " << native.sidecar << " (from "
        << reader->identity(native.moduleIndex) << ")\n";
  }
  if (nativeCount > 0) {
    out << "note: this executable requires " << nativeCount << " native addon"
        << (nativeCount == 1 ? "" : "s")
        << " alongside it; ship them together.\n";
    out << "note: they must sit beside " << outPath
        << ", not beside the container.\n";
  }
  return 0;
}

} // namespace node_compat
} // namespace hermes
