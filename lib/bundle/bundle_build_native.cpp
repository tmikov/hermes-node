/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// The orchestration for a native --build-exe: share the whole discovery /
// resolution / classification / container-assembly walk with the bytecode
// producer (buildBundleImpl, PayloadMode::NativeSources), then compile every
// JavaScript module it left as source with shermes and cc, and link the
// result into a standalone executable. Lives here, in hermesNodeBundleBuild,
// rather than in lib/build-native: it has to call buildBundleImpl, which
// links the Hermes parser, so lib/build-native stays the VM-free half (the
// pure helpers: staging, command construction, the job pool, the unit
// table), which is what lets BuildNativeTest run with no runtime.

#include <hermes/node-compat/bundle/bundle_build.h>

#include "bundle_build_internal.h"

#include <hermes/node-compat/build-exe/build_exe.h>
#include <hermes/node-compat/build-exe/kit_manifest.h>
#include <hermes/node-compat/build-native/build_native.h>
#include <hermes/node-compat/bundle/atomic_write.h>
#include <hermes/node-compat/version.h>

#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace hermes {
namespace node_compat {

namespace fs = std::filesystem;

namespace {

/// Reports \p argv's failure the way build_exe.cpp's runCommand() does, but
/// against a CommandResult (captured output) rather than an inherited
/// stdio child: the bool-returning runCommand() is private to
/// build_exe.cpp, so this producer only has runCommandCaptured available,
/// and prints the captured output in the same three-line shape a failed
/// module compile uses (see the loop over results below). The one-line
/// summary itself comes from describeCommandResult() (build_exe.h), shared
/// with runCommand() and job_pool.cpp so the four outcomes read identically
/// everywhere a subprocess result reaches a human.
void reportCommandFailure(
    const char *what,
    const std::vector<std::string> &argv,
    const CommandResult &result) {
  std::fprintf(
      stderr, "error: %s\n", describeCommandResult(result, what).c_str());
  if (!result.output.empty())
    std::fprintf(stderr, "%s", result.output.c_str());
  std::fprintf(stderr, "  command: %s\n", formatCommandLine(argv).c_str());
}

/// A fresh, empty directory under $TMPDIR (or /tmp), for staging every
/// module's source, generated C, and object, plus the temp container, the
/// payload assembly and the linker response file. Returns "" with \p error
/// set on failure.
std::string makeNativeBuildTempDir(std::string *error) {
  const char *tmpdirEnv = std::getenv("TMPDIR");
  std::string base = (tmpdirEnv && *tmpdirEnv) ? tmpdirEnv : "/tmp";
  std::string tmpl = (fs::path(base) / "hermes-node-native-XXXXXX").string();
  std::vector<char> buf(tmpl.begin(), tmpl.end());
  buf.push_back('\0');
  if (::mkdtemp(buf.data()) == nullptr) {
    *error = "cannot create a temp directory under " + base + ": " +
        std::strerror(errno);
    return "";
  }
  // Absolute, and normalized: the container path this temp directory holds
  // is interpolated into a quoted assembler string by nativePayloadAssembly
  // (payloadAssembly), which resolves a relative .incbin against the
  // assembler's own working directory rather than ours.
  std::string dir(buf.data());
  std::error_code ec;
  fs::path abs = fs::absolute(dir, ec);
  return ec ? dir : abs.lexically_normal().string();
}

/// Removes \p dir (recursively) unless \p keep, however the enclosing call
/// leaves -- success, a return partway through, or a thrown exception this
/// codebase does not otherwise use. \p keep is mutable so a caller can
/// decide, after the fact, that a failure is worth preserving the directory
/// for even though it did not ask for --keep-temp up front.
struct TempDirGuard {
  std::string dir;
  bool keep;
  ~TempDirGuard() {
    if (keep || dir.empty())
      return;
    std::error_code ec;
    fs::remove_all(dir, ec);
  }
};

/// Reads the whole file at \p path into \p out. Mirrors bundle_build.cpp's
/// own readFile(), which is private to that translation unit.
bool readWholeFile(const std::string &path, std::string *out) {
  std::ifstream in(path, std::ios::binary);
  if (!in)
    return false;
  std::ostringstream ss;
  ss << in.rdbuf();
  if (in.bad())
    return false;
  *out = ss.str();
  return true;
}

} // namespace

int buildNativeExecutable(const NativeBuildOptions &options) {
  auto overallStart = std::chrono::steady_clock::now();

  // 0. The link writes options.outPath; if that names the entry script
  // itself, the input is gone before it is ever compiled. Mirrors
  // buildExecutable's identical refusal (isSameFile(bundle, outPath),
  // build_exe.cpp) for the same reason: cheap to check, and the
  // alternative is silently linking over the user's own source.
  if (isSameFile(options.entryPath, options.outPath)) {
    std::fprintf(
        stderr,
        "error: --build-native output %s names the same file as the entry "
        "%s\n",
        options.outPath.c_str(),
        options.entryPath.c_str());
    return 1;
  }

  // 1. The kit. Its absence is the common case for a binary installed
  // without one, so say where it looked and how to point elsewhere -- and a
  // kit cut from a different build than this producer links archives and
  // headers this binary has no reason to match, exactly as it is for
  // --build-exe (build_exe.cpp).
  std::string error;
  std::optional<KitManifest> manifest = readKitManifest(options.kitDir, &error);
  if (!manifest) {
    std::fprintf(stderr, "error: %s\n", error.c_str());
    std::fprintf(
        stderr,
        "note: building a native executable needs a link kit; --kit=<dir> "
        "names one.\n");
    return 1;
  }
  if (manifest->version != HERMES_NODE_VERSION_STRING) {
    std::fprintf(
        stderr,
        "error: kit %s was cut from hermes-node %s, but this is hermes-node "
        "%s\n",
        manifest->kitDir.c_str(),
        manifest->version.c_str(),
        HERMES_NODE_VERSION_STRING);
    std::fprintf(
        stderr,
        "note: re-cut the kit with: cmake --build <build dir> --target "
        "hermes-node-kit\n");
    return 1;
  }
  // Refused here, before a single module is compiled: this is known the
  // moment the manifest is read, and failing after a multi-minute compile
  // for a reason available at the start is the worst available ordering.
  // Which is why the file itself is stat'd and not only the manifest key:
  // an archive the manifest names but the kit does not hold is just as
  // knowable now, and the only other thing that would notice is the link --
  // which runs after every module has been compiled.
  if (!options.bytecodeBuiltins) {
    const char *why = nullptr;
    if (manifest->nativeBuiltinsArchive.empty())
      why = "records no native built-ins archive";
    else if (::access(manifest->nativeBuiltinsArchive.c_str(), R_OK) != 0)
      why = "records a native built-ins archive that is not readable";
    if (why) {
      std::fprintf(stderr, "error: kit %s %s", manifest->kitDir.c_str(), why);
      if (!manifest->nativeBuiltinsArchive.empty())
        std::fprintf(stderr, ": %s", manifest->nativeBuiltinsArchive.c_str());
      std::fprintf(
          stderr,
          "\n"
          "       Rebuild it with: cmake --build <build dir> --target "
          "hermes-node-kit\n"
          "       or pass --bytecode-builtins to link the interpreted "
          "built-ins.\n");
      return 1;
    }
  }

  // 2. shermes. Unlike the C driver below there is no second candidate
  // worth trying: the kit's Static Hermes headers and this binary's
  // generation tag both came from the very build that cut this kit.
  std::string shermesPath = !options.shermesOverride.empty()
      ? options.shermesOverride
      : (fs::path(manifest->kitDir) / "shermes").string();
  if (::access(shermesPath.c_str(), X_OK) != 0) {
    std::fprintf(
        stderr,
        "error: shermes not found or not executable: %s\n",
        shermesPath.c_str());
    return 1;
  }

  // 3. The C driver. The manifest's cc: is an absolute path recorded on the
  // machine that cut the kit, so it is the first candidate rather than the
  // answer -- see driverCandidates(). captureDriverVersion() (build_exe.cpp)
  // is a static helper and not reachable from here, so this runs the same
  // probe through the public runCommandCaptured(): one subprocess answers
  // both "can this be run at all" and "is it Clang".
  std::string versionOutput;
  auto usable = [&versionOutput](const DriverCandidate &candidate) {
    CommandResult r = runCommandCaptured({candidate.driver, "--version"});
    // Matches captureDriverVersion()'s own nullopt cases exactly:
    // posix_spawnp failing to start the child (SpawnFailed), waitpid()
    // failing on something other than EINTR -- runCommandCaptured() already
    // retries EINTR internally, so WaitFailed here is a real wait error, not
    // a signal to swallow -- and exec failure surfacing through the child's
    // exit status (127) rather than through posix_spawnp's return value.
    if (r.outcome == CommandResult::Outcome::SpawnFailed ||
        r.outcome == CommandResult::Outcome::WaitFailed)
      return false;
    // Deliberately not "exited 0" otherwise -- see captureDriverVersion()'s
    // own comment: --version's exit status does not answer "can this be
    // run", so demanding success would fall back off a working compiler
    // that merely reports itself oddly.
    if (r.outcome == CommandResult::Outcome::Exited && r.status == 127)
      return false;
    versionOutput = r.output;
    return true;
  };
  std::optional<DriverCandidate> driver =
      resolveDriver(options.ccOverride, manifest->cc, usable);
  if (!driver) {
    std::fprintf(
        stderr, "error: no usable C++ driver found. Tried, in order:\n");
    for (const DriverCandidate &candidate :
         driverCandidates(options.ccOverride, manifest->cc))
      std::fprintf(
          stderr,
          "  %s (%s)\n",
          candidate.driver.c_str(),
          driverSourceName(candidate));
    if (options.ccOverride.empty())
      std::fprintf(stderr, "note: pass --cc=<compiler> to name one.\n");
    else
      std::fprintf(
          stderr,
          "note: --cc names the only driver tried; nothing is substituted "
          "for it.\n");
    return 1;
  }
  bool driverIsClang = versionOutputIsClang(versionOutput);

  if (options.verbose) {
    std::fprintf(
        stderr,
        "kit: %s (hermes-node %s)\n",
        manifest->kitDir.c_str(),
        manifest->version.c_str());
    // Which registry the link will resolve findEmbeddedModule() from. The
    // one link-time choice this producer makes whose wrong answer is
    // silent -- a binary with the bytecode registry runs correctly and
    // interprets everything -- so it is the one worth narrating. Beside
    // the kit because the archive is the kit's.
    if (options.bytecodeBuiltins)
      std::fprintf(stderr, "built-ins: bytecode (--bytecode-builtins)\n");
    else
      std::fprintf(
          stderr,
          "built-ins: native (%s)\n",
          manifest->nativeBuiltinsArchive.c_str());
    std::fprintf(stderr, "shermes: %s\n", shermesPath.c_str());
    std::fprintf(
        stderr,
        "cc: %s (%s%s)\n",
        driver->driver.c_str(),
        driverSourceName(*driver),
        driverIsClang ? ", clang" : ", not clang");
    if (recordedDriverWasRejected(driver->source))
      std::fprintf(
          stderr,
          "note: the kit recorded %s, which was not usable here.\n",
          manifest->cc.c_str());
  }

  // 4. The temp directory every staged file, the temp container, the
  // payload assembly and the linker response file live under.
  std::string tempDir = makeNativeBuildTempDir(&error);
  if (tempDir.empty()) {
    std::fprintf(stderr, "error: %s\n", error.c_str());
    return 1;
  }
  TempDirGuard tempGuard{tempDir, options.keepTemp};
  if (options.verbose)
    std::fprintf(stderr, "temp dir: %s\n", tempDir.c_str());

  // The produced executable's own absolute directory: where the run path
  // looks for a native addon's sidecar beside a linked executable, and what
  // the collision guard in step 10 below checks every sidecar destination
  // against. Resolved once, up front, and absolutized rather than taking
  // options.outPath's own parent_path(): fs::path("app").parent_path() is
  // the EMPTY string, which is exactly the sentinel buildBundleImpl reads
  // as "derive sidecarDir from outPath's own parent" -- and outPath there
  // is the throwaway temp container, not this executable. Passing that
  // sentinel by accident would silently copy every addon into the temp
  // directory, print success, and then have TempDirGuard delete them out
  // from under the user.
  std::error_code absOutEc;
  fs::path absOutPath = fs::absolute(fs::path(options.outPath), absOutEc);
  if (absOutEc) {
    std::fprintf(
        stderr,
        "error: cannot resolve %s: %s\n",
        options.outPath.c_str(),
        absOutEc.message().c_str());
    return 1;
  }
  fs::path outDir = absOutPath.parent_path();
  std::string sidecarDir = outDir.empty() ? std::string(".") : outDir.string();

  // Steps 5-11 run inside this lambda so that a failure partway through can
  // still leave the temp directory behind for inspection (see the call
  // site below): several of the error paths here print a `command:` line
  // naming a path under tempDir -- a staged source, a generated .c or .o,
  // the assembled payload.s/.o -- and TempDirGuard would otherwise erase
  // exactly the evidence that message just told the user to go look at.
  auto run = [&]() -> int {
    // 5. The shared walk, with the payload step told to leave every
    // JavaScript module's source in BuildProducts::pendingNative instead of
    // compiling it to bytecode. env is null: NativeSources never reaches
    // hermes_compile_to_bytecode (see bundle_build_internal.h and the env
    // audit in bundle_build.cpp's task history).
    std::string containerPath = (fs::path(tempDir) / "container.hbb").string();
    BuildProducts products;
    int buildRc = buildBundleImpl(
        /*env=*/nullptr,
        options.entryPath,
        containerPath,
        sidecarDir,
        options.verbose,
        options.includes,
        options.preloads,
        options.bakeWasmPaths,
        options.vmOptions,
        options.allowVmOptionsOverride,
        PayloadMode::NativeSources,
        &products);
    if (buildRc != 0)
      return buildRc;

    // 6. Every pending module becomes a compile job. The index and identity
    // are already filled in by buildBundleImpl's step 5 (see
    // bundle_build_internal.h): the index comes from moduleIndex, assigned
    // only once every module has one, which is why native compilation
    // cannot start any earlier.
    std::vector<NativeModuleJob> jobs;
    jobs.reserve(products.pendingNative.size());
    for (const PendingNativeModule &pending : products.pendingNative) {
      jobs.push_back(
          {pending.moduleIndex,
           pending.identity,
           pending.source,
           pending.typeScript,
           pending.isEntry,
           pending.isPreload});
    }
    unsigned parallelism =
        options.jobs != 0 ? options.jobs : std::thread::hardware_concurrency();
    if (parallelism == 0)
      parallelism = 1;

    std::vector<NativeModuleResult> results = compileModules(
        jobs,
        tempDir,
        shermesPath,
        *manifest,
        driver->driver,
        driverIsClang,
        options.opt,
        parallelism,
        runCommandCaptured);

    // 7. A failure is classified by CommandResult's own Outcome (Task 5),
    // which is exactly the distinction Task 11 assumed was unavailable from
    // outside the process. A SOURCE REJECTION -- shermes ran, exited
    // non-zero, and said why -- on a module that is neither the entry nor a
    // preload is recoverable the same way the bytecode producer's own
    // second stub site (makeThrowingStub, bundle_build.cpp) recovers from
    // an IRGen rejection: recompile a throwing stub in its place and let
    // the build continue. That symmetry is the fix for a false premise the
    // design shipped with -- that this case could never arise because the
    // shared scanner already filters it -- which measurably does not hold:
    // `import()` inside a `.cjs` parses fine and is rejected only here, in
    // IRGen, which the scanner never reaches (dz 01a0a0d6-03d4).
    //
    // Deliberately restricted to NativeStage::Shermes and never Compile:
    // shermes rejecting is a fact about the module's JavaScript, but cc
    // rejecting is a fact about the toolchain compiling shermes's
    // GENERATED C, which says nothing about the source. The realistic case
    // is cc running out of memory at -O3 on one large generated file (peak
    // RSS was measured at 3.66 GB on a 1,500-module build) -- a hard
    // failure there must stay a hard build error, not a module that throws
    // `SyntaxError: <identity>: <clang's out-of-memory text>` at run time.
    // Everything else -- a crash, a spawn or wait failure, an exit with no
    // diagnostic text, or a tool that exited 0 and wrote nothing -- also
    // says something about the toolchain or the machine rather than the
    // source, and must never become a module that throws at run time; the
    // entry and a preload get the hard error unconditionally too, being
    // certain to run. Every hard failure is still reported, not just the
    // first, because a parallel build finds several at once and re-running
    // to see the next one wastes minutes.
    bool anyFailed = false;
    for (size_t i = 0; i < results.size(); ++i) {
      const NativeModuleResult &r = results[i];
      if (r.ok) {
        if (options.verbose) {
          std::fprintf(
              stderr,
              "compile [%u] %s %zu src -> %zu c -> %zu obj  %.2f ms\n",
              jobs[i].moduleIndex,
              jobs[i].identity.c_str(),
              r.sourceBytes,
              r.cBytes,
              r.objectBytes,
              r.milliseconds);
        }
        continue;
      }

      bool sourceRejection =
          isNativeSourceRejection(jobs[i].isEntry, jobs[i].isPreload, r);
      if (sourceRejection) {
        // Same wording the bytecode producer's own compile-failure warning
        // uses (bundle_build.cpp), so a build log reads identically for the
        // identical situation on either path.
        std::fprintf(
            stderr,
            "warning: cannot compile %s (%s); packaged as a module that "
            "throws when required\n",
            jobs[i].identity.c_str(),
            r.diagnostics.c_str());
        ++products.stubbedModules;

        // Recompile in place: same moduleIndex, so the stub's object lands
        // at the exact stagedObjectPath the link step below already expects
        // for this job, and the unit table entry payloadAssembly() built
        // from `jobs` (step 8) still names the right symbol.
        NativeModuleJob stubJob = jobs[i];
        stubJob.source = makeThrowingStub(jobs[i].identity, r.diagnostics);
        stubJob.typeScript = false; // the stub is plain JavaScript
        std::vector<NativeModuleResult> stubResults = compileModules(
            {stubJob},
            tempDir,
            shermesPath,
            *manifest,
            driver->driver,
            driverIsClang,
            options.opt,
            /*parallelism=*/1,
            runCommandCaptured);
        const NativeModuleResult &stubResult = stubResults.front();
        if (!stubResult.ok) {
          std::fprintf(
              stderr,
              "error: internal: the stub for %s does not compile: %s\n",
              jobs[i].identity.c_str(),
              stubResult.message.c_str());
          if (!stubResult.diagnostics.empty())
            std::fprintf(stderr, "%s", stubResult.diagnostics.c_str());
          return 1;
        }
        if (options.verbose) {
          std::fprintf(
              stderr,
              "compile [%u] %s (stub) %zu src -> %zu c -> %zu obj  %.2f ms\n",
              jobs[i].moduleIndex,
              jobs[i].identity.c_str(),
              stubResult.sourceBytes,
              stubResult.cBytes,
              stubResult.objectBytes,
              stubResult.milliseconds);
        }
        continue;
      }

      anyFailed = true;
      std::fprintf(
          stderr,
          "error: %s: %s\n",
          jobs[i].identity.c_str(),
          r.message.c_str());
      if (!r.diagnostics.empty())
        std::fprintf(stderr, "%s", r.diagnostics.c_str());
      if (!r.failedCommand.empty()) {
        std::fprintf(
            stderr,
            "  command: %s\n",
            formatCommandLine(r.failedCommand).c_str());
      }
    }
    if (anyFailed)
      return 1;

    // 8. The unit table and the payload assembly that carries it plus the
    // container, written to one generated .s so the two cannot get out of
    // step (see nativePayloadAssembly / payloadAssembly). containerPath is
    // interpolated into a quoted assembler string exactly as buildExecutable
    // interpolates a bytecode bundle's path (build_exe.cpp), so it needs the
    // identical guard: it comes from tempDir, which comes from $TMPDIR, and
    // nothing upstream of here has ever checked what characters that
    // environment variable holds.
    if (std::string bad = checkIncbinPath(containerPath); !bad.empty()) {
      std::fprintf(
          stderr,
          "error: the temp container's path contains %s, which cannot be "
          "named in the generated assembly: %s\n",
          bad.c_str(),
          containerPath.c_str());
      std::fprintf(
          stderr, "note: point TMPDIR at a plainer path and rebuild.\n");
      return 1;
    }
    std::vector<std::string> unitSymbols =
        unitSymbolTable(products.moduleCount, jobs);
    std::string asmSource = nativePayloadAssembly(containerPath, unitSymbols);
    std::string asmPath = (fs::path(tempDir) / "payload.s").string();
    {
      std::ofstream asmFile(asmPath, std::ios::binary | std::ios::trunc);
      asmFile << asmSource;
      asmFile.close();
      if (!asmFile) {
        std::fprintf(stderr, "error: cannot write %s\n", asmPath.c_str());
        return 1;
      }
    }
    if (options.verbose) {
      std::fprintf(
          stderr,
          "payload assembly (%s):\n%s",
          asmPath.c_str(),
          asmSource.c_str());
    }

    // 9. Assemble the payload, then link it -- with the response file to
    // keep buildLinkCommand()'s single-blob-object ordering rule intact
    // whatever the module count is (see linkResponseFile's doc comment).
    std::string payloadObjPath = (fs::path(tempDir) / "payload.o").string();
    std::vector<std::string> assembleCmd = buildAssembleCommand(
        *manifest, driver->driver, driverIsClang, asmPath, payloadObjPath);
    if (options.verbose)
      std::fprintf(
          stderr, "assemble: %s\n", formatCommandLine(assembleCmd).c_str());
    CommandResult assembleResult = runCommandCaptured(assembleCmd);
    if (!assembleResult.ok()) {
      reportCommandFailure("the assembler", assembleCmd, assembleResult);
      return 1;
    }

    std::vector<std::string> objects;
    objects.reserve(jobs.size() + 1);
    objects.push_back(payloadObjPath);
    for (const NativeModuleJob &job : jobs)
      objects.push_back(stagedObjectPath(tempDir, job.moduleIndex));
    std::string responseFile, responseError;
    if (!linkResponseFile(objects, &responseFile, &responseError)) {
      std::fprintf(stderr, "error: %s\n", responseError.c_str());
      return 1;
    }
    std::string responsePath = (fs::path(tempDir) / "objects.rsp").string();
    {
      std::ofstream responseOut(
          responsePath, std::ios::binary | std::ios::trunc);
      responseOut << responseFile;
      responseOut.close();
      if (!responseOut) {
        std::fprintf(stderr, "error: cannot write %s\n", responsePath.c_str());
        return 1;
      }
    }

    std::vector<std::string> linkCmd = buildNativeLinkCommand(
        *manifest,
        driver->driver,
        "@" + responsePath,
        options.outPath,
        options.bytecodeBuiltins,
        hostObjectFormat());
    if (options.verbose)
      std::fprintf(stderr, "link: %s\n", formatCommandLine(linkCmd).c_str());
    CommandResult linkResult = runCommandCaptured(linkCmd);
    if (!linkResult.ok()) {
      reportCommandFailure("the linker", linkCmd, linkResult);
      return 1;
    }

    // 10. Now that the executable exists, place every native addon's
    // sidecar beside it. Deferred to here for the reason
    // BuildProducts::sidecarCopies documents: a native build still has a
    // compile and a link that can fail after the container is assembled,
    // and copying earlier would widen the window in which a failed build
    // leaves this run's sidecars beside a last-run (or nonexistent)
    // artifact.
    for (const SidecarCopy &copy : products.sidecarCopies) {
      const std::string &src = copy.src;
      const std::string &dst = copy.dst;
      // Refuse to write over the executable itself. This mirrors
      // bundle_build.cpp's own guard against a sidecar colliding with
      // outPath -- but that guard compares against the container passed to
      // buildBundleImpl, which in native mode is this run's throwaway temp
      // container, not the executable a sidecar actually has to sit
      // beside. So the real hazard is unguarded there and is checked here
      // instead, against the real output. Two tests, as bundle_build.cpp's
      // does: isSameFile catches a rebuild (the executable from last time
      // still exists), and the spelling comparison catches the first
      // build, where nothing exists yet and stat() has nothing to say.
      if (isSameFile(dst, options.outPath) ||
          fs::path(dst).lexically_normal() ==
              fs::path(options.outPath).lexically_normal()) {
        std::fprintf(
            stderr,
            "error: native addon sidecar %s would be written over the "
            "executable %s\n",
            dst.c_str(),
            options.outPath.c_str());
        return 1;
      }
      // An in-place addon (dst already IS src -- the common case of a flat
      // project built into its own directory) needs no copy, but it is
      // still required and still gets its line and its place in the count
      // below: dropping it here is exactly the bug this branch exists to
      // avoid (see SidecarCopy's own doc comment).
      if (!copy.inPlace) {
        std::string contents;
        if (!readWholeFile(src, &contents)) {
          std::fprintf(stderr, "error: cannot read %s\n", src.c_str());
          return 1;
        }
        if (!writeFileAtomically(
                dst, contents.data(), contents.size(), std::cerr))
          return 1;
        std::error_code statEc;
        fs::file_status srcStatus = fs::status(src, statEc);
        if (!statEc) {
          std::error_code modeEc;
          fs::permissions(dst, srcStatus.permissions(), modeEc);
        }
      }
      std::string identity =
          fs::path(src).lexically_relative(products.root).generic_string();
      std::printf(
          "native: %s (from %s)\n",
          fs::path(dst).filename().string().c_str(),
          identity.c_str());
    }
    if (!products.sidecarCopies.empty()) {
      std::printf(
          "note: this executable requires %zu native addon%s alongside "
          "it; ship them together.\n",
          products.sidecarCopies.size(),
          products.sidecarCopies.size() == 1 ? "" : "s");
    }

    // 11. The summary: the output path and its size, modules compiled,
    // stubs (unconditionally, matching the bytecode producer's own summary
    // line -- see bundle_build.cpp), and wall clock.
    std::error_code sizeEc;
    uintmax_t outSize = fs::file_size(options.outPath, sizeEc);
    if (sizeEc) {
      std::fprintf(
          stderr,
          "error: %s was not produced: %s\n",
          options.outPath.c_str(),
          sizeEc.message().c_str());
      return 1;
    }
    double wallMs = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - overallStart)
                        .count();
    std::printf("wrote %s (%ju bytes)\n", options.outPath.c_str(), outSize);
    // "compiled natively" rather than plain "modules:", the bytecode
    // producer's own wording (bundle_build.cpp): jobs.size() counts only
    // the JavaScript modules this run compiled, where the bytecode
    // summary's count is every record in the container (JSON and native
    // addons included) -- the same word over different denominators reads
    // as a discrepancy the moment someone compares the two lines.
    std::printf("modules: %zu compiled natively", jobs.size());
    if (products.stubbedModules != 0) {
      std::printf(
          ", %u packaged as throwing stub%s",
          products.stubbedModules,
          products.stubbedModules == 1 ? "" : "s");
    }
    std::printf("\n");
    std::printf("compile: %.2f ms\n", wallMs);

    return 0;
  };

  int rc = run();
  // A failure keeps the temp directory even without --keep-temp: several
  // of the error messages above (a failed module's `command:` line, a
  // failed assemble/link) name paths under it, and deleting them out from
  // under a message that just told the user to go inspect them would be
  // the same silent-evidence-loss bug this comment exists to prevent.
  // Success still cleans up by default, which is the common case.
  if (rc != 0 && !options.keepTemp)
    tempGuard.keep = true;
  // One line, printed whenever the directory is actually being kept --
  // whether the caller asked for it up front or a failure earned it just
  // above -- in a fixed, greppable format: a test that scrapes a sentence
  // for its path is a test that breaks the next time someone rewords the
  // sentence.
  if (tempGuard.keep) {
    std::fprintf(stderr, "build-native: temp directory: %s\n", tempDir.c_str());
  }
  return rc;
}

} // namespace node_compat
} // namespace hermes
