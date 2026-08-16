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
#include <hermes/node-compat/bundle/require_scanner.h>

#include <napi/hermes_napi_compile.h>

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

/// The module wrapper libjs/loader.js applies to a user file read from disk
/// (see loadModule() there). Every JavaScript module compiled into the
/// bundle is wrapped identically, so a compiled function value invoked as
/// (exports, require, module, __filename, __dirname) behaves the same way
/// whether the loader got it from the bundle or from a fresh disk compile.
constexpr std::string_view kWrapperPrefix =
    "(function(exports, require, module, __filename, __dirname) {";
constexpr std::string_view kWrapperSuffix = "\n})";

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
/// that could never have run. Every other extension (`.node` addons, assets,
/// config formats a loader hook might understand) is skipped for the reason
/// the spec gives: it is not JavaScript or JSON.
Packageability classifyFile(const std::string &path) {
  std::string ext = fs::path(path).extension().string();
  if (ext.empty() || ext == ".js" || ext == ".cjs" || ext == ".ts")
    return Packageability::kJavaScript;
  if (ext == ".json")
    return Packageability::kJSON;
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
  uint32_t edges = 0;
  /// Distinct specifier strings across every edge. Lower than `edges`
  /// exactly when two files require the same thing by the same name, which
  /// is what makes it worth printing next to the edge count.
  uint32_t distinctSpecifiers = 0;
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

  void summary(const BuildSummary &s) {
    if (!enabled_)
      return;
    std::fprintf(
        stderr,
        "modules:    %u  (%u js, %u json)\n",
        s.modules,
        s.jsModules,
        s.jsonModules);
    std::fprintf(
        stderr,
        "edges:      %u  (%u distinct specifiers)\n",
        s.edges,
        s.distinctSpecifiers);
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

/// Data collected for one visited file during the graph walk (pass 1),
/// before module indices exist.
struct FileInfo {
  ModuleKind kind;
  /// For kJSON, the raw file text -- also the final payload. For
  /// kJavaScript, the raw (unwrapped) source text until the compile step
  /// (buildBundle's step 4) overwrites it with serialized bytecode.
  std::string payload;
  /// (specifier as written, resolved absolute target path) for every
  /// literal require() this file contains that resolved to a packageable
  /// file (see classifyFile). Builtins and skipped-with-a-warning
  /// specifiers are not recorded here.
  std::vector<std::pair<std::string, std::string>> edges;
};

} // namespace

int buildBundle(
    napi_env env,
    const std::string &entryPath,
    const std::string &outPath,
    bool verbose) {
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
  reporter.discovered(0, absEntry);

  for (size_t i = 0; i < paths.size(); ++i) {
    // A copy, not a reference: the loop body appends newly discovered paths
    // to `paths` itself (below), and a reallocation would invalidate a
    // reference into it out from under this iteration.
    std::string path = paths[i];

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
    std::string parseError;
    if (!scanRequires(source, isTS, &specifiers, &parseError)) {
      // The entry is the one file the program is certain to load, so a
      // build that cannot read it has produced nothing runnable and fails.
      // Every other file may never be reached at run time, and packaging it
      // as a module that throws when required reproduces what running from
      // disk does: nothing at all unless something requires it, and the
      // same SyntaxError if something does. See makeThrowingStub.
      if (i == 0) {
        std::fprintf(
            stderr,
            "error: failed to parse %s: %s\n",
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

    // Pass A: resolve and classify every specifier, reporting (and, for a
    // hard error, returning) before anything is added to the graph.
    // `keep` collects only the ones that survive -- resolved to a
    // packageable file -- in source order, for pass B below.
    std::vector<std::pair<std::string, std::string>> keep;
    for (const std::string &specifier : specifiers) {
      if (isBuiltinSpecifier(specifier))
        continue;

      std::optional<std::string> resolved = resolveSpecifier(path, specifier);
      if (!resolved) {
        // A vendored package ('ws', and anything else realm.js lists
        // alongside it) is embedded in the binary, so the runtime serves it
        // whether or not a node_modules copy exists -- and it survives the
        // tree being deleted, like a builtin. Only an *installed* copy is
        // bundleable, which is why this is not in the skip set the builtin
        // check above uses: when one is installed it is resolved and
        // packaged like any other dependency, and the bundle's version wins,
        // which is the Task 6 anti-shadowing decision. This branch is the
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

  // Step 3: compute and announce the build root -- the longest path prefix
  // shared by every visited file's directory. The consumer (Task 6) needs
  // this to know which directory a disk-fallback path (e.g. for a skipped
  // native addon) is relative to.
  std::string root = commonAncestor(paths);
  std::printf("bundle root: %s\n", root.c_str());
  fs::path rootPath(root);

  // Step 4: compile every JavaScript file to bytecode, replacing its raw
  // source payload in place. optimize is unconditionally true: this is an
  // ahead-of-time artifact with no interactive fast path to protect.
  size_t totalBytecodeBytes = 0;
  for (size_t i = 0; i < paths.size(); ++i) {
    const std::string &path = paths[i];
    FileInfo &info = files.at(path);
    if (info.kind != ModuleKind::kJavaScript)
      continue;

    std::string wrapped;
    wrapped.reserve(
        kWrapperPrefix.size() + info.payload.size() + kWrapperSuffix.size());
    wrapped.append(kWrapperPrefix);
    wrapped.append(info.payload);
    wrapped.append(kWrapperSuffix);

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
      // stub carry. Entry handling matches the parse failure above.
      std::string error = takeCompileErrorText(env);
      if (i == 0) {
        std::fprintf(
            stderr,
            "error: failed to compile %s: %s\n",
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

      wrapped.assign(kWrapperPrefix);
      wrapped.append(makeThrowingStub(path, error));
      wrapped.append(kWrapperSuffix);
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
    else
      ++summary.jsModules;
    if (info.payload.size() > summary.largestBytes) {
      summary.largestBytes = info.payload.size();
      summary.largestIdentity = identity;
    }
    uint32_t idx = writer.addModule(identity, info.kind, info.payload);
    moduleIndex.emplace(path, idx);
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
