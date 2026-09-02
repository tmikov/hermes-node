/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <hermes/node-compat/bundle/bundle_tools.h>

#include <hermes/node-compat/bundle/atomic_write.h>
#include <hermes/node-compat/bundle/bundle_reader.h>
#include <hermes/node-compat/bundle/native_digest.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace hermes {
namespace node_compat {

namespace fs = std::filesystem;

namespace {

/// "0x" and eight lowercase hex digits, the spelling every generation tag
/// in the tooling uses.
std::string hex32(uint32_t value) {
  std::ostringstream s;
  s << "0x" << std::hex << std::setfill('0') << std::setw(8) << value;
  return s.str();
}

/// The reader validated every module's kind field at open time, so no
/// fourth value can reach here today. It still prints the number rather
/// than falling back to one of the three names: a tool whose whole job is
/// telling the truth about a file must not report a kind it does not
/// recognize as JavaScript.
std::string kindName(ModuleKind kind) {
  switch (kind) {
    case ModuleKind::kJavaScript:
      return "js";
    case ModuleKind::kJSON:
      return "json";
    case ModuleKind::kNative:
      return "native";
  }
  return "?" + std::to_string(static_cast<uint32_t>(kind));
}

/// What the "kind" column prints for module \p moduleIndex: the kind name,
/// plus a "resolve-only" marker when kRequirable is clear. Such a record
/// (a package.json the resolver read but require() must never reach, see
/// kRequirable in bundle_format.h) is otherwise indistinguishable from an
/// ordinary module in this table, and the whole point of the marker is that
/// it not be.
std::string kindColumn(const BundleReader &reader, uint32_t moduleIndex) {
  std::string label = kindName(reader.kind(moduleIndex));
  if (!reader.isRequirable(moduleIndex))
    label += " resolve-only";
  return label;
}

/// How many characters \p value occupies when printed.
size_t widthOf(uint64_t value) {
  return std::to_string(value).size();
}

/// Levenshtein distance between \p a and \p b (single-character insert,
/// delete or replace, unit cost each). Implemented locally rather than
/// pulled from llvh's ComputeEditDistance: the tools layer's whole point is
/// staying free of any Hermes dependency (see hermesNodeBundleTools in
/// lib/bundle/CMakeLists.txt), and a "did you mean" list is little enough
/// code that vendoring the algorithm is not worth the coupling.
size_t levenshtein(std::string_view a, std::string_view b) {
  std::vector<size_t> prev(b.size() + 1);
  std::vector<size_t> cur(b.size() + 1);
  for (size_t j = 0; j <= b.size(); ++j)
    prev[j] = j;
  for (size_t i = 1; i <= a.size(); ++i) {
    cur[0] = i;
    for (size_t j = 1; j <= b.size(); ++j) {
      size_t cost = a[i - 1] == b[j - 1] ? 0 : 1;
      cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost});
    }
    std::swap(prev, cur);
  }
  return prev[b.size()];
}

/// Up to three identities in \p reader closest to \p identity by edit
/// distance, dropping any candidate farther than a third of \p identity's
/// length. That budget is what keeps a wild typo -- one that does not name
/// anything close to a real identity -- from producing three suggestions
/// anyway just because the list would otherwise be short; a real typo of a
/// real (long) identity still lands comfortably inside it.
std::vector<std::string> suggestIdentities(
    const BundleReader &reader,
    const std::string &identity) {
  const size_t maxDist = identity.size() / 3;
  std::vector<std::pair<size_t, std::string>> ranked;
  for (uint32_t i = 0, n = reader.moduleCount(); i < n; ++i) {
    std::string candidate(reader.identity(i));
    size_t dist = levenshtein(identity, candidate);
    if (dist <= maxDist)
      ranked.emplace_back(dist, std::move(candidate));
  }
  // Stable: candidates at equal distance keep the container's own module
  // order rather than an arbitrary one.
  std::stable_sort(
      ranked.begin(), ranked.end(), [](const auto &a, const auto &b) {
        return a.first < b.first;
      });
  if (ranked.size() > 3)
    ranked.resize(3);

  std::vector<std::string> result;
  result.reserve(ranked.size());
  for (auto &entry : ranked)
    result.push_back(std::move(entry.second));
  return result;
}

/// Reports \p reason for the container at \p path on \p err.
///
/// The reader's messages ("hermes-node bundle: truncated (shorter than the
/// header)") describe the class of failure and never the file, because one
/// reader serves the run path too, where the container is the program and
/// naming it adds nothing. A diagnostic tool is the other case: it is
/// pointed at a file the user is asking about, often one of several, so the
/// answer has to say which one. MappedFile's own errors already name the
/// path, and --dump-bytecode prefixes every one of its messages with it, so
/// this is what keeps one class of failure from naming the file or not
/// depending on which layer happened to catch it.
void reportContainerError(
    std::ostream &err,
    const std::string &path,
    const std::string &reason) {
  err << "error: " << path << ": " << reason << "\n";
}

/// The mapping plus the validated reader over it, held together because
/// every tool verb that reads a container needs both for as long as it
/// runs: dumpBundle() reads file->size() for its own total, and
/// extractModule() and verifyNatives() read only through \c reader, but all
/// three need the mapping to outlive it.
struct OpenedBundle {
  MappedFile file;
  BundleReader reader;
};

/// Maps \p bundlePath and validates it through
/// BundleReader::openForInspection -- not open(): a container this binary
/// would refuse to run is still one worth describing, extracting from, or
/// checking the sidecars of. One copy of the open/map/validate sequence for
/// dumpBundle(), extractModule() and verifyNatives(), all three of which
/// used to paste it (the first two did; this factors it out before a third
/// copy could exist).
///
/// Reports the reason on \p err, in reportContainerError()'s shape, and
/// returns std::nullopt on any failure. Deliberately not release()'d: none
/// of the three callers' mappings outlive the call that opened them, unlike
/// the run path's, which stays mapped for the life of the process.
std::optional<OpenedBundle> openBundleForTool(
    const std::string &bundlePath,
    std::ostream &err) {
  std::string error;
  std::optional<MappedFile> file = MappedFile::open(bundlePath, &error);
  if (!file) {
    err << "error: " << error << "\n";
    return std::nullopt;
  }

  std::optional<BundleReader> reader =
      BundleReader::openForInspection(file->data(), file->size(), &error);
  if (!reader) {
    reportContainerError(err, bundlePath, error);
    return std::nullopt;
  }

  return OpenedBundle{std::move(*file), std::move(*reader)};
}

} // namespace

int dumpBundle(
    const std::string &bundlePath,
    uint32_t runningGeneration,
    bool verbose,
    std::ostream &out,
    std::ostream &err) {
  // Inspection mode: structural validation is unchanged, but the generation
  // tag is reported below rather than enforced.
  std::optional<OpenedBundle> opened = openBundleForTool(bundlePath, err);
  if (!opened)
    return 1;
  MappedFile *file = &opened->file;
  BundleReader *reader = &opened->reader;

  const uint32_t moduleCount = reader->moduleCount();
  const uint32_t edgeCount = reader->edgeCount();

  out << "bundle: " << bundlePath << "   format v" << reader->formatVersion()
      << "  generation " << hex32(reader->generationTag()) << "\n";
  out << "entry:  [" << reader->entry() << "] "
      << reader->identity(reader->entry()) << "\n";
  if (reader->generationTag() != runningGeneration) {
    // Below both header lines rather than beside the tag on the first one:
    // this says something about the binary doing the dumping, not about the
    // container, and the container's own description comes first.
    out << "generation: " << hex32(reader->generationTag())
        << "  MISMATCH (this binary requires " << hex32(runningGeneration)
        << ")\n";
  }

  // Edges per module, in and out. Only the verbose columns use them, so
  // only verbose pays for the walk.
  std::vector<uint32_t> edgesIn(moduleCount, 0);
  std::vector<uint32_t> edgesOut(moduleCount, 0);
  if (verbose) {
    for (uint32_t i = 0; i < edgeCount; ++i) {
      BundleReader::EdgeView e = reader->edge(i);
      // Both indices were range-checked when the container was opened.
      ++edgesOut[e.importer];
      ++edgesIn[e.target];
    }
  }

  // Every column is as wide as the widest value actually in it (or its
  // heading, whichever is wider), rather than a fixed guess: identities run
  // to the depth of a node_modules path, and a guessed width would wrap
  // them onto a second line and cost the table its shape.
  size_t idxWidth = std::strlen("idx");
  size_t kindWidth = std::strlen("kind");
  size_t bytesWidth = std::strlen("bytes");
  size_t inWidth = std::strlen("in");
  size_t outWidth = std::strlen("out");
  for (uint32_t i = 0; i < moduleCount; ++i) {
    idxWidth = std::max(idxWidth, widthOf(i));
    kindWidth = std::max(kindWidth, kindColumn(*reader, i).size());
    bytesWidth = std::max(bytesWidth, widthOf(reader->payload(i).size()));
    if (verbose) {
      inWidth = std::max(inWidth, widthOf(edgesIn[i]));
      outWidth = std::max(outWidth, widthOf(edgesOut[i]));
    }
  }

  out << "\nMODULES (" << moduleCount << ")\n";
  out << "  " << std::right << std::setw(idxWidth) << "idx" << "  " << std::left
      << std::setw(kindWidth) << "kind" << "  " << std::right
      << std::setw(bytesWidth) << "bytes";
  if (verbose) {
    out << "  " << std::setw(inWidth) << "in" << "  " << std::setw(outWidth)
        << "out";
  }
  out << "  identity\n";
  for (uint32_t i = 0; i < moduleCount; ++i) {
    out << "  " << std::right << std::setw(idxWidth) << i << "  " << std::left
        << std::setw(kindWidth) << kindColumn(*reader, i) << "  " << std::right
        << std::setw(bytesWidth) << reader->payload(i).size();
    if (verbose) {
      out << "  " << std::setw(inWidth) << edgesIn[i] << "  "
          << std::setw(outWidth) << edgesOut[i];
    }
    out << "  " << reader->identity(i) << "\n";
  }

  size_t importerWidth = 0;
  size_t specifierWidth = 0;
  for (uint32_t i = 0; i < edgeCount; ++i) {
    BundleReader::EdgeView e = reader->edge(i);
    importerWidth =
        std::max(importerWidth, reader->identity(e.importer).size());
    // Plus the two quotes the specifier is printed inside.
    specifierWidth = std::max(specifierWidth, e.specifier.size() + 2);
  }

  out << "\nEDGES (" << edgeCount << ")\n";
  // In table order, which is the sorted order the runtime binary
  // binary-searches: by importer index, then by specifier bytes. Regrouping
  // these by importer would read more nicely and would hide a sort bug --
  // and a sort bug is the one defect this dump is uniquely able to reveal,
  // because a wrongly ordered table makes require() miss at run time with
  // nothing else to point at.
  for (uint32_t i = 0; i < edgeCount; ++i) {
    BundleReader::EdgeView e = reader->edge(i);
    std::string quoted = "'" + std::string(e.specifier) + "'";
    out << "  " << std::left << std::setw(importerWidth)
        << reader->identity(e.importer) << "  " << std::setw(specifierWidth)
        << quoted << "  -> [" << e.target << "]\n";
  }

  // Only when there is at least one preload, so an ordinary container's
  // dump -- the overwhelming majority, until something produces preloads --
  // is unchanged.
  const uint32_t preloadCount = reader->preloadCount();
  if (preloadCount > 0) {
    out << "\nPRELOADS (" << preloadCount << ")\n";
    for (uint32_t i = 0; i < preloadCount; ++i) {
      uint32_t moduleIndex = reader->preload(i);
      out << "  [" << moduleIndex << "] " << reader->identity(moduleIndex)
          << "\n";
    }
  }

  // Same rule as PRELOADS above, for the same reason: no natives is still
  // the overwhelming majority of containers, and their dump must stay
  // exactly what it was before this table existed.
  const uint32_t nativeCount = reader->nativeCount();
  if (nativeCount > 0) {
    out << "\nNATIVES (" << nativeCount << ")\n";
    for (uint32_t i = 0; i < nativeCount; ++i) {
      BundleReader::NativeView native = reader->native(i);
      // Full 64 hex characters under --verbose; otherwise the first 16 (8
      // bytes), enough to tell two builds of the same addon apart at a
      // glance without the row wrapping. verifyNatives() prints the full
      // digest unconditionally instead -- there each row already stands
      // alone as a verdict (OK/MISSING/MISMATCH) rather than one line in an
      // inventory, so the extra width does not compete with anything.
      std::string digestHex = nativeDigestToHex(native.digest);
      // resize() shrinks here and never pads: BundleReader::openImpl()
      // rejects a native digest that is not exactly kNativeDigestBytes
      // (bundle_format.h) before this code ever runs, so digestHex is
      // always the full 64 hex characters at this point.
      if (!verbose)
        digestHex.resize(16);
      out << "  [" << native.moduleIndex << "] "
          << reader->identity(native.moduleIndex) << "\n";
      out << "      sidecar " << native.sidecar << "  " << native.byteLength
          << " bytes  sha256:" << digestHex << "\n";
    }
  }

  // Nearly the same rule as PRELOADS and NATIVES above -- a container with
  // no VM configuration at all must dump exactly as it did before this
  // section existed -- with one addition those two have no equivalent of:
  // the override bit is VM configuration even when no options accompany
  // it. A container built with --allow-vm-options-override and no --vm=
  // honours HERMES_NODE_VM_OPTIONS unconditionally, -enable-eval=true and
  // -Xhermes-internal-test-methods=true included, which is the single most
  // important thing an audit before shipping should surface. Printing only
  // on vmOptionCount > 0 said nothing at all about exactly that artifact.
  const uint32_t vmOptionCount = reader->vmOptionCount();
  if (vmOptionCount > 0 || reader->allowsVmOptionsOverride()) {
    out << "\nVM_OPTIONS (" << vmOptionCount << ")\n";
    out << "  overrides: "
        << (reader->allowsVmOptionsOverride() ? "allowed" : "locked") << "\n";
    for (uint32_t i = 0; i < vmOptionCount; ++i)
      out << "  " << reader->vmOption(i) << "\n";
  }

  const uint32_t strings = reader->stringsSize();
  const uint32_t modules = reader->moduleTableSize();
  const uint32_t edges = reader->edgeTableSize();
  const uint32_t preloads = reader->preloadTableSize();
  const uint32_t natives = reader->nativeTableSize();
  const uint32_t payload = reader->payloadSize();
  const uint32_t vmopts = reader->vmOptionsTableSize();
  size_t sectionWidth = std::max(
      {widthOf(strings),
       widthOf(modules),
       widthOf(edges),
       widthOf(preloads),
       widthOf(natives),
       widthOf(payload),
       widthOf(vmopts)});

  out << "\nSECTIONS\n";
  out << "  strings  " << std::right << std::setw(sectionWidth) << strings
      << " B    modules  " << std::setw(sectionWidth) << modules << " B\n";
  out << "  edges    " << std::setw(sectionWidth) << edges << " B    payload  "
      << std::setw(sectionWidth) << payload << " B\n";
  out << "  natives  " << std::setw(sectionWidth) << natives
      << " B    preloads " << std::setw(sectionWidth) << preloads << " B\n";
  // Unconditional, like natives and preloads above: a VM-options table is
  // just as real a section as either of them even when its count is zero.
  out << "  vmopts   " << std::setw(sectionWidth) << vmopts << " B\n";
  // The size of the file, which is larger than the seven sections add up to:
  // the header, and the padding that puts each payload on its alignment
  // boundary, belong to neither. Unconditional, like the natives row above
  // it: a preload table is just as real a section as a native table even
  // when its count is zero, which is the whole reason this row exists --
  // the old comment claimed the total exceeded the sections by header and
  // padding alone, which was already false for any container with
  // preloads, since their table's bytes were counted in the total but
  // named in no row.
  out << "total " << file->size() << " bytes\n";

  return 0;
}

int extractModule(
    const std::string &bundlePath,
    const std::string &identity,
    const std::string &outPath,
    std::ostream &err) {
  // Before the container is even opened, let alone written over. A module's
  // payload is a fraction of the container it came from, and the write is a
  // rename onto --out, so extracting onto the container replaces it with a
  // piece of itself -- irreversibly, silently (the mapping holds the old
  // inode, so nothing downstream notices), and with a zero exit status. The
  // headline case for these verbs is a container built by another
  // hermes-node, which is precisely the one the user cannot rebuild.
  if (isSameFile(outPath, bundlePath)) {
    err << "error: --out=" << outPath << " names the same file as the bundle "
        << bundlePath
        << "; extracting a module onto its own container would destroy it\n";
    return 1;
  }

  // Inspection mode, exactly like dumpBundle: getting bytecode out of a
  // container the current binary refuses to run is a reason to have this
  // feature, not a reason to withhold it.
  std::optional<OpenedBundle> opened = openBundleForTool(bundlePath, err);
  if (!opened)
    return 1;
  BundleReader *reader = &opened->reader;

  std::optional<uint32_t> found;
  for (uint32_t i = 0, n = reader->moduleCount(); i < n; ++i) {
    if (reader->identity(i) == identity) {
      found = i;
      break;
    }
  }

  if (!found) {
    err << "error: no module '" << identity << "' in " << bundlePath << "\n";
    std::vector<std::string> suggestions = suggestIdentities(*reader, identity);
    if (!suggestions.empty()) {
      err << "did you mean:\n";
      for (const std::string &s : suggestions)
        err << "  " << s << "\n";
    }
    return 1;
  }

  // A native's bytes are never in the container -- they ship as a flat
  // sidecar file beside the bundle, because dlopen() takes a path and there
  // is no portable way to load a shared object from memory (see
  // openBundle() in bundle_run.cpp). Extracting one would write an empty
  // file: a tool whose whole job is describing a container must not lie
  // about a file it exists to describe by reporting success on nothing.
  // Named by nativeFor() rather than by re-deriving it, and checked before
  // any write, exactly like the same-file guard above.
  if (reader->kind(*found) == ModuleKind::kNative) {
    std::optional<BundleReader::NativeView> native = reader->nativeFor(*found);
    err << "error: '" << identity
        << "' is a native addon; its bytes are not in the container.\n"
        << "It ships alongside the bundle as "
        << (native ? native->sidecar : std::string_view("<unknown>")) << ".\n";
    return 1;
  }

  // Verbatim: no header, no transformation. That is what makes a
  // JavaScript extraction directly loadable and a JSON extraction
  // byte-identical to its source.
  std::string_view payload = reader->payload(*found);
  if (!writeFileAtomically(outPath, payload.data(), payload.size(), err))
    return 1;

  return 0;
}

int verifyNatives(
    const std::string &bundlePath,
    bool verbose,
    std::ostream &out,
    std::ostream &err) {
  // Inspection mode, exactly like dumpBundle and extractModule: a container
  // this binary refuses to run is still one whose sidecars are worth
  // checking.
  std::optional<OpenedBundle> opened = openBundleForTool(bundlePath, err);
  if (!opened)
    return 1;
  BundleReader *reader = &opened->reader;

  // The same rule the run-time loader uses to turn a recorded sidecar name
  // into a path it dlopen()s (openBundle() in bundle_run.cpp): sidecars
  // live flat, beside the container, in its own directory. realpath'd
  // first, exactly as openBundle() does, so a bundle reached through a
  // symlinked directory is checked in the same place the run would look --
  // otherwise this verb could report OK (or MISSING) about a directory the
  // run never consults, which is a worse failure than not having the verb
  // at all. If this ever drifts from openBundle()'s sequence, the two
  // stop agreeing on where a sidecar lives.
  std::error_code canonicalEc;
  fs::path canonicalBundle = fs::canonical(fs::path(bundlePath), canonicalEc);
  const fs::path sidecarDir = canonicalEc
      ? fs::absolute(fs::path(bundlePath)).parent_path()
      : canonicalBundle.parent_path();

  const uint32_t nativeCount = reader->nativeCount();
  if (nativeCount == 0) {
    // Said out loud rather than exiting 0 in silence: in a CI log, no output
    // at all is indistinguishable from the verb never having run.
    out << "no native addons recorded\n";
    return 0;
  }
  uint32_t failed = 0;
  for (uint32_t i = 0; i < nativeCount; ++i) {
    BundleReader::NativeView native = reader->native(i);
    const std::string sidecar(native.sidecar);
    const std::string identity(reader->identity(native.moduleIndex));
    const std::string sidecarPath = (sidecarDir / sidecar).string();

    // Streamed, not read whole: an addon can be tens of megabytes and
    // nothing here needs the bytes themselves, only their hash.
    std::string digestError;
    std::optional<NativeDigest> digest =
        nativeFileDigest(sidecarPath, &digestError);

    // nativeFileDigest() has three failure modes, not one: the file is not
    // there, the file is there but unreadable (a directory at the path, a
    // permission bit, an I/O error), and the file is absurdly large. Only
    // the first is MISSING. The other two get ERROR and carry the reason on
    // the row itself, because the operator reading a CI log greps the status
    // word first and cannot re-run the build to get --verbose after the
    // fact. exists() rather than parsing errno out of the message: it is one
    // extra stat on a verb that already reads the whole file, and a race
    // between the two is not a distinction an audit needs to make.
    const char *status;
    std::string rowSuffix;
    if (!digest) {
      std::error_code existsEc;
      const bool present = fs::exists(fs::path(sidecarPath), existsEc);
      status = present ? "ERROR" : "MISSING";
      if (present)
        rowSuffix = ": " + digestError;
      ++failed;
    } else if (
        digest->byteLength != native.byteLength ||
        digest->raw != native.digest) {
      status = "MISMATCH";
      ++failed;
    } else {
      status = "OK";
    }

    // Column format fixed at these widths (not sized to the widest value
    // present, unlike dumpBundle's tables): a sidecar name is a flat
    // basename by construction, so there is no deep node_modules path here
    // to force a wider column, and a fixed layout is what makes the output
    // diffable across runs against different bundles.
    out << std::left << std::setw(8) << status << ' ' << std::left
        << std::setw(24) << sidecar << " (" << identity << ")" << rowSuffix
        << "\n";

    // Under --verbose, every entry gets its expected and actual length and
    // hash -- including OK ones: a verification whose passing case shows
    // nothing is hard to trust.
    if (verbose) {
      out << "    expected " << native.byteLength
          << " bytes sha256:" << nativeDigestToHex(native.digest) << "\n";
      if (digest) {
        out << "    actual   " << digest->byteLength
            << " bytes sha256:" << nativeDigestToHex(digest->raw) << "\n";
      } else {
        out << "    actual   (" << digestError << ")\n";
      }
    }
  }

  if (failed > 0) {
    err << "error: " << failed << " of " << nativeCount << " native addon"
        << (nativeCount == 1 ? "" : "s") << " failed verification\n";
    return 1;
  }
  return 0;
}

} // namespace node_compat
} // namespace hermes
