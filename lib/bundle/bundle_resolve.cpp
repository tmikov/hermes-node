/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <hermes/node-compat/bundle/bundle_resolve.h>

#include <cctype>
#include <filesystem>
#include <initializer_list>
#include <unordered_set>

namespace hermes {
namespace node_compat {

namespace {

namespace fs = std::filesystem;

/// Joins \p base and \p rel the way a require() path join would, and
/// collapses "." / ".." segments purely lexically (no filesystem access, no
/// symlink resolution) so the result stays byte-for-byte predictable even
/// when the containing directory is itself reached through a symlink (e.g.
/// macOS's /tmp -> /private/tmp).
std::string joinNormalized(const fs::path &base, std::string_view rel) {
  return (base / fs::path(std::string(rel))).lexically_normal().string();
}

/// Reads a JSON string literal from `content[i]` (which must be the opening
/// `"`), advancing \p i past the closing `"`. Not a full JSON escape
/// decoder (e.g. `\uXXXX` is passed through undecoded) -- package.json
/// string values relevant to this reader are plain keys and relative
/// paths, and a malformed/exotic value falls through to the index.* probe
/// like any other unresolved main. Returns nullopt, with \p i left
/// unspecified, if the string is unterminated.
std::optional<std::string> readJsonString(
    const std::string &content,
    size_t &i) {
  size_t n = content.size();
  ++i; // skip the opening quote.
  std::string value;
  while (i < n) {
    char c = content[i];
    if (c == '"') {
      ++i;
      return value;
    }
    if (c == '\\' && i + 1 < n) {
      ++i;
      value.push_back(content[i]);
    } else {
      value.push_back(c);
    }
    ++i;
  }
  return std::nullopt;
}

/// Skips one JSON value starting at `content[i]`, advancing \p i past it.
/// Used to skip over the *value* half of every top-level key/value pair
/// that isn't "main", without inspecting or caring what that value
/// contains -- in particular, without ever treating a string that appears
/// in value position as a possible key. A string value is skipped via
/// readJsonString(); an object or array value is skipped by counting only
/// its own bracket character (braces for `{`, brackets for `[`) while
/// still reading nested strings atomically, so a bracket character that
/// happens to appear inside a nested string never perturbs the count, and
/// a differently-typed bracket nested inside (e.g. `[` inside an object)
/// passes through uncounted -- valid JSON keeps every bracket type
/// independently balanced, so counting just one type still lands exactly
/// on this value's matching close. Anything else (number, true, false,
/// null) is skipped up to the next structural character or whitespace.
/// \return false if a string within the value is unterminated or an
///     object/array value never closes.
bool skipValue(const std::string &content, size_t &i) {
  const size_t n = content.size();
  if (i >= n)
    return false;
  char c = content[i];
  if (c == '"')
    return readJsonString(content, i).has_value();

  if (c == '{' || c == '[') {
    const char open = c;
    const char close = (c == '{') ? '}' : ']';
    int depth = 0;
    while (i < n) {
      char d = content[i];
      if (d == '"') {
        if (!readJsonString(content, i))
          return false; // unterminated string -- malformed file.
        continue;
      }
      if (d == open) {
        ++depth;
        ++i;
        continue;
      }
      if (d == close) {
        --depth;
        ++i;
        if (depth == 0)
          return true;
        continue;
      }
      ++i;
    }
    return false; // unterminated object/array.
  }

  // A bare token: number, true, false, null, or anything else malformed.
  // Leniently skip to the next delimiter rather than trying to validate it
  // -- this function only needs to get past it, not judge it.
  while (i < n && content[i] != ',' && content[i] != '}' && content[i] != ']' &&
         !std::isspace(static_cast<unsigned char>(content[i])))
    ++i;
  return true;
}

/// Reads the `"main"` field out of `<dir>/package.json` without a JSON
/// parser: a minimal top-level-only object walk. It reads one key/value
/// pair of the root object at a time -- a string strictly in key position
/// (immediately after `{` or `,`, before `:`) is the only kind of token
/// ever compared against "main"; every value, string or otherwise, is
/// skipped via skipValue() without inspection. This is what keeps a
/// nested `"browser": {"main": "wrong.js"}` from being mistaken for the
/// real top-level `"main"` (its "main" is a key one level down, never
/// reached by this loop), and equally keeps a top-level *value* that
/// happens to equal the text "main" (e.g. `"description": "main"`) from
/// being mistaken for a key -- this function only ever reads a candidate
/// key at the point in the grammar where a key must be, never by scanning
/// for the text "main" and guessing.
///
/// A candidate key that turns out not to be "main" (or is "main" but
/// malformed -- see below) never aborts the scan early *unless the
/// malformed field is itself named "main"*: this function's only job is
/// to find that one field, so once it is found -- correctly, in key
/// position -- a syntax problem in its value is conclusive, not a reason
/// to keep looking for another (a well-formed package.json has at most one
/// top-level "main").
///
/// package.json "exports" is deliberately NOT supported in v1 -- only
/// "main" is consulted. This is the most likely source of a future
/// resolution mismatch with Node's real resolver (which prefers "exports"
/// when present); a reader extending this must not assume the omission was
/// an oversight.
///
/// Anything short of a well-formed top-level `"main": "<string>"` (no
/// object, no such key, missing colon, missing/unterminated string,
/// non-string value) is treated as absent, per the caller's fallback to
/// the index.* probe.
std::optional<std::string> readPackageMain(
    FileSource &src,
    const std::string &dir) {
  std::optional<std::string> fileContent = src.readPackageJson(dir);
  if (!fileContent)
    return std::nullopt;
  const std::string &content = *fileContent;
  const size_t n = content.size();

  auto skipWs = [&](size_t &i) {
    while (i < n && std::isspace(static_cast<unsigned char>(content[i])))
      ++i;
  };

  size_t i = 0;
  skipWs(i);
  if (i >= n || content[i] != '{')
    return std::nullopt; // package.json must be a top-level object.
  ++i;

  while (true) {
    skipWs(i);
    if (i >= n)
      return std::nullopt; // unterminated object.
    if (content[i] == '}')
      return std::nullopt; // end of object: no top-level "main".
    if (content[i] != '"')
      return std::nullopt; // malformed: expected a key string here.

    std::optional<std::string> key = readJsonString(content, i);
    if (!key)
      return std::nullopt; // unterminated key string.

    skipWs(i);
    if (i >= n || content[i] != ':')
      return std::nullopt; // malformed: key with no colon.
    ++i;
    skipWs(i);

    if (*key == "main") {
      if (i >= n || content[i] != '"')
        return std::nullopt; // main present but not a string value.
      return readJsonString(content, i);
    }

    if (!skipValue(content, i))
      return std::nullopt; // malformed value.

    skipWs(i);
    if (i < n && content[i] == ',') {
      ++i;
      continue;
    }
    // Any other character here (including a well-formed '}') means this
    // was the last pair and no "main" key was found at the top level.
    return std::nullopt;
  }
}

constexpr int kMaxResolveDepth = 32;

std::optional<std::string>
resolveBase(FileSource &src, const std::string &base, int depth) {
  if (depth > kMaxResolveDepth)
    return std::nullopt;

  if (src.isRegularFile(base))
    return base;
  for (const char *ext : {".js", ".ts", ".json"}) {
    std::string candidate = base + ext;
    if (src.isRegularFile(candidate))
      return candidate;
  }

  if (src.isDirectory(base)) {
    if (std::optional<std::string> main = readPackageMain(src, base)) {
      std::string mainBase = joinNormalized(fs::path(base), *main);
      if (std::optional<std::string> resolved =
              resolveBase(src, mainBase, depth + 1))
        return resolved;
    }
    for (const char *name : {"index.js", "index.ts", "index.json"}) {
      std::string candidate = (fs::path(base) / name).string();
      if (src.isRegularFile(candidate))
        return candidate;
    }
  }

  return std::nullopt;
}

/// The embedded builtin module names, kept in sync by hand with
/// `builtinIds` in libjs/shims/internal/bootstrap/realm.js.
const std::unordered_set<std::string_view> &builtinIds() {
  static const std::unordered_set<std::string_view> ids = {
      "assert",
      "assert/strict",
      "async_hooks",
      "buffer",
      "child_process",
      "cluster",
      "console",
      "constants",
      "dgram",
      "diagnostics_channel",
      "dns",
      "dns/promises",
      "domain",
      "events",
      "fs",
      "fs/promises",
      "http",
      "https",
      "net",
      "os",
      "path",
      "path/posix",
      "path/win32",
      "process",
      "querystring",
      "readline",
      "readline/promises",
      "repl",
      "module",
      "crypto",
      "stream",
      "stream/consumers",
      "stream/promises",
      "stream/web",
      "string_decoder",
      "timers",
      "timers/promises",
      "tls",
      "tty",
      "url",
      "util",
      "util/types",
      "vm",
  };
  return ids;
}

/// Strips a leading "node:" from \p specifier, if present.
std::string_view stripNodeScheme(std::string_view specifier) {
  static constexpr std::string_view kNodeScheme = "node:";
  if (specifier.substr(0, kNodeScheme.size()) == kNodeScheme)
    return specifier.substr(kNodeScheme.size());
  return specifier;
}

/// Splits an absolute path into its non-empty "/"-separated segments, e.g.
/// "/a/b" -> {"a", "b"}, "/" -> {}.
std::vector<std::string> splitAbsPath(const std::string &path) {
  std::vector<std::string> segments;
  size_t start = 0;
  while (start < path.size()) {
    size_t slash = path.find('/', start);
    size_t end = slash == std::string::npos ? path.size() : slash;
    if (end > start)
      segments.push_back(path.substr(start, end - start));
    start = end + 1;
  }
  return segments;
}

} // namespace

std::optional<std::string> resolveSpecifier(
    FileSource &src,
    std::string_view fromFile,
    std::string_view specifier) {
  fs::path fromDir = fs::path(std::string(fromFile)).parent_path();

  // Node's exact predicate (Module._resolveLookupPaths in
  // libjs-node/internal/modules/cjs/loader.js): a request is relative iff it
  // starts with '.' and is either exactly ".", or its second character is
  // '.' or '/'. Testing for the "./" and "../" prefixes instead would be
  // subtly narrower: the bare specifiers "." and ".." are relative to Node
  // -- `require('..')` is how a package's `bin/` script reaches its own
  // root, and it appears in real trees -- but would be classified as bare
  // here and sent into the node_modules walk below, where "<dir>/
  // node_modules/.." normalizes straight back to "<dir>" and silently
  // resolves to the WRONG module (the requiring file's own directory
  // instead of its parent).
  bool isRelative = !specifier.empty() && specifier[0] == '.' &&
      (specifier.size() == 1 || specifier[1] == '.' || specifier[1] == '/');
  if (isRelative) {
    std::string base = joinNormalized(fromDir, specifier);
    return resolveBase(src, base, 0);
  }

  // Bare specifier: walk node_modules from fromDir up to the filesystem
  // root, exactly as Node's NODE_MODULES_PATHS does -- including its skip of
  // any directory that is itself named "node_modules", which would otherwise
  // probe a "node_modules/node_modules/<specifier>" that Node never looks
  // at.
  fs::path dir = fromDir;
  while (true) {
    if (dir.filename() != "node_modules") {
      std::string candidate = joinNormalized(dir / "node_modules", specifier);
      if (std::optional<std::string> resolved = resolveBase(src, candidate, 0))
        return resolved;
    }

    fs::path parent = dir.parent_path();
    if (parent == dir)
      break; // reached the filesystem root
    dir = parent;
  }
  return std::nullopt;
}

std::optional<std::string> resolveSpecifier(
    std::string_view fromFile,
    std::string_view specifier) {
  DiskFileSource disk;
  return resolveSpecifier(disk, fromFile, specifier);
}

bool isBuiltinSpecifier(std::string_view specifier) {
  return builtinIds().count(stripNodeScheme(specifier)) != 0;
}

bool isVendoredSpecifier(std::string_view specifier) {
  // Kept in sync by hand with `vendoredIds` in
  // libjs/shims/internal/bootstrap/realm.js. Exact names only, no subpaths:
  // that file's allRequirableSet holds exactly these strings, so
  // require('ws/lib/websocket') is not requirable from the embedded copy
  // either.
  static const std::unordered_set<std::string_view> ids = {"ws"};
  return ids.count(stripNodeScheme(specifier)) != 0;
}

std::string commonAncestor(const std::vector<std::string> &absPaths) {
  if (absPaths.empty())
    return "/";

  std::optional<std::vector<std::string>> common;
  for (const std::string &p : absPaths) {
    std::string dir = fs::path(p).parent_path().string();
    std::vector<std::string> segments = splitAbsPath(dir);
    if (!common) {
      common = std::move(segments);
      continue;
    }
    size_t n = std::min(common->size(), segments.size());
    size_t matched = 0;
    while (matched < n && (*common)[matched] == segments[matched])
      ++matched;
    common->resize(matched);
  }

  if (common->empty())
    return "/";
  std::string result;
  for (const std::string &seg : *common) {
    result += '/';
    result += seg;
  }
  return result;
}

} // namespace node_compat
} // namespace hermes
