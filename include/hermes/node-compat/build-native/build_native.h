/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_NODE_COMPAT_BUILD_NATIVE_BUILD_NATIVE_H
#define HERMES_NODE_COMPAT_BUILD_NATIVE_BUILD_NATIVE_H

#include <hermes/node-compat/build-exe/build_exe.h>
#include <hermes/node-compat/build-exe/kit_manifest.h>

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace hermes {
namespace node_compat {

/// How hard both compilers try. One knob for two tools so a single number
/// means one thing; shermes has four levels and -O is its highest, which is
/// why O2 and O3 differ only on the cc side.
enum class OptLevel { O0, O1, O2, O3, Os };

/// The Static Hermes unit name for container module \p moduleIndex:
/// "hn_m" and the index, zero-padded to six digits (more digits past that,
/// never fewer -- the padding is a minimum width, not a truncation).
///
/// The index rather than the identity, because isValidSHUnitName permits
/// alphanumerics and underscore only -- an identity would have to be
/// mangled, and a mangling is a second thing that can collide. The index is
/// already unique, already the table's key, and already in the container.
std::string nativeUnitName(uint32_t moduleIndex);

/// The staged source path for container module \p i inside \p tempDir:
/// flat, not a tree mirroring the module's identity. -source-name carries
/// the name that matters for diagnostics and stack traces, so the staged
/// filename itself means nothing and can be as plain as an index.
std::string stagedSourcePath(const std::string &tempDir, uint32_t i);

/// The staged path for the C file shermes generates from module \p i.
std::string stagedCPath(const std::string &tempDir, uint32_t i);

/// The staged path for the object file cc generates from module \p i.
std::string stagedObjectPath(const std::string &tempDir, uint32_t i);

/// Writes the CommonJS-wrapped form of \p source to stagedSourcePath(
/// tempDir, i) -- the same wrapper the require() scanner parsed and the
/// bytecode producer compiles, from the one header both already share, so
/// what shermes sees is what the scan resolved require() bindings in.
///
/// Returns true on success; on failure, reports the reason to \p error.
bool stageModule(
    const std::string &tempDir,
    uint32_t i,
    std::string_view source,
    std::string *error);

/// The shermes argv that turns one staged module into generated C.
///
/// \p sourceName is the module's container identity and is what -source-name
/// puts into the unit's source-location table, so it is what stack traces
/// name. The staged path is a flat temp file and never appears in a trace.
///
/// Every language flag here is load-bearing and silent when missing. The
/// bytecode compiler enables ES6 block scoping and async generators;
/// shermes defaults both off, and block scoping off changes what a
/// let-in-loop closure captures with no diagnostic at build or run time --
/// measured as 3,3,3 where the same program prints 0,1,2. See
/// JSLanguageFlags in cjs_wrapper.h.
std::vector<std::string> buildShermesCommand(
    const std::string &shermesPath,
    const std::string &stagedPath,
    const std::string &outCPath,
    const std::string &sourceName,
    const std::string &unitName,
    bool typeScript,
    OptLevel opt);

/// The cc argv that turns generated C into an object.
///
/// -x c precedes the input and is mandatory: kit.manifest's `cc` is the C++
/// LINK driver, and a C++ driver compiles a .c file as C++ -- five hard
/// errors on real generated code. A separate C driver is deliberately not
/// recorded, because the manifest's sysroot, -arch and sanitizer flags came
/// from this one.
///
/// -std=gnu11 rather than the driver's default so the dialect cannot drift,
/// and gnu rather than c11 because the SH headers use zero-length arrays.
///
/// The manifest's driverFlags are forwarded here as well as to the assemble
/// step in build_exe.h, for the identical reason: some of them SELECT A
/// TARGET (-arch, --target, -isysroot), and compiling without them leaves
/// the object host-only, which the link cannot resolve for the slice it was
/// not built for. -Qunused-arguments is added only when \p driverIsClang,
/// the same suppression buildAssembleCommand() uses and for the same
/// reason: forwarding the whole driverFlags list means link-only flags
/// reach a compile that has no use for them.
std::vector<std::string> buildCompileCommand(
    const KitManifest &manifest,
    const std::string &driver,
    bool driverIsClang,
    const std::string &cPath,
    const std::string &objPath,
    OptLevel opt);

/// Builds the contents of a linker response file naming \p objects, one
/// quoted path per line, and writes it to \p out.
///
/// Response-file syntax is neither shell nor assembler: GNU ld and ld64
/// both split on whitespace and honour " quoting, so a path is always
/// quoted here, which makes an embedded space harmless -- and it has to be,
/// since checkIncbinPath() (build_exe.h) permits spaces and a macOS path
/// routinely has one. A quote, a backslash, a CR or an LF cannot be
/// expressed this way and are refused instead, reporting the offending
/// path in \p error -- by calling checkIncbinPath() directly rather than
/// keeping a second copy of its four-character check, since the two are
/// required to agree about what a usable path is.
///
/// Consumed by Task 11, which writes the result to
/// `<tempDir>/objects.rsp` and passes `@<path>` where buildLinkCommand()
/// (build_exe.h) expects its single blob-object argument.
bool linkResponseFile(
    const std::vector<std::string> &objects,
    std::string *out,
    std::string *error);

/// One container module to compile natively: the source text a bytecode
/// build would compile, carried here instead so the pool never re-reads the
/// container.
struct NativeModuleJob {
  uint32_t moduleIndex = 0;
  std::string identity; // what -source-name records
  std::string source; // unwrapped module text
  bool typeScript = false;
  /// True for the entry and for a preload, both certain to run: the
  /// orchestrator (bundle_build_native.cpp) refuses to turn a rejection of
  /// either into a throwing stub, the same rule the bytecode producer's own
  /// two stub sites apply. Mirrors PendingNativeModule::isEntry/isPreload
  /// (bundle_build_internal.h), which is where these are computed.
  bool isEntry = false;
  bool isPreload = false;
};

/// Where a module's compile stopped. The distinction is what decides
/// whether a build continues: see the failure policy in the design.
enum class NativeStage { Stage, Shermes, Compile };

/// The outcome of compiling one module: either \c ok, or enough to explain
/// what went wrong and to whom -- \c message is a one-line reason fit for a
/// summary line, \c diagnostics is the captured child output fit for
/// printing verbatim, and \c failedCommand is the argv that failed (empty
/// for a staging failure, which never ran a command).
///
/// \c outcome and \c exitStatus are the CommandResult the failing (or, for
/// the exited-0-but-no-output-file case, "succeeding") subprocess actually
/// returned, carried past this struct's own summarizing because the
/// orchestrator has to tell a source rejection (Exited, non-zero, with
/// diagnostics) from a crashed or unusable toolchain (Signalled,
/// SpawnFailed, WaitFailed) or a tool that quietly wrote nothing (Exited,
/// zero, no output file) -- exactly the distinction CommandResult exists to
/// make (see its own doc comment in build_exe.h) and precisely the
/// information \c message's prose has already thrown away. Meaningless
/// when \c failedStage is \c Stage, which never ran a subprocess at all.
struct NativeModuleResult {
  uint32_t moduleIndex = 0;
  bool ok = false;
  NativeStage failedStage = NativeStage::Stage;
  std::string diagnostics; // captured child output
  std::vector<std::string> failedCommand; // empty for a staging failure
  std::string message; // one-line reason
  CommandResult::Outcome outcome = CommandResult::Outcome::Exited;
  int exitStatus = 0;
  size_t sourceBytes = 0, cBytes = 0, objectBytes = 0;
  double milliseconds = 0;
};

/// Whether a failed module compile is a SOURCE REJECTION -- recoverable by
/// recompiling a throwing stub in its place -- rather than a hard build
/// error. Pulled out of the orchestrator (bundle_build_native.cpp) as its
/// own function so the classification is unit-testable without a toolchain
/// or a runtime: BuildNativeTest constructs a NativeModuleResult by hand and
/// asks this directly, which a lit test provoking a real `cc` failure (an
/// allocation failure at -O3, see below) cannot do cleanly.
///
/// True only for \c NativeStage::Shermes, on a module that is neither the
/// entry nor a preload (both certain to run, so both hard-fail
/// unconditionally), that exited non-zero with diagnostic text captured.
/// \c NativeStage::Compile is deliberately excluded: \c cc compiles the C
/// \c shermes generated, never the module's own JavaScript, so a rejection
/// there is a fact about the toolchain or the machine -- the realistic case
/// is \c cc running out of memory at -O3 on one large generated file, peak
/// RSS measured at 3.66 GB on a 1,500-module build -- and must stay a hard
/// build error rather than become a module that throws a \c SyntaxError
/// quoting the compiler's own text. See "Failure policy" in CLAUDE.md.
bool isNativeSourceRejection(
    bool isEntry,
    bool isPreload,
    const NativeModuleResult &result);

/// Runs an argv and reports what happened. Injected rather than called
/// directly so the pool, the ordering and the failure classification are
/// testable with no toolchain; the real caller passes runCommandCaptured.
using CommandRunner =
    std::function<CommandResult(const std::vector<std::string> &)>;

/// Compiles every job, at most \p parallelism at a time, and returns one
/// result per job IN JOB ORDER regardless of completion order -- the unit
/// table is indexed by module index, and a verbose log that jumps around is
/// harder to read for no gain.
///
/// A module whose shermes step fails does not reach cc: the C file it would
/// compile does not exist, and a second failure for the same module would
/// only bury the first. Likewise, a tool that exits 0 without producing its
/// output file fails that module at that stage -- otherwise the missing
/// file surfaces much later as an undefined symbol at link time, attributed
/// to nothing.
std::vector<NativeModuleResult> compileModules(
    const std::vector<NativeModuleJob> &jobs,
    const std::string &tempDir,
    const std::string &shermesPath,
    const KitManifest &manifest,
    const std::string &driver,
    bool driverIsClang,
    OptLevel opt,
    unsigned parallelism,
    const CommandRunner &run);

/// The per-module-index symbol table payloadAssembly() wants: the unit name
/// for a module that was compiled, an empty string for every other record
/// in the container -- a JSON module, a native addon, a resolve-only
/// package.json.
///
/// Indexed by container module index with holes rather than compacted,
/// because the index is how bundleLoadCallback finds an entry and a second
/// mapping is a second thing that can be wrong.
std::vector<std::string> unitSymbolTable(
    uint32_t moduleCount,
    const std::vector<NativeModuleJob> &jobs);

/// The two link arguments that select the kit's NATIVE built-in registry, in
/// the order they must appear.
///
/// Both registries define findEmbeddedModule(). The linker takes the first
/// definition it needs, so placing \p archivePath ahead of the merged kit
/// archive resolves it here and leaves the bytecode member unpulled -- which
/// keeps 2.1 MB of built-in bytecode out of the artifact rather than merely
/// unused inside it. Not a saving overall: the native code that replaces it
/// costs +7.27 MB, so this only keeps the default from paying for both.
///
/// Order alone would be silent when wrong: a link that picked the bytecode
/// registry would succeed and produce a working, interpreted binary. So the
/// marker is rooted with -Wl,-u, which does three jobs -- it extracts the
/// archive member, it keeps that member from -dead_strip / --gc-sections, and
/// it fails the link by name if the archive is absent. The -u MUST precede the
/// archive: GNU ld scans archives in order and does not go back.
std::vector<std::string> nativeBuiltinsLinkArgs(
    const std::string &archivePath,
    ObjectFormat format);

/// The complete link command for a native build.
///
/// This exists so the producer and its test call the SAME construction. The
/// one misconfiguration the linker cannot catch is a correct archive order
/// with the -u root missing, and a test over nativeBuiltinsLinkArgs() alone
/// would pass in exactly that state, because the producer would not be calling
/// it. \p bytecodeBuiltins omits both arguments, which is what
/// --bytecode-builtins does.
std::vector<std::string> buildNativeLinkCommand(
    const KitManifest &manifest,
    const std::string &driver,
    const std::string &blobArg,
    const std::string &outPath,
    bool bytecodeBuiltins,
    ObjectFormat format);

/// Generates the assembly carrying both the container at \p containerPath
/// and \p unitSymbols, via payloadAssembly() at this host's object format --
/// one generated .s so the two cannot get out of step and the link gains
/// one object rather than two.
std::string nativePayloadAssembly(
    const std::string &containerPath,
    const std::vector<std::string> &unitSymbols);

} // namespace node_compat
} // namespace hermes

#endif // HERMES_NODE_COMPAT_BUILD_NATIVE_BUILD_NATIVE_H
