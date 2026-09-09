/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <hermes/node-compat/compile-cache/compile_cache.h>

#include <zlib.h>

#include <picohash_wrapper.h>

#include <dirent.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <string_view>
#include <vector>

namespace hermes {
namespace node_compat {

uint32_t compileCacheCrc32(const void *data, size_t size) {
  uLong crc = crc32(0L, Z_NULL, 0);
  if (size == 0)
    return static_cast<uint32_t>(crc);
  return static_cast<uint32_t>(
      crc32(crc, static_cast<const Bytef *>(data), static_cast<uInt>(size)));
}

uint32_t compileCacheKey(std::string_view filename, CompileCacheKind kind) {
  auto kindByte = static_cast<uint8_t>(kind);
  uLong crc = crc32(0L, Z_NULL, 0);
  crc = crc32(crc, reinterpret_cast<const Bytef *>(&kindByte), 1);
  // zlib's crc32 returns 0 outright for a NULL buffer rather than leaving
  // the accumulated value alone, so an empty view (data() == nullptr) would
  // otherwise discard the kind byte above and collapse every kind onto the
  // same key.
  if (!filename.empty()) {
    crc = crc32(
        crc,
        reinterpret_cast<const Bytef *>(filename.data()),
        static_cast<uInt>(filename.size()));
  }
  return static_cast<uint32_t>(crc);
}

std::string compileCacheGenerationName(
    std::string_view version,
    std::string_view arch,
    uint32_t bytecodeVersion,
    uint32_t configCrc) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "-bc%u-%08x", bytecodeVersion, configCrc);

  std::string result;
  result.reserve(version.size() + arch.size() + sizeof(buf));
  result.append(version);
  result += '-';
  result.append(arch);
  result += buf;
  return result;
}

std::string compileCacheWasmDigest(
    const uint8_t *codegenConfig,
    size_t codegenConfigSize,
    const uint8_t *wasm,
    size_t size) {
  // The config's length is hashed before the config itself, because the
  // config is variable-length and simply concatenating the two is ambiguous:
  // config "ab" with module "c" would hash identically to config "a" with
  // module "bc", and those are different compiles. A fixed-width config did
  // not have this problem; a byte string does.
  uint8_t lenBytes[8];
  for (size_t i = 0; i < sizeof(lenBytes); ++i)
    lenBytes[i] = static_cast<uint8_t>(
        (static_cast<uint64_t>(codegenConfigSize) >> (8 * i)) & 0xff);

  picohash_ctx_t ctx;
  ph_init_sha256(&ctx);
  ph_update(&ctx, lenBytes, sizeof(lenBytes));
  if (codegenConfigSize != 0)
    ph_update(&ctx, codegenConfig, codegenConfigSize);
  if (size != 0)
    ph_update(&ctx, wasm, size);
  uint8_t digest[PICOHASH_SHA256_DIGEST_LENGTH];
  ph_final(&ctx, digest);

  static const char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(sizeof(digest) * 2);
  for (uint8_t b : digest) {
    out.push_back(kHex[b >> 4]);
    out.push_back(kHex[b & 0xf]);
  }
  return out;
}

namespace {

/// Entry header, 24 bytes. Field order matches the design document. The
/// magic and header version live in the public header (compile_cache.h),
/// next to the header size, because --dump-bytecode recognizes an entry by
/// them too.
struct EntryHeader {
  uint32_t magic;
  uint32_t headerVersion;
  uint32_t sourceCrc;
  uint32_t sourceSize;
  uint32_t bytecodeSize;
  uint32_t reserved;
};

static_assert(
    sizeof(EntryHeader) == kCompileCacheHeaderSize,
    "EntryHeader must match kCompileCacheHeaderSize");

/// Write exactly \p size bytes, retrying short writes. Returns false on error.
bool writeAll(int fd, const void *data, size_t size) {
  const auto *p = static_cast<const uint8_t *>(data);
  while (size > 0) {
    ssize_t n = ::write(fd, p, size);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      return false;
    }
    p += n;
    size -= static_cast<size_t>(n);
  }
  return true;
}

/// Read exactly \p size bytes, retrying short reads. Returns false on error
/// or premature end of file.
bool readAll(int fd, void *data, size_t size) {
  auto *p = static_cast<uint8_t *>(data);
  while (size > 0) {
    ssize_t n = ::read(fd, p, size);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      return false;
    }
    if (n == 0)
      return false;
    p += n;
    size -= static_cast<size_t>(n);
  }
  return true;
}

/// Distinguishes concurrent writers of the same entry. getpid() alone is not
/// enough: runHermesNode is documented thread-safe, so two runtimes in one
/// process can write the same entry concurrently and would otherwise share a
/// temp path.
std::atomic<uint64_t> g_tempCounter{0};

} // namespace

void CacheMapping::destroy() {
  if (base != nullptr)
    ::munmap(base, length);
  delete this;
}

void CacheMapping::finalizer(const uint8_t *, size_t, void *hint) {
  static_cast<CacheMapping *>(hint)->destroy();
}

bool compileCacheWriteEntry(
    const std::string &path,
    const CompileCacheEntry &entry,
    const uint8_t *bytecode,
    size_t bytecodeSize) {
  if (bytecode == nullptr || bytecodeSize == 0)
    return false;
  if (bytecodeSize > UINT32_MAX)
    return false;

  // Unique temp name so concurrent writers never collide. pid alone is not
  // enough -- two runtimes on two threads of one process share a pid -- so a
  // monotonic counter makes it thread-unique too.
  std::string tmp = path + "." + std::to_string(::getpid()) + "." +
      std::to_string(g_tempCounter.fetch_add(1, std::memory_order_relaxed)) +
      ".tmp";

  int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0)
    return false;

  EntryHeader header{};
  header.magic = kCompileCacheMagic;
  header.headerVersion = kCompileCacheHeaderVersion;
  header.sourceCrc = entry.sourceCrc;
  header.sourceSize = entry.sourceSize;
  header.bytecodeSize = static_cast<uint32_t>(bytecodeSize);
  header.reserved = 0;

  bool ok = writeAll(fd, &header, sizeof(header)) &&
      writeAll(fd, bytecode, bytecodeSize);
  ::close(fd);

  if (!ok || ::rename(tmp.c_str(), path.c_str()) != 0) {
    ::unlink(tmp.c_str());
    return false;
  }
  return true;
}

bool compileCacheReadEntry(CompileCacheEntry &entry) {
  int fd = ::open(entry.cacheFilePath.c_str(), O_RDONLY);
  if (fd < 0)
    return false;

  EntryHeader header{};
  if (!readAll(fd, &header, sizeof(header))) {
    ::close(fd);
    return false;
  }

  // Cheapest checks first: a changed file usually changes length, so the
  // size check rejects most edits before the CRC matters.
  if (header.magic != kCompileCacheMagic ||
      header.headerVersion != kCompileCacheHeaderVersion ||
      header.sourceSize != entry.sourceSize ||
      header.sourceCrc != entry.sourceCrc || header.bytecodeSize == 0) {
    ::close(fd);
    return false;
  }

  size_t mapSize = sizeof(EntryHeader) + header.bytecodeSize;

  // Refuse a file shorter than its header claims. Atomic writes make this
  // unreachable for entries we wrote, but the directory is user-writable.
  struct stat st {};
  if (::fstat(fd, &st) != 0 || static_cast<size_t>(st.st_size) < mapSize) {
    ::close(fd);
    return false;
  }

  void *base = ::mmap(nullptr, mapSize, PROT_READ, MAP_PRIVATE, fd, 0);
  ::close(fd); // The mapping keeps the inode alive on its own.
  if (base == MAP_FAILED)
    return false;

  auto *mapping = new CacheMapping();
  mapping->base = base;
  mapping->length = mapSize;

  entry.mapping = mapping;
  entry.bytecode = static_cast<const uint8_t *>(base) + sizeof(EntryHeader);
  entry.bytecodeSize = header.bytecodeSize;
  return true;
}

namespace {

/// Recursively delete \p path. Best effort.
void removeTree(const std::string &path) {
  DIR *d = ::opendir(path.c_str());
  if (d != nullptr) {
    while (struct dirent *e = ::readdir(d)) {
      if (::strcmp(e->d_name, ".") == 0 || ::strcmp(e->d_name, "..") == 0)
        continue;
      std::string child = path + "/" + e->d_name;
      struct stat st {};
      if (::lstat(child.c_str(), &st) == 0 && S_ISDIR(st.st_mode))
        removeTree(child);
      else
        ::unlink(child.c_str());
    }
    ::closedir(d);
  }
  ::rmdir(path.c_str());
}

} // namespace

std::string compileCacheDefaultRoot() {
  if (const char *xdg = ::getenv("XDG_CACHE_HOME")) {
    if (xdg[0] != '\0')
      return std::string(xdg) + "/hermes-node/compile-cache";
  }
  if (const char *home = ::getenv("HOME")) {
    if (home[0] != '\0')
      return std::string(home) + "/.cache/hermes-node/compile-cache";
  }
  return std::string();
}

bool compileCacheMakeDirs(const std::string &path) {
  if (path.empty())
    return false;
  // Create each component in turn, tolerating EEXIST so concurrent
  // processes racing to create the same directory both succeed.
  for (size_t i = 1; i <= path.size(); ++i) {
    if (i != path.size() && path[i] != '/')
      continue;
    std::string component = path.substr(0, i);
    if (::mkdir(component.c_str(), 0755) != 0 && errno != EEXIST)
      return false;
  }
  struct stat st {};
  return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

void compileCachePruneGenerations(
    const std::string &versionedRoot,
    const std::string &keepName,
    size_t keepCount) {
  DIR *d = ::opendir(versionedRoot.c_str());
  if (d == nullptr)
    return;

  std::vector<std::pair<time_t, std::string>> others;
  while (struct dirent *e = ::readdir(d)) {
    if (::strcmp(e->d_name, ".") == 0 || ::strcmp(e->d_name, "..") == 0)
      continue;
    if (keepName == e->d_name)
      continue;
    std::string child = versionedRoot + "/" + e->d_name;
    struct stat st {};
    if (::lstat(child.c_str(), &st) != 0 || !S_ISDIR(st.st_mode))
      continue;
    others.emplace_back(st.st_mtime, child);
  }
  ::closedir(d);

  if (others.size() <= keepCount)
    return;

  // Most recently modified first; everything past keepCount goes.
  std::sort(others.begin(), others.end(), [](const auto &a, const auto &b) {
    return a.first > b.first;
  });
  for (size_t i = keepCount; i < others.size(); ++i)
    removeTree(others[i].second);
}

bool CompileCache::enable(
    const std::string &root,
    const std::string &generationName) {
  if (root.empty() || generationName.empty())
    return false;

  // v1/ is a structure escape hatch: a fundamentally different layout later
  // becomes v2/ and can coexist with this one.
  std::string versionedRoot = root + "/v1";
  std::string generationDir = versionedRoot + "/" + generationName;

  bool existed = false;
  {
    struct stat st {};
    existed = ::stat(generationDir.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
  }

  if (!compileCacheMakeDirs(generationDir))
    return false;

  // Prune only when a generation was actually created, so a normal startup
  // never scans the cache root.
  if (!existed) {
    compileCachePruneGenerations(
        versionedRoot, generationName, kCompileCacheGenerationsKept);
  }

  generationDir_ = generationDir;
  versionedRoot_ = versionedRoot;
  enabled_ = true;
  return true;
}

void CompileCache::trace(const char *what, std::string_view filename) const {
  if (tracing_)
    // filename.data() may be null for a default-constructed view; passing
    // that through %.*s is undefined behavior even with zero precision, so
    // substitute a valid empty string in that case.
    std::fprintf(
        stderr,
        "[compile cache] %s %.*s\n",
        what,
        static_cast<int>(filename.size()),
        filename.data() != nullptr ? filename.data() : "");
}

bool CompileCache::lookup(
    CompileCacheEntry &entry,
    std::string_view source,
    std::string_view filename,
    CompileCacheKind kind) {
  if (!enabled_)
    return false;

  entry.key = compileCacheKey(filename, kind);
  entry.sourceCrc = compileCacheCrc32(source.data(), source.size());
  entry.sourceSize = static_cast<uint32_t>(source.size());

  char rel[16];
  std::snprintf(
      rel, sizeof(rel), "/%02x/%08x", (entry.key >> 24) & 0xff, entry.key);
  entry.cacheFilePath = generationDir_ + rel;

  if (compileCacheReadEntry(entry)) {
    trace("hit ", filename);
    return true;
  }
  trace("miss", filename);
  return false;
}

void CompileCache::save(
    const CompileCacheEntry &entry,
    const uint8_t *bytecode,
    size_t bytecodeSize) {
  if (!enabled_ || entry.cacheFilePath.empty())
    return;

  // Create the fanout directory. Cheap enough to attempt every time; mkdir
  // on an existing directory is a single failed syscall.
  size_t slash = entry.cacheFilePath.rfind('/');
  if (slash != std::string::npos)
    compileCacheMakeDirs(entry.cacheFilePath.substr(0, slash));

  if (!compileCacheWriteEntry(
          entry.cacheFilePath, entry, bytecode, bytecodeSize))
    trace("save-failed", entry.cacheFilePath);
}

void CompileCache::invalidate(const CompileCacheEntry &entry) {
  if (!enabled_ || entry.cacheFilePath.empty())
    return;
  ::unlink(entry.cacheFilePath.c_str());
  trace("invalidated", entry.cacheFilePath);
}

bool CompileCache::lookupWasm(
    CompileCacheEntry &entry,
    const uint8_t *wasm,
    size_t size,
    const uint8_t *codegenConfig,
    size_t codegenConfigSize) {
  if (!enabled_)
    return false;

  std::string digest =
      compileCacheWasmDigest(codegenConfig, codegenConfigSize, wasm, size);
  // The digest is the file name; the CRC and size stay as the cheap
  // truncation guard the header already carries.
  entry.key = compileCacheCrc32(digest.data(), digest.size());
  entry.sourceCrc = compileCacheCrc32(wasm, size);
  entry.sourceSize = static_cast<uint32_t>(size);
  entry.cacheFilePath =
      generationDir_ + "/" + digest.substr(0, 2) + "/w" + digest;

  bool hit = compileCacheReadEntry(entry);
  trace(hit ? "wasm hit" : "wasm miss", digest);
  return hit;
}

void CompileCache::saveWasm(
    const CompileCacheEntry &entry,
    const uint8_t *hbc,
    size_t hbcSize) {
  // save() already creates the fanout directory from entry.cacheFilePath and
  // writes through the temp-and-rename path, so there is nothing Wasm-
  // specific about persisting the bytes.
  save(entry, hbc, hbcSize);

  if (!sweptWasm_) {
    sweptWasm_ = true;
    compileCacheEvictWasm(versionedRoot_, config_);
  }
}

namespace {

/// True if \p name is exactly "w" followed by 64 lowercase hex digits --
/// the shape a Wasm entry's file name always has. A leftover temp file from
/// an interrupted saveWasm() ("w<digest>.<pid>.<n>.tmp") starts with 'w' too
/// but is longer, so this rejects it.
bool isWasmEntryName(const char *name) {
  size_t i = 0;
  if (name[i++] != 'w')
    return false;
  for (; i < 65; ++i) {
    char c = name[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
      return false;
  }
  return name[65] == '\0';
}

} // namespace

/// True if \p name looks like a temp file an interrupted entry write left
/// behind: "w<64 hex>.<pid>.<n>.tmp". Deliberately narrow -- it is a licence
/// to unlink, so it matches the shape compileCacheWriteEntry actually
/// produces and nothing else.
static bool isWasmEntryTempName(const char *name) {
  size_t i = 0;
  if (name[i++] != 'w')
    return false;
  for (; i < 65; ++i) {
    char c = name[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
      return false;
  }
  if (name[i] != '.')
    return false;
  size_t len = std::strlen(name);
  const char kSuffix[] = ".tmp";
  const size_t suffixLen = sizeof(kSuffix) - 1;
  return len > i + suffixLen &&
      std::strcmp(name + len - suffixLen, kSuffix) == 0;
}

void compileCacheEvictWasm(
    const std::string &versionedRoot,
    const CacheConfig &config) {
  if (versionedRoot.empty())
    return;

  struct Victim {
    std::string path;
    uint64_t size;
    int64_t stamp;
  };
  std::vector<Victim> entries;
  uint64_t total = 0;

  // Temp files older than this are assumed abandoned. A real write takes
  // milliseconds; an hour is enormous by comparison, and erring long only
  // costs disk, where erring short risks unlinking a live writer's file.
  const time_t kTempReapAgeSeconds = 3600;
  const time_t now = ::time(nullptr);

  // EVERY generation, not just the current one. The budget names the cache,
  // and kCompileCacheGenerationsKept older generations are retained beside
  // the current one, each holding whatever it accumulated while it was
  // current -- so a sweep confined to one directory leaves real disk use at
  // a multiple of the configured number.
  DIR *versioned = ::opendir(versionedRoot.c_str());
  if (!versioned)
    return;
  std::vector<std::string> generationDirs;
  while (struct dirent *gen = ::readdir(versioned)) {
    if (gen->d_name[0] == '.')
      continue;
    std::string genDir = versionedRoot + "/" + gen->d_name;
    struct stat st {};
    if (::lstat(genDir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode))
      continue;
    generationDirs.push_back(std::move(genDir));
  }
  ::closedir(versioned);

  for (const std::string &generationDir : generationDirs) {
    DIR *root = ::opendir(generationDir.c_str());
    if (!root)
      continue;
    while (struct dirent *fan = ::readdir(root)) {
      if (fan->d_name[0] == '.')
        continue;
      std::string fanDir = generationDir + "/" + fan->d_name;
      DIR *sub = ::opendir(fanDir.c_str());
      if (!sub)
        continue;
      while (struct dirent *ent = ::readdir(sub)) {
        // An abandoned temp file is reaped here rather than counted. Counting
        // it would let a transient file evict real entries to make room for
        // something about to be renamed away; reaping is what bounds the
        // leak, and the age threshold is what keeps a live writer safe.
        if (isWasmEntryTempName(ent->d_name)) {
          std::string tmpPath = fanDir + "/" + ent->d_name;
          struct stat tst {};
          if (::lstat(tmpPath.c_str(), &tst) == 0 && S_ISREG(tst.st_mode) &&
              now - tst.st_mtime > kTempReapAgeSeconds)
            ::unlink(tmpPath.c_str());
          continue;
        }
        // Only Wasm entries. JavaScript entries are keyed by path and
        // rewritten in place, so they never accumulate the way these do.
        if (!isWasmEntryName(ent->d_name))
          continue;
        std::string path = fanDir + "/" + ent->d_name;
        struct stat st {};
        if (::lstat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode))
          continue;
        int64_t stamp = config.recency == CacheRecency::kAtime
            ? static_cast<int64_t>(st.st_atime)
            : static_cast<int64_t>(st.st_mtime);
        entries.push_back({path, static_cast<uint64_t>(st.st_size), stamp});
        total += static_cast<uint64_t>(st.st_size);
      }
      ::closedir(sub);
    }
    ::closedir(root);
  }

  if (total <= config.maxWasmBytes)
    return;

  // Oldest first.
  std::sort(
      entries.begin(), entries.end(), [](const Victim &a, const Victim &b) {
        return a.stamp < b.stamp;
      });

  for (const Victim &v : entries) {
    if (total <= config.maxWasmBytes)
      break;
    // Best effort: a file another process is still mapping unlinks fine on
    // POSIX -- the inode survives until the last mapping is dropped, which
    // is the same property compileCachePruneGenerations relies on.
    if (::unlink(v.path.c_str()) == 0)
      total -= v.size;
  }
}

} // namespace node_compat
} // namespace hermes
