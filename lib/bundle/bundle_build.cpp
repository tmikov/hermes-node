/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <hermes/node-compat/bundle/bundle_build.h>

#include <hermes/node-compat/bundle/atomic_write.h>
#include <hermes/node-compat/bundle/bundle_generation.h>
#include <hermes/node-compat/bundle/bundle_reader.h>
#include <hermes/node-compat/bundle/bundle_resolve.h>
#include <hermes/node-compat/bundle/bundle_writer.h>
#include <hermes/node-compat/bundle/cjs_wrapper.h>
#include <hermes/node-compat/bundle/native_digest.h>
#include <hermes/node-compat/bundle/require_scanner.h>

#include <napi/hermes_napi_compile.h>

#include <zlib.h>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace hermes {
namespace node_compat {

namespace {

namespace fs = std::filesystem;

bool isRegularFile(const std::string &path) {
  std::error_code ec;
  return fs::is_regular_file(path, ec) && !ec;
}

bool hasExtension(std::string_view path, std::string_view ext) {
  return path.size() >= ext.size() &&
      path.compare(path.size() - ext.size(), ext.size(), ext) == 0;
}

/// Reads the whole file at \p path into \p out. Returns false on any I/O
/// error, leaving \p out unspecified.
bool readFile(const std::string &path, std::string *out) {
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

/// Removes a leading `#!` line from \p source, in place.
///
/// A hashbang is legal only at the very start of a Program, and every
/// JavaScript module here is compiled inside the CommonJS wrapper below, so
/// an unstripped one is a syntax error that fails the build for a file that
/// runs perfectly from disk. `bin/` CLI scripts are exactly the shape that
/// carries one, and they are commonly extensionless.
///
/// The newline is deliberately kept, so every line after the hashbang keeps
/// its original number in stack traces. This mirrors what the two other
/// compile paths already do: libjs/loader.js's loadModule() and
/// compileFunctionForCJSLoaderCb in lib/bindings/node_contextify.cpp.
void stripShebang(std::string *source) {
  if (source->size() < 2 || (*source)[0] != '#' || (*source)[1] != '!')
    return;
  size_t nl = source->find('\n');
  if (nl == std::string::npos)
    source->clear();
  else
    source->erase(0, nl);
}

/// Returns the text of the exception pending on \p env, and clears it.
/// hermes_compile_to_bytecode reports a failure as a pending JS exception
/// rather than returning text (see hermes_napi_compile.h), and that text is
/// needed twice: in the build warning, and inside the stub the failure
/// produces (see makeThrowingStub).
std::string takeCompileErrorText(napi_env env) {
  bool pending = false;
  napi_is_exception_pending(env, &pending);
  if (!pending)
    return "compilation failed";
  napi_value exc;
  napi_get_and_clear_last_exception(env, &exc);
  napi_value msg;
  napi_coerce_to_string(env, exc, &msg);
  char buf[4096];
  size_t len = 0;
  napi_get_value_string_utf8(env, msg, buf, sizeof(buf), &len);
  return std::string(buf, len);
}

/// Escapes \p s for use inside a double-quoted JavaScript string literal.
///
/// Bytes at 0x80 and above are passed through unchanged: the input is the
/// UTF-8 text of a path or a compiler diagnostic, and the source this is
/// spliced into is read as UTF-8 too.
std::string quoteForJSString(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (unsigned char c : s) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (c < 0x20) {
          char esc[7];
          std::snprintf(esc, sizeof(esc), "\\u%04x", c);
          out += esc;
        } else {
          out += static_cast<char>(c);
        }
    }
  }
  return out;
}

/// The source of the module that stands in for \p path when this build
/// cannot turn it into bytecode -- because the parser or the compiler
/// rejected it with \p error.
///
/// Requiring such a file from disk throws a SyntaxError at exactly this
/// point, when the loader compiles it, so the stub throws one too. Carrying
/// the failure into the container rather than failing the build is what lets
/// a program be bundled when it never loads the offending file: real
/// packages ship files for other runtimes and other module systems
/// (`import()` inside a `.cjs` file is the case that motivated this) behind
/// branches the run never takes. The path is in the message because a
/// bundled module has no source file for a stack trace to name.
std::string makeThrowingStub(
    const std::string &path,
    const std::string &error) {
  // takeCompileErrorText returns the exception's toString(), which already
  // begins with its class name; the stub supplies that name itself, so
  // leaving the prefix in would read "SyntaxError: x.js: SyntaxError: ...".
  // The warning keeps the full text, where nothing else names the class.
  constexpr std::string_view kPrefix = "SyntaxError: ";
  std::string_view message(error);
  if (message.substr(0, kPrefix.size()) == kPrefix)
    message.remove_prefix(kPrefix.size());
  return "throw new SyntaxError(\"" +
      quoteForJSString(path + ": " + std::string(message)) + "\");\n";
}

/// What the producer does with a file the resolver handed back.
enum class Packageability {
  /// Compile to bytecode and store as a kJavaScript module.
  kJavaScript,
  /// Store the raw text as a kJSON module.
  kJSON,
  /// Record as a kNative module -- an addon whose bytes are not in the
  /// container at all, but are copied to a flat sidecar file beside it.
  kNative,
  /// Leave out of the container with a warning; the runtime reads it from
  /// disk instead.
  kSkip,
};

/// Classifies \p path by extension.
///
/// The resolver has already decided that \p path is the module a specifier
/// names (see resolveSpecifier), so the only question left is how to store
/// it. Everything the CommonJS loader would execute as JavaScript is
/// packaged as JavaScript:
///
/// - `.js` and `.cjs` are both CommonJS JavaScript. `.cjs` is what a package
///   with `"type": "module"` names its CommonJS files, so refusing it would
///   drop real JavaScript out of the bundle.
/// - No extension at all is JavaScript too. A bare file like
///   `node_modules/yargs/yargs` is a real, commonly-shipped module entry
///   point; the loader treats an unregistered extension (and no extension is
///   the limit case) as `.js`. There is nothing in the identity string to
///   infer a kind from, which is exactly why the container records each
///   module's kind explicitly rather than letting the consumer re-derive it.
///   Note that a dotfile lands here as well: `fs::path("/x/.babelrc")` has an
///   empty extension(), so `require('./.babelrc')` is compiled as JavaScript
///   and a build error is how a non-JavaScript one reports itself. That
///   matches Node and matches this repo's own loader, both of which treat a
///   dotfile as `.js`, but it is a build failure where the narrower
///   whitelist used to warn and skip.
/// - `.ts` is compiled with the TypeScript front end, as it is on disk.
///
/// `.mjs` is deliberately NOT packaged even though it is JavaScript: it is
/// ESM, and this runtime's CommonJS loader cannot execute ESM at all. There
/// is no fallback to preserve here -- `require()` of an .mjs throws whether
/// or not a bundle is involved -- so the reason to skip it is simpler than
/// that: its `import`/`export` syntax is a syntax error inside the CommonJS
/// wrapper below, so packaging one would fail the whole build over a module
/// that could never have run.
///
/// `.node` is packaged, but not the way the other two are: its bytes stay
/// out of the container and are copied to a flat sidecar file beside it,
/// because dlopen() takes a path and there is no portable way to load a
/// shared object from memory. What the container records is the identity,
/// the sidecar's name, and a digest -- see the native table in
/// bundle_format.h. Every other extension (assets, config formats a loader
/// hook might understand) is skipped for the reason the spec gives: it is
/// not JavaScript or JSON.
Packageability classifyFile(const std::string &path) {
  std::string ext = fs::path(path).extension().string();
  if (ext.empty() || ext == ".js" || ext == ".cjs" || ext == ".ts")
    return Packageability::kJavaScript;
  if (ext == ".json")
    return Packageability::kJSON;
  if (ext == ".node")
    return Packageability::kNative;
  return Packageability::kSkip;
}

/// Explains why \p resolvedPath (already classified as Packageability::
/// kSkip) is left out of the bundle. Shared by the build-time warning and
/// BuildReporter::skipped() so the two can never drift into different
/// wording for the same skip.
std::string formatSkipReason(const std::string &resolvedPath) {
  std::string ext = fs::path(resolvedPath).extension().string();
  if (ext == ".mjs")
    return ".mjs is ESM, not packageable";
  return ext + " is not packageable";
}

/// Why a specifier that resolved to a vendored package (see
/// isVendoredSpecifier) with no local install is left out of the bundle.
/// Shared by the build-time warning and BuildReporter::skipped() for the
/// same reason formatSkipReason() is: one wording, not two.
constexpr std::string_view kVendoredSkipReason =
    "vendored package, served by the runtime";

/// Everything the closing summary reports. A struct rather than a dozen
/// parameters because half of these are only knowable after
/// BundleWriter::serialize() has laid the container out, and a positional
/// argument list that long is a place for two size_t values to be swapped
/// silently.
struct BuildSummary {
  uint32_t modules = 0;
  uint32_t jsModules = 0;
  uint32_t jsonModules = 0;
  /// Native addons: modules of the container whose bytes are not in it.
  /// Counted in `modules` like any other, so the three kinds still sum to
  /// the total, but reported on a line of their own as well (see
  /// nativeBytes).
  uint32_t nativeModules = 0;
  /// The addons' total size, summed from the digests. Deliberately kept
  /// out of every other byte figure here: those describe the container,
  /// and these bytes are not in it -- they sit beside it as sidecar files.
  uint64_t nativeBytes = 0;
  uint32_t edges = 0;
  /// Distinct specifier strings across every edge. Lower than `edges`
  /// exactly when two files require the same thing by the same name, which
  /// is what makes it worth printing next to the edge count.
  uint32_t distinctSpecifiers = 0;
  /// Modules recorded in the container's preload table. Zero for the
  /// overwhelming majority of builds -- no --preload was given -- so the
  /// summary line only appears when this is non-zero.
  uint32_t preloads = 0;
  /// Entries in the container's string table, and its size in bytes. One
  /// table holds identities and specifiers together, so this is neither the
  /// module count nor the specifier count.
  size_t stringEntries = 0;
  size_t stringBytes = 0;
  /// The payload section's size: every module's payload, plus the padding
  /// that puts each one on its alignment boundary.
  size_t payloadBytes = 0;
  /// Bytecode only, unpadded, summed across the JavaScript modules. Lower
  /// than payloadBytes by the JSON text and the padding.
  size_t bytecodeBytes = 0;
  /// The single largest module payload, which is the line that answers
  /// "what is big in here" without reading the whole module table.
  std::string largestIdentity;
  size_t largestBytes = 0;
  /// The whole file, which exceeds the sections above by the header.
  size_t totalBytes = 0;
};

/// Data collected for one visited file during the graph walk (pass 1),
/// before module indices exist.
struct FileInfo {
  ModuleKind kind;
  /// For kJSON, the raw file text -- also the final payload. For
  /// kJavaScript, the raw (unwrapped) source text until the compile step
  /// (buildBundle's step 4) overwrites it with serialized bytecode. Always
  /// empty for kNative: an addon's bytes never enter the container, and
  /// reading a shared object that can be tens of megabytes into memory to
  /// then not use it would be the one cost this design exists to avoid.
  std::string payload;
  /// (specifier as written, resolved absolute target path) for every
  /// literal require() this file contains that resolved to a packageable
  /// file (see classifyFile). Builtins and skipped-with-a-warning
  /// specifiers are not recorded here.
  std::vector<std::pair<std::string, std::string>> edges;
  /// Passed straight to BundleWriter::addModule(). Defaults to
  /// requirable -- every module discovered by the walk below is something
  /// require() can load. The one exception is a package.json packaged
  /// afterward purely for the resolver (see the loop after the walk in
  /// buildBundle), which sets this to 0 so require() cannot see it even
  /// though its bytes are in the container.
  uint32_t flags = kRequirable;

  /// kNative only: the flat file name this addon is copied to, beside the
  /// bundle, and what the container records for it. Assigned by
  /// buildBundle's step 3b, which needs the root the identity is relative
  /// to and so cannot run during the walk.
  std::string sidecarName;
  /// kNative only: the identity that already owned `sidecarName`'s plain
  /// basename, when this addon had to take a disambiguated one. Empty
  /// otherwise -- and empty for every other kind. Kept so --verbose can
  /// say why a hash appeared in a file name instead of leaving it to be
  /// noticed.
  std::string sidecarCollidedWith;
  /// kNative only: length and SHA-256 of the addon as it was at build
  /// time, recorded in the container's native table for --dump and
  /// --verify-natives. Nothing on the run path reads them.
  NativeDigest digest;
};

/// Verbose build reporting. Every method is a no-op when disabled, so call
/// sites stay unconditional and the quiet path stays exactly as it was --
/// this class only ever prints to stderr, and never influences what
/// buildBundle discovers, compiles, or writes.
///
/// Labels in the configuration and summary blocks are padded to a fixed
/// column, which is what the design's samples show and what makes the
/// numbers in a summary scannable.
///
/// The discovery block is deliberately FLAT: `require`, `known` and `skip`
/// start in column 0, exactly like `discover`. Indenting them under the
/// nearest `discover` line above would assert a parentage that does not
/// hold, because a require is reported before the module it discovered is
/// (see buildBundle's pass B: resolved() then discovered()). Every require
/// line after the first therefore sits under the module its own target
/// opened, not under its importer, and an entry with two requires would show
/// the second one nested beneath the first one's target. The verbs are
/// padded to a common width, which aligns their specifiers without claiming
/// anything about structure.
///
/// Their `->` columns are NOT aligned to each other and module indices are
/// not zero-padded to the final module count: both would mean buffering the
/// walk, and the walk's output is streamed so that a long build says what it
/// is doing while it does it.
class BuildReporter {
 public:
  explicit BuildReporter(bool enabled) : enabled_(enabled) {}

  void config(
      const std::string &entry,
      const std::string &output,
      uint32_t generation,
      const std::string &generationDescription,
      bool optimized) {
    if (!enabled_)
      return;
    std::fprintf(stderr, "entry:      %s\n", entry.c_str());
    // Absolute, like the entry above: a build is reproduced from a shell
    // whose working directory is not recorded anywhere else in this output.
    std::fprintf(stderr, "output:     %s\n", output.c_str());
    // The tag is eight hex digits and says nothing about why two builds
    // disagree; the description beside it is what a later MISMATCH from
    // --dump can be read against.
    std::fprintf(
        stderr,
        "generation: 0x%08x (%s)\n",
        generation,
        generationDescription.c_str());
    std::fprintf(stderr, "optimize:   %s\n", optimized ? "on" : "off");
  }

  void discovered(uint32_t index, const std::string &path) {
    if (!enabled_)
      return;
    std::fprintf(stderr, "discover [%u] %s\n", index, path.c_str());
  }

  /// A module recorded to run before the entry. Reported separately from
  /// discovered() because being packaged and being a preload are different
  /// facts about the same module.
  void preloaded(uint32_t index, const std::string &path) {
    if (!enabled_)
      return;
    std::fprintf(stderr, "preload [%u] %s\n", index, path.c_str());
  }

  void resolved(const std::string &specifier, const std::string &target) {
    if (!enabled_)
      return;
    std::fprintf(
        stderr, "require '%s' -> %s\n", specifier.c_str(), target.c_str());
  }

  void known(const std::string &specifier, uint32_t index) {
    if (!enabled_)
      return;
    std::fprintf(stderr, "known   '%s' -> [%u]\n", specifier.c_str(), index);
  }

  void skipped(const std::string &specifier, const std::string &reason) {
    if (!enabled_)
      return;
    std::fprintf(
        stderr, "skip    '%s' (%s)\n", specifier.c_str(), reason.c_str());
  }

  /// A file packaged as a module that throws when required, because this
  /// build could not turn it into bytecode. Reported against the file
  /// rather than a specifier: the same file may be reached by several.
  void stubbed(const std::string &path, const std::string &reason) {
    if (!enabled_)
      return;
    std::fprintf(stderr, "stub    %s (%s)\n", path.c_str(), reason.c_str());
  }

  /// One use of require the scanner could not follow, at its source
  /// position. The default build reports only how many there were, because
  /// a large tree has many and drowning the actionable warnings in them
  /// would cost more than the positions are worth; this is where the
  /// positions live.
  void requireGap(const RequireGap &gap, const std::string &path) {
    if (!enabled_)
      return;
    std::fprintf(
        stderr,
        "%s %s:%u:%u\n",
        gap.kind == RequireGapKind::kComputedArgument ? "dynamic" : "escape ",
        path.c_str(),
        gap.line,
        gap.column);
  }

  void compiled(
      uint32_t index,
      const std::string &identity,
      size_t sourceBytes,
      size_t bytecodeBytes,
      double milliseconds) {
    if (!enabled_)
      return;
    totalCompileMs_ += milliseconds;
    // The ratio is the number a reader would otherwise compute by hand for
    // every line, and it is the one that identifies an outlier: a module
    // whose bytecode is several times the usual multiple of its source is
    // where a build's size went. sourceBytes is never zero -- every source
    // is wrapped in the CommonJS prologue before it is measured -- but a
    // division guarded by nothing is a division waiting to be moved.
    double ratio = sourceBytes != 0
        ? static_cast<double>(bytecodeBytes) / static_cast<double>(sourceBytes)
        : 0.0;
    std::fprintf(
        stderr,
        "compile [%u] %s %zu src -> %zu bc (%.1fx) %.2f ms\n",
        index,
        identity.c_str(),
        sourceBytes,
        bytecodeBytes,
        ratio,
        milliseconds);
  }

  /// One native addon placed beside the container. \p info is the walk's
  /// record for it, carrying the sidecar name and the digest; \p src and
  /// \p dst are absolute. \p inPlace says the two name the same file --
  /// the addon already sits at the bundle root under the name it would be
  /// copied to -- so nothing was written.
  void nativeCopied(
      const std::string &identity,
      const FileInfo &info,
      const std::string &src,
      const std::string &dst,
      bool inPlace) {
    if (!enabled_)
      return;
    std::fprintf(
        stderr,
        "native  %s -> %s%s\n",
        identity.c_str(),
        info.sidecarName.c_str(),
        inPlace ? " (already in place)" : "");
    // Said outright rather than left as a hash in a file name for someone
    // to notice and wonder about.
    if (!info.sidecarCollidedWith.empty()) {
      std::fprintf(
          stderr,
          "  name collides with %s from %s; using %s\n",
          fs::path(src).filename().string().c_str(),
          info.sidecarCollidedWith.c_str(),
          info.sidecarName.c_str());
    }
    if (inPlace) {
      std::fprintf(stderr, "  at   %s\n", src.c_str());
    } else {
      std::fprintf(stderr, "  from %s\n", src.c_str());
      std::fprintf(stderr, "  to   %s\n", dst.c_str());
    }
    // The whole hash, not a prefix: --verbose is where a number is printed
    // to be compared against another one, and half of a hash cannot be.
    std::fprintf(
        stderr,
        "  %u bytes sha256:%s\n",
        info.digest.byteLength,
        nativeDigestToHex(info.digest.raw).c_str());
  }

  void summary(const BuildSummary &s) {
    if (!enabled_)
      return;
    // The native count is on this line even though its bytes are not: the
    // three kinds have to sum to the total, or the breakdown is a puzzle.
    std::fprintf(
        stderr,
        "modules:    %u  (%u js, %u json, %u native)\n",
        s.modules,
        s.jsModules,
        s.jsonModules,
        s.nativeModules);
    std::fprintf(
        stderr,
        "edges:      %u  (%u distinct specifiers)\n",
        s.edges,
        s.distinctSpecifiers);
    if (s.preloads != 0)
      std::fprintf(stderr, "preloads:   %u\n", s.preloads);
    // Separate from every other byte figure below, and labelled with where
    // those bytes actually are: a reader who added this into `total:` would
    // be describing a file that does not exist. Only when there are any, in
    // the register `preloads:` above already uses.
    if (s.nativeModules != 0) {
      std::fprintf(
          stderr,
          "natives:    %u %s, %llu bytes alongside (not in the container)\n",
          s.nativeModules,
          s.nativeModules == 1 ? "file" : "files",
          static_cast<unsigned long long>(s.nativeBytes));
    }
    std::fprintf(
        stderr,
        "strings:    %zu %s, %zu bytes\n",
        s.stringEntries,
        s.stringEntries == 1 ? "entry" : "entries",
        s.stringBytes);
    std::fprintf(stderr, "payload:    %zu bytes\n", s.payloadBytes);
    std::fprintf(stderr, "bytecode:   %zu bytes\n", s.bytecodeBytes);
    std::fprintf(
        stderr,
        "largest:    %s  %zu bytes\n",
        s.largestIdentity.c_str(),
        s.largestBytes);
    std::fprintf(stderr, "total:      %zu bytes\n", s.totalBytes);
    std::fprintf(stderr, "compile:    %.2f ms\n", totalCompileMs_);
  }

 private:
  bool enabled_;
  double totalCompileMs_ = 0;
};

} // namespace

int buildBundle(
    napi_env env,
    const std::string &entryPath,
    const std::string &outPath,
    bool verbose,
    const std::vector<std::string> &includes,
    const std::vector<std::string> &preloads) {
  BuildReporter reporter(verbose);
  uint32_t generation = bundleGenerationTag();

  // Step 1: make the entry path absolute and verify it names a regular
  // file. fs::absolute() only prepends the current directory when
  // entryPath is relative; it never resolves symlinks or otherwise touches
  // entryPath itself, matching resolveSpecifier's own no-realpath policy
  // (see bundle_resolve.h) so a bundle built from a path reached through a
  // symlinked directory stays predictable.
  std::error_code ec;
  fs::path absEntryPath = fs::absolute(fs::path(entryPath), ec);
  if (ec) {
    std::fprintf(
        stderr,
        "error: cannot resolve entry path %s: %s\n",
        entryPath.c_str(),
        ec.message().c_str());
    return 1;
  }
  std::string absEntry = absEntryPath.lexically_normal().string();
  if (!isRegularFile(absEntry)) {
    std::fprintf(
        stderr, "error: entry is not a regular file: %s\n", absEntry.c_str());
    return 1;
  }
  // A .json entry would be recorded as a kJSON module and set as the
  // bundle's entry -- the build would succeed and produce a valid-looking
  // container, but the consumer has no way to execute a JSON value as the
  // CommonJS entry point. A .node addon or an .mjs file is not something
  // this loader can execute at all. Reject any of them here, before any
  // work is done, rather than producing an unrunnable bundle.
  // "CommonJS" is the load-bearing word: an .mjs file IS JavaScript, and
  // saying only "JavaScript" would make its rejection look like a bug.
  if (classifyFile(absEntry) != Packageability::kJavaScript) {
    std::fprintf(
        stderr,
        "error: entry must be a CommonJS JavaScript or TypeScript file: %s\n",
        absEntry.c_str());
    return 1;
  }

  // Absolute for the same reason absEntry is: a build report has to be
  // readable from somewhere other than the shell that produced it, and the
  // working directory it was relative to is recorded nowhere else. Purely
  // for the report -- outPath itself is still written exactly as given.
  std::error_code outEc;
  fs::path absOutPath = fs::absolute(fs::path(outPath), outEc);
  reporter.config(
      absEntry,
      outEc ? outPath : absOutPath.lexically_normal().string(),
      generation,
      bundleGenerationDescription(),
      /*optimized=*/true);

  // Step 2: worklist walk of the whole require() graph. `paths` records
  // discovery order -- also the order module indices are assigned in pass 2
  // below, once the walk (and thus the full file set the root is computed
  // over) is complete. `pathIndex` is the same discovery-order index kept
  // as a lookup, for BuildReporter::known() to report which already-visited
  // module a specifier landed on. `files` accumulates each visited file's
  // kind, payload, and outgoing edges.
  std::vector<std::string> paths{absEntry};
  std::unordered_map<std::string, uint32_t> pathIndex{{absEntry, 0}};
  std::unordered_map<std::string, FileInfo> files;
  // One instance for the whole walk (not one per resolveSpecifier call):
  // readPackageJsonPaths() accumulates on this object as resolution reads
  // package.json files, and the loop below (after the walk) packages
  // whatever it recorded.
  DiskFileSource disk;
  size_t computedRequires = 0;
  size_t filesWithComputedRequires = 0;
  size_t escapedRequires = 0;
  size_t filesWithEscapedRequires = 0;
  reporter.discovered(0, absEntry);

  // Seed --include's extra roots before the walk starts, so the worklist
  // loop below walks each of them exactly as it walks the entry -- there is
  // no second walk. Resolved from absEntry the same way a require() inside
  // the entry would be, and against the same `disk` instance the walk uses,
  // so a package.json read while resolving an include is recorded and
  // packaged like any other.
  for (const std::string &spec : includes) {
    std::optional<std::string> resolved =
        resolveSpecifier(disk, absEntry, spec);
    if (!resolved) {
      std::fprintf(
          stderr, "error: --include=%s cannot be resolved\n", spec.c_str());
      return 1;
    }
    // A .node is Packageability::kNative, not kSkip, so it passes here:
    // --include=<addon>.node is exactly how an addon reached by a computed
    // path (bindings, node-gyp-build) gets into the container.
    if (classifyFile(*resolved) == Packageability::kSkip) {
      std::fprintf(
          stderr,
          "error: --include=%s resolves to %s, which is not packageable "
          "(%s)\n",
          spec.c_str(),
          resolved->c_str(),
          formatSkipReason(*resolved).c_str());
      return 1;
    }
    if (pathIndex.count(*resolved) != 0)
      continue; // already reached from the entry
    pathIndex.emplace(*resolved, static_cast<uint32_t>(paths.size()));
    paths.push_back(*resolved);
    reporter.discovered(static_cast<uint32_t>(paths.size() - 1), *resolved);
  }

  // --preload is a third caller of the seed-a-root mechanism above: the
  // walk below packages it exactly as it packages the entry and each
  // --include. What makes it a preload is the index recorded here. A
  // recorded preload that was not packaged would be a container that
  // cannot run, which is why this seeds rather than requiring the user to
  // pass --include as well.
  std::vector<std::string> preloadPaths;
  for (const std::string &spec : preloads) {
    std::optional<std::string> resolved =
        resolveSpecifier(disk, absEntry, spec);
    if (!resolved) {
      std::fprintf(
          stderr, "error: --preload=%s cannot be resolved\n", spec.c_str());
      return 1;
    }
    if (classifyFile(*resolved) == Packageability::kSkip) {
      std::fprintf(
          stderr,
          "error: --preload=%s resolves to %s, which is not packageable "
          "(%s)\n",
          spec.c_str(),
          resolved->c_str(),
          formatSkipReason(*resolved).c_str());
      return 1;
    }
    // The same module named twice runs once -- the second load is a
    // Module._cache hit -- so recording it twice would promise something
    // the loader cannot do.
    if (std::find(preloadPaths.begin(), preloadPaths.end(), *resolved) !=
        preloadPaths.end())
      continue;
    preloadPaths.push_back(*resolved);
    if (pathIndex.count(*resolved) != 0)
      continue; // already reached from the entry or an --include
    pathIndex.emplace(*resolved, static_cast<uint32_t>(paths.size()));
    paths.push_back(*resolved);
    reporter.discovered(static_cast<uint32_t>(paths.size() - 1), *resolved);
  }

  for (size_t i = 0; i < paths.size(); ++i) {
    // A copy, not a reference: the loop body appends newly discovered paths
    // to `paths` itself (below), and a reallocation would invalidate a
    // reference into it out from under this iteration.
    std::string path = paths[i];

    // A native addon is a leaf of the graph: nothing scans it, nothing
    // compiles it, and it contributes no edges. It is also the one visited
    // file that is never read into memory here -- a shared object can be
    // tens of megabytes and none of those bytes belong in the container.
    // The digest is streamed instead, which is both the record the native
    // table wants and the read that proves the file is there and readable,
    // at the point in the build where saying so is still cheap.
    if (classifyFile(path) == Packageability::kNative) {
      FileInfo info;
      info.kind = ModuleKind::kNative;
      std::string digestError;
      std::optional<NativeDigest> digest = nativeFileDigest(path, &digestError);
      if (!digest) {
        std::fprintf(stderr, "error: %s\n", digestError.c_str());
        return 1;
      }
      info.digest = std::move(*digest);
      files.emplace(path, std::move(info));
      continue;
    }

    std::string source;
    if (!readFile(path, &source)) {
      std::fprintf(stderr, "error: cannot read %s\n", path.c_str());
      return 1;
    }

    FileInfo info;
    // Only packageable files ever reach the worklist (the entry check above
    // and the edge filter below both reject the rest), so the kind follows
    // straight from the same classification.
    if (classifyFile(path) == Packageability::kJSON) {
      info.kind = ModuleKind::kJSON;
      info.payload = std::move(source);
      files.emplace(path, std::move(info));
      continue;
    }

    info.kind = ModuleKind::kJavaScript;
    // Before the scan, not just before the compile, so the text the parser
    // sees and the text that becomes bytecode are the same string.
    stripShebang(&source);
    bool isTS = hasExtension(path, ".ts");

    std::vector<std::string> specifiers;
    std::vector<RequireGap> gaps;
    std::string parseError;
    if (!scanRequires(source, isTS, &specifiers, &parseError, &gaps)) {
      // The entry is the one file the program is certain to load, so a
      // build that cannot read it has produced nothing runnable and fails.
      // A preload is certain to run too -- it is not reached through the
      // require() graph the tolerant-stub policy below is written for, it
      // is seeded onto the worklist and always executed by run() before the
      // entry -- so it gets the same hard error, not a throwing stub that
      // would only surface the SyntaxError once the run already can't
      // recover from it. Every other file may never be reached at run
      // time, and packaging it as a module that throws when required
      // reproduces what running from disk does: nothing at all unless
      // something requires it, and the same SyntaxError if something does.
      // See makeThrowingStub.
      bool isPreload =
          std::find(preloadPaths.begin(), preloadPaths.end(), path) !=
          preloadPaths.end();
      if (i == 0 || isPreload) {
        std::fprintf(
            stderr,
            "error: failed to parse %s %s: %s\n",
            i == 0 ? "entry" : "preload",
            path.c_str(),
            parseError.c_str());
        return 1;
      }
      std::fprintf(
          stderr,
          "warning: cannot parse %s (%s); packaged as a module that throws "
          "when required\n",
          path.c_str(),
          parseError.c_str());
      reporter.stubbed(path, "does not parse");
      info.payload = makeThrowingStub(path, parseError);
      files.emplace(path, std::move(info));
      continue;
    }

    // A use of require this scan could not follow names something absent
    // from the container and answered by the run-time fallback -- correct
    // wherever the source tree is still there, and a hole in the bundle the
    // moment it is not. The walk cannot close it; what it can do is stop
    // being silent about it. Counted here and reported once at the end: a
    // large tree has many, and a line each would bury the warnings a user
    // can act on.
    if (!gaps.empty()) {
      bool computedHere = false;
      bool escapedHere = false;
      for (const RequireGap &gap : gaps) {
        reporter.requireGap(gap, path);
        if (gap.kind == RequireGapKind::kComputedArgument) {
          ++computedRequires;
          computedHere = true;
        } else {
          ++escapedRequires;
          escapedHere = true;
        }
      }
      filesWithComputedRequires += computedHere;
      filesWithEscapedRequires += escapedHere;
    }

    // Pass A: resolve and classify every specifier, reporting (and, for a
    // hard error, returning) before anything is added to the graph.
    // `keep` collects only the ones that survive -- resolved to a
    // packageable file -- in source order, for pass B below.
    std::vector<std::pair<std::string, std::string>> keep;
    for (const std::string &specifier : specifiers) {
      if (isBuiltinSpecifier(specifier))
        continue;

      std::optional<std::string> resolved =
          resolveSpecifier(disk, path, specifier);
      if (!resolved) {
        // A vendored package ('ws', and anything else realm.js lists
        // alongside it) is embedded in the binary, so the runtime serves it
        // whether or not a node_modules copy exists -- and it survives the
        // tree being deleted, like a builtin. Only an *installed* copy is
        // bundleable, which is why this is not in the skip set the builtin
        // check above uses: when one is installed it is resolved and
        // packaged like any other dependency, and the bundle's version
        // wins, which is the anti-shadowing rule. This branch is the
        // other direction -- nothing on disk to package -- where failing the
        // build would refuse to bundle a program that runs fine.
        if (isVendoredSpecifier(specifier)) {
          std::string reason(kVendoredSkipReason);
          std::fprintf(
              stderr,
              "warning: not packaging '%s' from %s (%s)\n",
              specifier.c_str(),
              path.c_str(),
              reason.c_str());
          reporter.skipped(specifier, reason);
          continue;
        }
        // Nothing on disk to package, and nothing this build can say about
        // whether that is a defect: an unresolvable literal require() is
        // how a package probes for an optional dependency, and the
        // MODULE_NOT_FOUND it throws is caught and handled by the program
        // that wrote it. Leaving the specifier out of the edge table hands
        // it to the run-time loader, which resolves it against the
        // filesystem and throws exactly that error when it is still absent
        // -- the same outcome as running from disk, and a successful load
        // when the dependency has since been installed. Failing the build
        // here would refuse to bundle a program that runs fine.
        std::string reason("cannot be resolved, left to the run-time loader");
        std::fprintf(
            stderr,
            "warning: not packaging '%s' from %s (%s)\n",
            specifier.c_str(),
            path.c_str(),
            reason.c_str());
        reporter.skipped(specifier, reason);
        continue;
      }
      if (classifyFile(*resolved) == Packageability::kSkip) {
        std::string reason = formatSkipReason(*resolved);
        std::fprintf(
            stderr,
            "warning: skipping %s (%s)\n",
            resolved->c_str(),
            reason.c_str());
        reporter.skipped(specifier, reason);
        continue;
      }

      keep.emplace_back(specifier, *resolved);
    }

    // Pass B: add an edge for every survivor, and discover or (report as)
    // recognize its target. Split from pass A so that every skip this file
    // causes is reported before any known/discover line it causes -- e.g.
    // a file that both requires a shared dependency and a native addon
    // reports the addon skip first, the shared-dependency recognition
    // second, regardless of which require() came first in the source.
    // resolved() is reported here, not in pass A, so a resolution is
    // immediately followed by its outcome (discover/known) in the
    // narration instead of every require() in the file being reported
    // before any of their outcomes.
    for (const auto &[specifier, target] : keep) {
      reporter.resolved(specifier, target);
      info.edges.emplace_back(specifier, target);
      auto it = pathIndex.find(target);
      if (it == pathIndex.end()) {
        uint32_t idx = static_cast<uint32_t>(paths.size());
        pathIndex.emplace(target, idx);
        paths.push_back(target);
        reporter.discovered(idx, target);
      } else {
        reporter.known(specifier, it->second);
      }
    }

    info.payload = std::move(source);
    files.emplace(path, std::move(info));
  }

  // Package every package.json resolveSpecifier read while answering a
  // "main" field. Nothing require()s these -- a bundle's closed-world
  // property otherwise depends on a source tree that stays on disk after
  // the build, since a future run-time resolver needs the same "main"
  // answers the producer got.
  //
  // Not every one of these is kept, though. `moduleDirs` below is built
  // from the entries `paths` held from the require() walk alone, before
  // this loop starts appending to it -- that is the set this loop tests
  // ancestry against, so a package.json kept by one iteration cannot make
  // a later iteration keep another that would otherwise be dropped. A
  // candidate is kept only when its directory is an ancestor of (or equal
  // to) some packaged module's directory; otherwise it is dropped. This is
  // not "kept iff already under the root", because the root has not been
  // computed yet -- it is computed below, from `paths` after this loop, so
  // a kept package.json outside a shallower module's directory can still
  // widen the root to cover it (e.g. a package whose entire reachable
  // module graph lives under its own `lib/` subdirectory: the package.json
  // sits one level higher than every module, so it is an ancestor of all
  // of them, is kept, and pulls the root up to the package directory,
  // which is exactly where the consumer needs it for a dynamic resolution
  // of that package by name). A candidate that is an ancestor of nothing
  // packaged -- the original bug this loop's filter exists for, e.g. a
  // failed require() probe into an unrelated node_modules/foo that never
  // got packaged -- is dropped.
  //
  // What dropping guarantees, exactly: no packaged module sits under that
  // directory, so no *module* the consumer serves is made unreachable. It
  // does not guarantee that no resolution can want the file, because a
  // package.json is reached as a resolution INPUT rather than as a module,
  // and the two reachabilities are not the same. A package whose "main"
  // escapes its own directory ("main": "../../../outside/real.js") has its
  // module packaged elsewhere and its own directory an ancestor of nothing
  // packaged, so its package.json is dropped and a run-time computed
  // require of that package BY NAME then throws MODULE_NOT_FOUND. That is
  // the known residual case, left as is deliberately: the shape is
  // pathological, and the alternative -- keeping every package.json any
  // probe ever read -- is the bug this filter was added to fix, where an
  // unrelated failed probe widened the root and renamed every identity in
  // the container.
  size_t moduleCount = paths.size();
  // The module directories, computed once. The filter below is
  // O(candidates x modules), and rebuilding fs::path(paths[i])
  // .parent_path() inside the inner loop parsed and allocated a path per
  // pair; a literal require.resolve() being a discovery edge only grows
  // the module count, which is the multiplicand.
  std::vector<fs::path> moduleDirs;
  moduleDirs.reserve(moduleCount);
  for (size_t i = 0; i < moduleCount; ++i)
    moduleDirs.push_back(fs::path(paths[i]).parent_path());
  for (const std::string &pkgPath : disk.readPackageJsonPaths()) {
    if (pathIndex.count(pkgPath) != 0)
      continue; // the program requires it too; it is already kRequirable.
    fs::path pkgDir = fs::path(pkgPath).parent_path();
    bool isAncestorOfSomeModule = false;
    for (const fs::path &moduleDir : moduleDirs) {
      fs::path relative = moduleDir.lexically_relative(pkgDir);
      if (!relative.empty() && relative.begin()->string() != "..") {
        isAncestorOfSomeModule = true;
        break;
      }
    }
    if (!isAncestorOfSomeModule)
      continue; // an ancestor of nothing packaged; see comment above.
    std::string text;
    if (!readFile(pkgPath, &text))
      continue; // read once already; a disappearance now is not fatal.
    FileInfo info;
    info.kind = ModuleKind::kJSON;
    info.flags = kResolveOnly; // require() must not see it.
    info.payload = std::move(text);
    pathIndex.emplace(pkgPath, static_cast<uint32_t>(paths.size()));
    paths.push_back(pkgPath);
    files.emplace(pkgPath, std::move(info));
  }

  // Step 3: compute and announce the build root -- the longest path prefix
  // shared by every visited file's directory (modules and the package.json
  // files kept just above). Module identities are relative to it, and the
  // consumer recovers it from the bundle file's own directory, which is
  // why the bundle has to sit at the printed root. Once, after the walk,
  // rather than per site during it -- and naming --verbose, because that is
  // where the positions are. Two lines rather than a combined one: a
  // computed argument leaves a call site the reader can go and look at,
  // while an escaped require leaves only the point where it stopped being
  // traceable, and the second is the worse news.
  if (computedRequires != 0) {
    std::fprintf(
        stderr,
        "warning: %zu computed require()/require.resolve() %s in %zu %s: "
        "not packaged; "
        "answered at run time only if the container already holds the "
        "target, else --include it (--verbose lists them)\n",
        computedRequires,
        computedRequires == 1 ? "call" : "calls",
        filesWithComputedRequires,
        filesWithComputedRequires == 1 ? "file" : "files");
  }
  if (escapedRequires != 0) {
    std::fprintf(
        stderr,
        "warning: require used as a value in %zu %s in %zu %s: whatever it "
        "goes on to load is not packaged (--verbose lists them)\n",
        escapedRequires,
        escapedRequires == 1 ? "place" : "places",
        filesWithEscapedRequires,
        filesWithEscapedRequires == 1 ? "file" : "files");
  }

  std::string root = commonAncestor(paths);
  std::printf("bundle root: %s\n", root.c_str());
  fs::path rootPath(root);

  // Step 3b: give every native addon a flat sidecar name and work out where
  // its copy goes. Flat, rather than mirroring the identity's own subtree:
  // the whole point of shipping a bundle is a small countable set of files,
  // and "bundle plus tree" is not a better distribution unit than a tree
  // (see history/plans/2026-08-21-bundle-natives-design.md). One
  // consequence worth knowing: every native lands in one directory, so an
  // addon whose RPATH is $ORIGIN-relative finds a sibling .so the user
  // drops beside the bundle.
  //
  // Naming happens here, next to the root it needs; the copying happens at
  // step 5b, once everything that can fail the build has run. Nothing in
  // this block writes a file.
  fs::path sidecarDir = fs::path(outPath).parent_path();
  std::unordered_map<std::string, std::string> sidecarOwner; // name -> id
  auto identityOf = [&rootPath](const std::string &path) {
    return fs::path(path).lexically_relative(rootPath).generic_string();
  };

  // Pass 1: an addon that ALREADY is the file it would be copied to gets
  // first claim on its plain basename. Discovery order must not decide
  // this. Consider a project with proj/binding.node beside its entry and
  // node_modules/foo/build/Release/binding.node underneath -- the most
  // ordinary pair of addon paths there is. If foo's copy is discovered
  // first and takes the plain name, its destination IS the other addon's
  // source file, and the copy overwrites a file in the user's source tree
  // with the wrong shared object. Giving the in-place candidate the name
  // makes that pair cost zero writes instead of one destructive one.
  for (const std::string &path : paths) {
    FileInfo &info = files.at(path);
    if (info.kind != ModuleKind::kNative)
      continue;
    std::string base = fs::path(path).filename().string();
    // Guarded against a name already claimed because two identities can be
    // the same file (a hard link, or a symlink into the tree): the second
    // one falls through to pass 2 and takes a hashed name of its own.
    if (sidecarOwner.count(base) == 0 &&
        isSameFile(path, (sidecarDir / base).string())) {
      info.sidecarName = base;
      sidecarOwner.emplace(base, identityOf(path));
    }
  }

  // Pass 2: everyone else, in discovery order.
  for (const std::string &path : paths) {
    FileInfo &info = files.at(path);
    if (info.kind != ModuleKind::kNative || !info.sidecarName.empty())
      continue;
    std::string identity = identityOf(path);
    std::string base = fs::path(path).filename().string();
    std::string name = base;
    auto owner = sidecarOwner.find(name);
    if (owner != sidecarOwner.end()) {
      // Two identities with the same basename -- two packages each shipping
      // their own build/Release/binding.node is the ordinary shape of this.
      // Disambiguate with a short hash of the identity, which is stable
      // across builds. The map from identity to sidecar name is recorded in
      // the container, so this rule can change later without invalidating a
      // bundle that already exists.
      info.sidecarCollidedWith = owner->second;
      char suffix[16];
      std::snprintf(
          suffix,
          sizeof(suffix),
          "-%08x",
          static_cast<unsigned>(crc32(
              0L,
              reinterpret_cast<const Bytef *>(identity.data()),
              static_cast<uInt>(identity.size()))));
      fs::path p(base);
      name = p.stem().string() + suffix + p.extension().string();
      // Two identities whose hashes collide, or an addon literally named
      // like a disambiguated one. Both are absurd and neither is
      // impossible, and the alternative to failing is writing one addon's
      // bytes over another's and shipping the wrong file -- which nothing
      // downstream would catch, since the container names only the sidecar.
      auto second = sidecarOwner.find(name);
      if (second != sidecarOwner.end()) {
        std::fprintf(
            stderr,
            "error: native addons %s and %s both want the sidecar file %s\n",
            second->second.c_str(),
            identity.c_str(),
            name.c_str());
        return 1;
      }
    }
    sidecarOwner.emplace(name, identity);
    info.sidecarName = name;
  }

  // Pass 3: destinations, in discovery order (which is the order the
  // `native:` lines and the native table come out in).
  struct NativePlacement {
    /// Index into `paths`, so the addon's source path, FileInfo and module
    /// index are all one lookup away.
    size_t index;
    /// Absolute or as-given, matching outPath's own spelling.
    std::string dst;
    /// The addon already IS `dst`: nothing to copy. A flat project whose
    /// entry and addon sit in one directory, built into that directory, is
    /// the common case and not a mistake. Copying would rewrite one of the
    /// build's own inputs with a fresh inode, mtime and possibly different
    /// permission bits, for no gain.
    bool inPlace;
  };
  std::vector<NativePlacement> nativeCopies;
  for (size_t i = 0; i < paths.size(); ++i) {
    const FileInfo &info = files.at(paths[i]);
    if (info.kind != ModuleKind::kNative)
      continue;
    std::string dst = (sidecarDir / info.sidecarName).string();

    // Refuse to write over the container itself: an addon whose sidecar
    // name collides with the bundle's own would otherwise have the build
    // destroy its own output. Two tests because neither alone is enough --
    // the (st_dev, st_ino) one --extract-module --out uses catches a
    // rebuild, where the container from last time is still there, and the
    // spelling comparison catches the first build, where neither file
    // exists yet and stat() has nothing to say. dst is always
    // sidecarDir / name, so the two spellings are equal exactly when the
    // sidecar name is the bundle's own basename.
    if (isSameFile(dst, outPath) ||
        fs::path(dst).lexically_normal() ==
            fs::path(outPath).lexically_normal()) {
      std::fprintf(
          stderr,
          "error: native addon %s would be written over the bundle %s\n",
          paths[i].c_str(),
          outPath.c_str());
      return 1;
    }
    // isSameFile() stat()s, so it follows symlinks -- correct for the
    // name-claim and overwrite checks above, which ask "would this write
    // land somewhere it must not?", and wrong here, where the question is
    // "is there nothing to do?". A symlink at the sidecar path pointing at
    // the addon's source answers same-file, the copy is skipped, and the
    // "sidecar" stays a link into the source tree: shipping the output
    // directory alone then gives a bundle that throws, and --verify-natives
    // (which reads through the link) calls it OK. So a symlink is never in
    // place; it gets overwritten by a real copy.
    std::error_code linkEc;
    const bool dstIsSymlink =
        fs::is_symlink(fs::symlink_status(fs::path(dst), linkEc));
    const bool inPlace = !dstIsSymlink && isSameFile(paths[i], dst);
    nativeCopies.push_back({i, dst, inPlace});
  }

  // A structural check behind pass 1's policy: no addon's destination may
  // be another addon's source. Pass 1 is what makes this hold, and if it is
  // ever weakened -- or defeated by a shape nobody thought of -- the
  // failure is silent and destructive: an input file overwritten, and a
  // container whose native table describes bytes that are no longer in the
  // sidecar (the digest was taken during the walk, and nothing on the run
  // path reads it). Quadratic in the number of natives, which is one or
  // two.
  for (const NativePlacement &copy : nativeCopies) {
    if (copy.inPlace)
      continue;
    for (const NativePlacement &other : nativeCopies) {
      if (other.index == copy.index ||
          !isSameFile(copy.dst, paths[other.index]))
        continue;
      std::fprintf(
          stderr,
          "error: the sidecar for native addon %s (%s) is the source file of "
          "native addon %s\n",
          identityOf(paths[copy.index]).c_str(),
          copy.dst.c_str(),
          identityOf(paths[other.index]).c_str());
      return 1;
    }
  }

  // Step 4: compile every JavaScript file to bytecode, replacing its raw
  // source payload in place. optimize is unconditionally true: this is an
  // ahead-of-time artifact with no interactive fast path to protect.
  size_t totalBytecodeBytes = 0;
  for (size_t i = 0; i < paths.size(); ++i) {
    const std::string &path = paths[i];
    FileInfo &info = files.at(path);
    if (info.kind != ModuleKind::kJavaScript)
      continue;

    std::string wrapped = wrapCJS(info.payload);

    hermes_compile_flags cflags{};
    cflags.struct_size = sizeof(cflags);
    cflags.optimize = true;
    cflags.enable_ts = hasExtension(path, ".ts");

    uint8_t *bytecodeData = nullptr;
    size_t bytecodeSize = 0;
    // hermes_compile_to_bytecode counts the NUL terminator in source_size
    // (see hermes_napi_compile.h); std::string::data() is NUL-terminated
    // since C++11, so size() + 1 is exactly that count.
    auto compileStart = std::chrono::steady_clock::now();
    napi_status status = hermes_compile_to_bytecode(
        env,
        reinterpret_cast<const uint8_t *>(wrapped.data()),
        wrapped.size() + 1,
        path.c_str(),
        &cflags,
        &bytecodeData,
        &bytecodeSize);
    auto compileEnd = std::chrono::steady_clock::now();
    if (status != napi_ok) {
      // The pending exception has to be taken before anything else is
      // compiled on this env, and it is the text both the warning and the
      // stub carry. Entry and preload handling matches the parse failure
      // above, for the same reason: both are certain to run.
      std::string error = takeCompileErrorText(env);
      bool isPreload =
          std::find(preloadPaths.begin(), preloadPaths.end(), path) !=
          preloadPaths.end();
      if (i == 0 || isPreload) {
        std::fprintf(
            stderr,
            "error: failed to compile %s %s: %s\n",
            i == 0 ? "entry" : "preload",
            path.c_str(),
            error.c_str());
        return 1;
      }
      std::fprintf(
          stderr,
          "warning: cannot compile %s (%s); packaged as a module that throws "
          "when required\n",
          path.c_str(),
          error.c_str());
      reporter.stubbed(path, "does not compile");

      wrapped = wrapCJS(makeThrowingStub(path, error));
      // The stub is plain JavaScript whatever the file's extension was.
      cflags.enable_ts = false;
      status = hermes_compile_to_bytecode(
          env,
          reinterpret_cast<const uint8_t *>(wrapped.data()),
          wrapped.size() + 1,
          path.c_str(),
          &cflags,
          &bytecodeData,
          &bytecodeSize);
      if (status != napi_ok) {
        std::fprintf(
            stderr,
            "error: internal: the stub for %s does not compile: %s\n",
            path.c_str(),
            takeCompileErrorText(env).c_str());
        return 1;
      }
    }

    reporter.compiled(
        static_cast<uint32_t>(i),
        fs::path(path).lexically_relative(rootPath).generic_string(),
        wrapped.size(),
        bytecodeSize,
        std::chrono::duration<double, std::milli>(compileEnd - compileStart)
            .count());
    totalBytecodeBytes += bytecodeSize;

    info.payload.assign(
        reinterpret_cast<const char *>(bytecodeData), bytecodeSize);
    hermes_free_bytecode(bytecodeData);
  }

  // Step 5: now that the full file set (and its root) is known, assign
  // module indices -- one pass to add every module, a second to add every
  // edge, since an edge references a target module's index and every index
  // must exist before any edge can be recorded.
  BundleWriter writer;
  std::unordered_map<std::string, uint32_t> moduleIndex;
  // Filled in as the container is assembled, and completed below once
  // serialize() has laid it out. Accumulated unconditionally rather than
  // under `verbose`: every field here is a counter or a comparison over
  // data this loop already walks, and computing it twice -- once here and
  // once in a verbose-only pass -- is how the two answers drift apart.
  BuildSummary summary;
  std::set<std::string> distinctSpecifiers;
  for (const std::string &path : paths) {
    const FileInfo &info = files.at(path);
    std::string identity =
        fs::path(path).lexically_relative(rootPath).generic_string();
    if (info.kind == ModuleKind::kJSON)
      ++summary.jsonModules;
    else if (info.kind == ModuleKind::kNative)
      ++summary.nativeModules;
    else
      ++summary.jsModules;
    if (info.payload.size() > summary.largestBytes) {
      summary.largestBytes = info.payload.size();
      summary.largestIdentity = identity;
    }
    uint32_t idx =
        writer.addModule(identity, info.kind, info.flags, info.payload);
    moduleIndex.emplace(path, idx);
  }
  // Every module now has an index, so the preload table -- which stores
  // indices, not paths -- can be filled in, in the order --preload was
  // given (preloadPaths already collapsed duplicates to one entry each).
  for (const std::string &p : preloadPaths) {
    uint32_t idx = moduleIndex.at(p);
    writer.addPreload(idx);
    reporter.preloaded(idx, p);
  }
  summary.preloads = static_cast<uint32_t>(preloadPaths.size());
  // The native table, keyed by the same discovery-order module indices the
  // preload table above uses -- which is why it is filled in here, once
  // every module has one. A kNative module without a row is a container the
  // reader refuses to open (see BundleReader::open), so this loop and the
  // module loop above must walk the same set.
  for (const NativePlacement &copy : nativeCopies) {
    const std::string &path = paths[copy.index];
    const FileInfo &info = files.at(path);
    writer.addNative(
        moduleIndex.at(path),
        info.sidecarName,
        info.digest.byteLength,
        info.digest.raw);
    summary.nativeBytes += info.digest.byteLength;
  }
  uint32_t edgeCount = 0;
  for (const std::string &path : paths) {
    const FileInfo &info = files.at(path);
    uint32_t importerIdx = moduleIndex.at(path);
    for (const auto &edge : info.edges) {
      const std::string &specifier = edge.first;
      const std::string &target = edge.second;
      writer.addEdge(importerIdx, specifier, moduleIndex.at(target));
      distinctSpecifiers.insert(specifier);
      ++edgeCount;
    }
  }
  writer.setEntry(moduleIndex.at(absEntry));
  summary.modules = static_cast<uint32_t>(paths.size());
  summary.edges = edgeCount;
  summary.distinctSpecifiers = static_cast<uint32_t>(distinctSpecifiers.size());
  summary.bytecodeBytes = totalBytecodeBytes;

  // Step 5b: copy every native addon next to the bundle. Last, deliberately:
  // the container is written temp-then-rename so that a failed build cannot
  // damage a good artifact, and a sidecar is half of that same artifact.
  // Copying eagerly -- before the compile, which is the step most likely to
  // fail -- means a failed rebuild in a deployment directory leaves the
  // PREVIOUS build's container beside THIS build's sidecars, a mismatch
  // nothing on the run path detects (the digests are --verify-natives only).
  // The cost is compile time spent on a build that then fails to place its
  // sidecars; the alternative cost is mutating a directory the user may
  // still be running from. Everything above this point that can fail has
  // already run -- serialize() cannot fail (see the assert below) -- so the
  // two writes are effectively adjacent.
  for (const NativePlacement &copy : nativeCopies) {
    const std::string &src = paths[copy.index];
    const FileInfo &info = files.at(src);
    std::string identity = identityOf(src);

    if (!copy.inPlace) {
      std::string contents;
      if (!readFile(src, &contents)) {
        std::fprintf(stderr, "error: cannot read %s\n", src.c_str());
        return 1;
      }
      // Temp file then rename, like the container's own write, so a failure
      // never leaves a half-written shared object beside a good bundle --
      // which dlopen() would reject with an error about the addon rather
      // than about the build that truncated it.
      if (!writeFileAtomically(
              copy.dst, contents.data(), contents.size(), std::cerr))
        return 1;
      // Preserve the source's mode bits so a 0644 copy of a 0755 addon does
      // not surprise anyone reading `ls -l`. dlopen() itself needs only
      // read permission, so a failure on either call is not worth failing
      // the build over -- but the stat has to be checked before its result
      // is used, because perms::unknown handed to permissions() means 0777
      // rather than "leave it alone".
      std::error_code statEc;
      fs::file_status srcStatus = fs::status(src, statEc);
      if (!statEc) {
        std::error_code modeEc;
        fs::permissions(copy.dst, srcStatus.permissions(), modeEc);
      }
    }

    // Stdout, under `bundle root:`: this is part of the artifact's
    // description, not a diagnostic. Printed after the copy, so the line
    // means the file is there.
    std::printf(
        "native: %s (from %s)\n", info.sidecarName.c_str(), identity.c_str());
    reporter.nativeCopied(identity, info, src, copy.dst, copy.inPlace);
  }
  if (!nativeCopies.empty()) {
    // Packaging a native changes the distribution contract from "one file"
    // to "one file plus these", and a build that changed it silently would
    // be the same class of failure this subsystem exists to remove -- found
    // on the deployment machine instead of at build time.
    std::printf(
        "note: this bundle requires %zu native addon%s alongside it; ship "
        "them together.\n",
        nativeCopies.size(),
        nativeCopies.size() == 1 ? "" : "s");
  }

  // Step 6: serialize and write via a temp file + rename, so a build that
  // fails partway through never leaves a partial bundle at outPath.
  std::vector<uint8_t> bytes = writer.serialize(generation);
  // BundleWriter::serialize() returns empty only when no module was ever
  // added or setEntry() was never called (see bundle_writer.h). Both are
  // unreachable here: the entry-extension check above guarantees at least
  // one JS/TS entry file, so paths is never empty, and the module/edge pass
  // above always adds every visited path and always calls setEntry(). A
  // violation here would be a bug in this function, not a user-triggerable
  // error, so this is an assert rather than a runtime check.
  assert(!bytes.empty() && "serialize() unexpectedly produced no output");

  // The summary is reported here rather than before serialize(), because
  // three of its lines describe the laid-out container and do not exist
  // until it has been laid out. Still before the write: the report is about
  // what was built, not about whether the rename succeeded, and a failed
  // write reports itself.
  summary.stringEntries = writer.stringCount();
  summary.totalBytes = bytes.size();
  // Section sizes read back out of the finished container rather than
  // recomputed here, so this summary and a later `--dump` of the same file
  // cannot disagree about how big its sections are: both read the header.
  // Gated on `verbose` because nothing else reads these two numbers, and a
  // quiet build must stay exactly the work it is today.
  if (verbose) {
    std::string readbackError;
    std::optional<BundleReader> readback = BundleReader::openForInspection(
        bytes.data(), bytes.size(), &readbackError);
    if (readback) {
      summary.stringBytes = readback->stringsSize();
      summary.payloadBytes = readback->payloadSize();
    } else {
      // Unreachable short of a bug in BundleWriter or BundleReader, and
      // worth one line rather than silently reporting two zeroes: the
      // container about to be written is one this binary's own reader
      // rejects.
      std::fprintf(
          stderr,
          "warning: cannot read back the container just built: %s\n",
          readbackError.c_str());
    }
  }
  reporter.summary(summary);

  // Reported through std::cerr, matching the atomic-write helper's
  // interface for its other caller (bundle_tools.cpp's extractModule),
  // which has no stdio stream of its own to print to.
  if (!writeFileAtomically(outPath, bytes.data(), bytes.size(), std::cerr))
    return 1;

  return 0;
}

} // namespace node_compat
} // namespace hermes
