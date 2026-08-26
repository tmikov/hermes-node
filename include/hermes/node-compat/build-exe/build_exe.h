/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_NODE_COMPAT_BUILD_EXE_BUILD_EXE_H
#define HERMES_NODE_COMPAT_BUILD_EXE_BUILD_EXE_H

#include <hermes/node-compat/build-exe/kit_manifest.h>

#include <functional>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

namespace hermes {
namespace node_compat {

/// Where a chosen toolchain driver came from. Reported by --verbose and
/// named in the failure message, because "which compiler did it actually
/// run" is the first question when a link goes wrong on a machine that is
/// not the one that cut the kit.
enum class DriverSource {
  /// --cc=<x> on the command line.
  Override,
  /// The manifest's `cc:` line, verbatim -- the absolute path recorded when
  /// the kit was cut.
  ManifestPath,
  /// The basename of that path, found on PATH: the same compiler on a
  /// different machine.
  ManifestName,
  /// Plain `c++`, the POSIX-conventional driver name.
  Fallback,
};

struct DriverCandidate {
  std::string driver;
  DriverSource source;
};

/// The drivers to try, best first, without touching the filesystem.
///
/// The kit records an absolute path to the compiler that cut it, which is
/// a fact about that machine. Treating it as a requirement makes a kit
/// unusable anywhere else; treating it as merely a hint would ignore that
/// the kit's archives and driver flags came from that specific compiler --
/// a kit cut with -stdlib=libc++, with LTO bitcode in its archives, or in
/// an ASAN configuration will not link under a different driver. So the
/// recorded compiler is preferred and `c++` is the graceful degradation.
///
/// A candidate equal to one already in the list is dropped, so the list
/// never suggests the same thing twice: a manifest recording a bare
/// `clang++` yields no separate basename candidate, and one recording
/// `c++` collapses to a single entry.
///
/// Exposed separately from resolveDriver() because this list is what the
/// "no usable driver" error has to print, and because the ORDER is the
/// design decision here -- checkable with no filesystem and no toolchain.
std::vector<DriverCandidate> driverCandidates(
    const std::string &ccOverride,
    const std::string &manifestCc);

/// The first of driverCandidates() that \p usable accepts, or nullopt if
/// none does.
///
/// \p usable is injected rather than hardcoded so the ordering can be
/// tested without a filesystem; buildExecutable() passes one that asks
/// whether the candidate names an executable file (for an absolute path)
/// or is findable on PATH (for a bare name).
std::optional<DriverCandidate> resolveDriver(
    const std::string &ccOverride,
    const std::string &manifestCc,
    const std::function<bool(const DriverCandidate &)> &usable);

/// Whether \p versionOutput -- what `<driver> --version` printed -- came
/// from Clang, which decides whether the assemble step may use
/// -Qunused-arguments (see buildAssembleCommand()).
///
/// Asking the driver is the only option available. The flag cannot be
/// recorded in the manifest, because --cc can replace the driver long after
/// the kit was cut; and it cannot be dropped, because the whole driver-flag
/// list is forwarded to the assemble on purpose and something has to
/// silence the resulting noise.
///
/// A driver that says nothing recognizable is NOT treated as Clang:
/// guessing yes would hand an unknown driver a flag it may reject, turning
/// it into a hard failure on first use, where guessing no costs only some
/// warnings.
bool versionOutputIsClang(const std::string &versionOutput);

/// \p argv rendered as a command line the user can paste into a shell,
/// edit, and run.
///
/// That is the whole requirement, and it is why this quotes where an
/// earlier version deliberately did not. Nothing here goes through a shell
/// -- runCommand() spawns an argv directly -- so printing the arguments
/// bare was an accurate record of what ran. But it was not a command
/// anyone could re-run: a kit under a path with a space, or an
/// `-isysroot /Some SDK`, printed as two arguments and pasted back as two
/// arguments. Reproducing the failing link by hand is the reason the
/// command is printed at all, so being pasteable wins.
///
/// Only arguments a shell would touch are quoted, so an ordinary command
/// line still reads as one.
std::string formatCommandLine(const std::vector<std::string> &argv);

/// Whether choosing \p source means the kit's recorded compiler was tried
/// and found unusable -- which is worth telling the user, since it is the
/// difference between using a kit as intended and linking with a
/// substitute.
///
/// False for --cc, which skips the recorded compiler without judging it:
/// reporting it "not usable" there would be untrue.
bool recordedDriverWasRejected(DriverSource source);

/// Builds a standalone executable from an already-built container.
///
/// The container is not injected into a prebuilt binary; it is assembled
/// into an object and *linked* against the kit (see kit_manifest.h)
/// together with the app entry object the kit carries. So the result is an
/// ordinary executable the linker produced, with the bundle in a read-only
/// section of it, and nothing at run time reads \p bundlePath -- deleting
/// the container afterwards leaves a working program.
///
/// Needs no Hermes runtime: the container arrives already compiled, so
/// this reads it, generates one object, and drives the toolchain. That is
/// what keeps hermesNodeBuildExe free of the VM, the same property
/// BundleFormatTest and BundleToolsTest rely on.
///
/// \p verbose narrates to \p err: the kit and its manifest version, the
/// container and its size, the generated assembly, and both command lines
/// verbatim. The produced executable is the same with or without it.
///
/// Returns a process exit code -- 0, or 1 with the reason reported on
/// \p err. Success prints one line to \p out: the output path and its
/// size, which is the only thing that ever goes to \p out.
///
/// The assembler's and the linker's own diagnostics do NOT go to \p err.
/// The subprocesses inherit this process's stderr, so a compiler error
/// lands on fd 2 while the "failed with exit status" line naming the
/// command lands on \p err. A caller passing an ostringstream therefore
/// captures the summary and not the explanation. That is deliberate: a
/// toolchain's diagnostics are streamed, colored, and sized by the
/// toolchain, and re-serializing them through a C++ stream would lose all
/// three for the sake of a caller that does not exist -- the one caller
/// passes std::cerr.
/// \p ccOverride is --cc=<x>, or empty for none. It is the first driver
/// candidate; see driverCandidates() for the rest and why they are ordered
/// as they are.
int buildExecutable(
    const std::string &bundlePath,
    const std::string &outPath,
    const std::string &kitDir,
    const std::string &ccOverride,
    bool verbose,
    std::ostream &out,
    std::ostream &err);

/// The exact argv for the final link, given a manifest and the payload
/// object.
///
/// Separated from the running of it so the ordering rule -- the driver's
/// own flags, then BOTH objects, then everything the manifest names (whose
/// own order already puts system libraries last), then the output -- is
/// testable without a toolchain. Get it wrong and lazy archive resolution
/// finds nothing, which surfaces as a page of undefined symbols that reads
/// like a broken kit rather than like a mis-ordered command.
///
/// The app entry object is derived here as
/// `<manifest.kitDir>/hermes-node-bundle-main.o` rather than passed in,
/// which is why kitDir is a field on KitManifest: it is the same directory
/// the manifest's {kit} substitution already used, and a second parameter
/// would be one more chance for the two to disagree.
std::vector<std::string> buildLinkCommand(
    const KitManifest &manifest,
    const std::string &driver,
    const std::string &blobObject,
    const std::string &outPath);

/// The exact argv for assembling the generated payload source into an
/// object, given a manifest and the two paths.
///
/// The manifest's driver flags are forwarded here as well as to the link,
/// which matters for exactly one reason: some of them SELECT A TARGET.
/// `-arch x86_64 -arch arm64` on a universal macOS kit, a `--target=` or an
/// `-isysroot` on a cross-compiling one. Assemble without them and the
/// payload object is host-only, and the link cannot resolve
/// hermesNodeBundleStart for the slice it was not built for. That is not
/// hypothetical: release CI configures macOS with
/// CMAKE_OSX_ARCHITECTURES="x86_64;arm64" and then runs the test suite,
/// which cuts a kit from that very link line.
///
/// The whole list is forwarded rather than a hand-picked target-selecting
/// subset, because a hand-maintained list of "which flags select a target,
/// per driver" is the same class of thing the manifest exists to abolish --
/// it drifts, and its drift is discovered at somebody else's link. The
/// price is that link-only flags (-rdynamic, -Wl,...) reach a compile that
/// has no use for them, hence the leading -Qunused-arguments: without it
/// every --build-exe would print a handful of
/// -Wunused-command-line-argument warnings that mean nothing. That flag is
/// Clang-only, which this project already requires (see CLAUDE.md); a kit
/// cut with GCC would fail here immediately and by name, rather than
/// silently.
///
/// Separated from the running of it for the same reason
/// buildLinkCommand() is: it is checkable without a toolchain, and the
/// architecture case cannot be checked on this host at all.
std::vector<std::string> buildAssembleCommand(
    const KitManifest &manifest,
    const std::string &driver,
    bool driverIsClang,
    const std::string &asmPath,
    const std::string &objPath);

/// The object file format the generated assembly targets. An explicit
/// parameter rather than an #ifdef inside payloadAssembly(), so that both
/// spellings are compiled -- and can be asserted -- on every host. With the
/// #ifdef, the branch for the platform you are not on is not merely
/// untested, it is not in the binary: a typo in it (a dropped leading
/// underscore, a mangled .p2align) passes every test and every build on
/// this host and surfaces on the first Darwin build, as an assembler error
/// or as openEmbeddedBundle() refusing a misaligned payload at app startup.
enum class ObjectFormat { ELF, MachO };

/// The format this host's toolchain produces, which is what
/// buildExecutable() asks for. The only #ifdef in this interface, and it
/// selects a value rather than removing code.
constexpr ObjectFormat hostObjectFormat() {
#ifdef __APPLE__
  return ObjectFormat::MachO;
#else
  return ObjectFormat::ELF;
#endif
}

/// The assembler source that carries \p bundlePath's bytes into a
/// read-only section, defining hermesNodeBundleStart and
/// hermesNodeBundleEnd around them (bundle_main.cpp declares both; the
/// size is their difference, so no stored length can disagree with the
/// bytes).
///
/// \p bundlePath is interpolated into a quoted assembler string, so it
/// must be absolute and free of the characters checkIncbinPath() rejects;
/// buildExecutable() has already established both by the time it calls
/// this.
///
/// Exposed for the same reason buildLinkCommand() is: what this emits is
/// checkable without a toolchain, and two of its lines are load-bearing in
/// a way that is invisible in the produced binary until something else
/// goes wrong -- the alignment openEmbeddedBundle() enforces, and the ELF
/// note whose absence makes the linker mark the executable as needing an
/// executable stack.
std::string payloadAssembly(
    const std::string &bundlePath,
    ObjectFormat format = hostObjectFormat());

} // namespace node_compat
} // namespace hermes

#endif // HERMES_NODE_COMPAT_BUILD_EXE_BUILD_EXE_H
