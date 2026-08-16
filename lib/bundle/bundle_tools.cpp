/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <hermes/node-compat/bundle/bundle_tools.h>

#include <hermes/node-compat/bundle/atomic_write.h>
#include <hermes/node-compat/bundle/bundle_reader.h>

#include <sys/stat.h>

#include <algorithm>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace hermes {
namespace node_compat {

namespace {

/// "0x" and eight lowercase hex digits, the spelling every generation tag
/// in the tooling uses.
std::string hex32(uint32_t value) {
  std::ostringstream s;
  s << "0x" << std::hex << std::setfill('0') << std::setw(8) << value;
  return s.str();
}

/// The reader validated every module's kind field at open time, so no
/// third value can reach here today. It still prints the number rather
/// than falling back to one of the two names: a tool whose whole job is
/// telling the truth about a file must not report a kind it does not
/// recognize as JavaScript.
std::string kindName(ModuleKind kind) {
  switch (kind) {
    case ModuleKind::kJavaScript:
      return "js";
    case ModuleKind::kJSON:
      return "json";
  }
  return "?" + std::to_string(static_cast<uint32_t>(kind));
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

/// True if \p a and \p b both exist and name the same file -- same device,
/// same inode.
///
/// Compares identity rather than spelling because the spellings that reach
/// the same file are unbounded: "app.hbb" and "./app.hbb", a relative and an
/// absolute path, a path through a symlinked directory, a symlink to the
/// container, and a second hard link to it. stat() follows symlinks, so all
/// of those collapse to one comparison here.
///
/// A path that cannot be stat()'d (most often because it does not exist,
/// which is the normal case for an output file) is not the same file as
/// anything, so the answer is false and the caller carries on.
bool isSameFile(const std::string &a, const std::string &b) {
  struct stat sa {};
  struct stat sb {};
  if (::stat(a.c_str(), &sa) != 0 || ::stat(b.c_str(), &sb) != 0)
    return false;
  return sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
}

} // namespace

int dumpBundle(
    const std::string &bundlePath,
    uint32_t runningGeneration,
    bool verbose,
    std::ostream &out,
    std::ostream &err) {
  std::string error;
  std::optional<MappedFile> file = MappedFile::open(bundlePath, &error);
  if (!file) {
    err << "error: " << error << "\n";
    return 1;
  }

  // Inspection mode: structural validation is unchanged, but the generation
  // tag is reported below rather than enforced.
  std::optional<BundleReader> reader =
      BundleReader::openForInspection(file->data(), file->size(), &error);
  if (!reader) {
    reportContainerError(err, bundlePath, error);
    return 1;
  }

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
    kindWidth = std::max(kindWidth, kindName(reader->kind(i)).size());
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
        << std::setw(kindWidth) << kindName(reader->kind(i)) << "  "
        << std::right << std::setw(bytesWidth) << reader->payload(i).size();
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

  const uint32_t strings = reader->stringsSize();
  const uint32_t modules = reader->moduleTableSize();
  const uint32_t edges = reader->edgeTableSize();
  const uint32_t payload = reader->payloadSize();
  size_t sectionWidth = std::max(
      std::max(widthOf(strings), widthOf(modules)),
      std::max(widthOf(edges), widthOf(payload)));

  out << "\nSECTIONS\n";
  out << "  strings  " << std::right << std::setw(sectionWidth) << strings
      << " B    modules  " << std::setw(sectionWidth) << modules << " B\n";
  out << "  edges    " << std::setw(sectionWidth) << edges << " B    payload  "
      << std::setw(sectionWidth) << payload << " B\n";
  // The size of the file, which is larger than the four sections add up to:
  // the header, and the padding that puts each payload on its alignment
  // boundary, belong to neither.
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

  std::string error;
  std::optional<MappedFile> file = MappedFile::open(bundlePath, &error);
  if (!file) {
    err << "error: " << error << "\n";
    return 1;
  }

  // Inspection mode, exactly like dumpBundle: getting bytecode out of a
  // container the current binary refuses to run is a reason to have this
  // feature, not a reason to withhold it. Deliberately not release()'d:
  // unlike the run path, nothing here outlives this call, so the mapping
  // is fine to unmap on the way out.
  std::optional<BundleReader> reader =
      BundleReader::openForInspection(file->data(), file->size(), &error);
  if (!reader) {
    reportContainerError(err, bundlePath, error);
    return 1;
  }

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

  // Verbatim: no header, no transformation. That is what makes a
  // JavaScript extraction directly loadable and a JSON extraction
  // byte-identical to its source.
  std::string_view payload = reader->payload(*found);
  if (!writeFileAtomically(outPath, payload.data(), payload.size(), err))
    return 1;

  return 0;
}

} // namespace node_compat
} // namespace hermes
