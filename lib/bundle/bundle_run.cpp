/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <hermes/node-compat/bundle/bundle_run.h>

#include <hermes/node-compat/bundle/bundle_generation.h>
#include <hermes/node-compat/bundle/bundle_reader.h>
#include <hermes/node-compat/bundle/mapped_file.h>

#include <napi/hermes_napi.h>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace hermes {
namespace node_compat {

namespace {

namespace fs = std::filesystem;

/// The open bundle. Process-global rather than per-env because the mapping
/// it owns backs bytecode that Hermes executes in place: the bytes have to
/// stay mapped for as long as any runtime can reach a function compiled out
/// of them, which in practice is the life of the process. --bundle is
/// refused together with --inspect (the only way a second runtime is
/// started), so exactly one runtime ever consults this.
struct OpenBundle {
  /// Views into the mapping, which is deliberately never unmapped, so they
  /// stay valid for the life of the process. The reader holds the only
  /// pointer to it.
  std::optional<BundleReader> reader;
  /// identity -> module index. The edge table is index-keyed, but the JS
  /// side speaks identities, so this is the one translation both natives
  /// need. Keys are string_views into the mapped string table: no copies,
  /// and nothing to keep in sync.
  std::unordered_map<std::string_view, uint32_t> byIdentity;
  /// Directory holding the bundle file, with symlinks resolved. Module
  /// identities are relative to it, so it is what __filename is built from.
  std::string root;
};

OpenBundle &openBundleState() {
  static OpenBundle state;
  return state;
}

/// Reads \p value as a UTF-8 string into \p out. Returns false if it is not
/// a string -- including undefined, which is how JS spells "no bundled
/// importer".
bool stringArg(napi_env env, napi_value value, std::string *out) {
  napi_valuetype type;
  if (napi_typeof(env, value, &type) != napi_ok || type != napi_string)
    return false;
  size_t len = 0;
  if (napi_get_value_string_utf8(env, value, nullptr, 0, &len) != napi_ok)
    return false;
  out->assign(len, '\0');
  return napi_get_value_string_utf8(env, value, &(*out)[0], len + 1, &len) ==
      napi_ok;
}

napi_value undefinedValue(napi_env env) {
  napi_value undef;
  napi_get_undefined(env, &undef);
  return undef;
}

/// Returns the module index for \p identity, or nullopt if the bundle has
/// no such module.
std::optional<uint32_t> indexOf(const std::string &identity) {
  const OpenBundle &state = openBundleState();
  auto it = state.byIdentity.find(std::string_view(identity));
  if (it == state.byIdentity.end())
    return std::nullopt;
  return it->second;
}

/// __bundleLookup(importerIdentity, specifier) -> identity | undefined.
///
/// undefined means "not in the bundle" -- the caller falls back to disk. An
/// importerIdentity that is undefined (no bundled importer) or unknown is a
/// miss for the same reason: there is no row in the edge table to consult.
napi_value bundleLookupCallback(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok ||
      argc < 2)
    return undefinedValue(env);

  std::string importer;
  std::string specifier;
  if (!stringArg(env, argv[0], &importer) ||
      !stringArg(env, argv[1], &specifier))
    return undefinedValue(env);

  std::optional<uint32_t> importerIndex = indexOf(importer);
  if (!importerIndex)
    return undefinedValue(env);

  const OpenBundle &state = openBundleState();
  std::optional<uint32_t> target =
      state.reader->lookup(*importerIndex, specifier);
  if (!target)
    return undefinedValue(env);

  std::string_view identity = state.reader->identity(*target);
  napi_value result;
  if (napi_create_string_utf8(env, identity.data(), identity.size(), &result) !=
      napi_ok)
    return nullptr;
  return result;
}

/// __bundleLoad(identity) -> function | string.
///
/// A JavaScript module's payload is bytecode for the CommonJS wrapper
/// function the producer compiled (bundle_build.cpp's kWrapperPrefix), so
/// running it yields that function, ready to be called with (exports,
/// require, module, __filename, __dirname). A JSON module's payload is its
/// raw text; JS parses it, which keeps JSON.parse's error messages and
/// their positions exactly as they would be from disk.
napi_value bundleLoadCallback(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok ||
      argc < 1) {
    napi_throw_error(env, nullptr, "__bundleLoad requires an identity");
    return nullptr;
  }

  std::string identity;
  if (!stringArg(env, argv[0], &identity)) {
    napi_throw_error(env, nullptr, "__bundleLoad requires a string identity");
    return nullptr;
  }

  std::optional<uint32_t> index = indexOf(identity);
  if (!index) {
    // Only reachable if JS asked for an identity the bundle never handed
    // it, which would be a bug in bundle-loader.js rather than bad input.
    std::string message = "no such module in the bundle: " + identity;
    napi_throw_error(env, nullptr, message.c_str());
    return nullptr;
  }

  const OpenBundle &state = openBundleState();
  std::string_view payload = state.reader->payload(*index);

  if (state.reader->kind(*index) == ModuleKind::kJSON) {
    napi_value result;
    if (napi_create_string_utf8(env, payload.data(), payload.size(), &result) !=
        napi_ok)
      return nullptr;
    return result;
  }

  // The mapping outlives the runtime, so there is nothing to finalize and
  // no copy to make: pass a null finalizer (the buffer is externally
  // managed) and mark it persistent, exactly as the embedded-module loader
  // does for bytecode linked into the binary. Both are the same situation --
  // static bytes, alive for the whole process.
  hermes_bytecode_flags flags{};
  flags.struct_size = sizeof(flags);
  flags.persistent = true;

  // The same path the loader will pass as __filename (root joined with the
  // identity), so a stack trace names the module the way the module itself
  // does. The path the producer recorded at compile time is a build-machine
  // path that means nothing here.
  std::string sourceUrl = state.root + "/" + identity;

  napi_value result;
  napi_status status = hermes_run_bytecode(
      env,
      reinterpret_cast<const uint8_t *>(payload.data()),
      payload.size(),
      nullptr,
      nullptr,
      sourceUrl.c_str(),
      &flags,
      &result);
  if (status != napi_ok) {
    // Bytecode that passed BundleReader::open's structural checks but that
    // Hermes will not load. There is no source to recompile from, so this
    // is terminal for this module: leave the pending exception (or raise
    // one if the failure was a validation error rather than a JS throw) and
    // let it propagate through require().
    bool pending = false;
    napi_is_exception_pending(env, &pending);
    if (!pending) {
      std::string message =
          "failed to load bundled bytecode for module: " + identity;
      napi_throw_error(env, nullptr, message.c_str());
    }
    return nullptr;
  }
  return result;
}

/// __bundleEntry() -> identity of the module the bundle was built from.
napi_value bundleEntryCallback(napi_env env, napi_callback_info /*info*/) {
  const OpenBundle &state = openBundleState();
  std::string_view identity = state.reader->identity(state.reader->entry());
  napi_value result;
  if (napi_create_string_utf8(env, identity.data(), identity.size(), &result) !=
      napi_ok)
    return nullptr;
  return result;
}

/// __bundleRoot() -> the directory identities are relative to.
napi_value bundleRootCallback(napi_env env, napi_callback_info /*info*/) {
  const OpenBundle &state = openBundleState();
  napi_value result;
  if (napi_create_string_utf8(
          env, state.root.c_str(), NAPI_AUTO_LENGTH, &result) != napi_ok)
    return nullptr;
  return result;
}

/// Creates \p name as a function and sets it both on \p global (as
/// `__bundle<Name>`) and on \p bundleObject (as \p shortName).
napi_status defineNative(
    napi_env env,
    napi_value global,
    napi_value bundleObject,
    const char *globalName,
    const char *shortName,
    napi_callback callback) {
  napi_value fn;
  napi_status status = napi_create_function(
      env, globalName, NAPI_AUTO_LENGTH, callback, nullptr, &fn);
  if (status != napi_ok)
    return status;
  status = napi_set_named_property(env, global, globalName, fn);
  if (status != napi_ok)
    return status;
  return napi_set_named_property(env, bundleObject, shortName, fn);
}

} // namespace

bool openBundle(const std::string &path, std::string *error) {
  OpenBundle &state = openBundleState();
  if (state.reader) {
    *error = "a bundle is already open";
    return false;
  }

  std::optional<MappedFile> file = MappedFile::open(path, error);
  if (!file)
    return false;

  std::optional<BundleReader> reader = BundleReader::open(
      file->data(), file->size(), bundleGenerationTag(), error);
  if (!reader) {
    // Leave nothing behind on a failure: this is written as a recoverable
    // bool API, so a caller that reports the error and carries on must not
    // be charged an address-space leak for a container it never got. Not
    // releasing the mapping is what unmaps it, when `file` goes out of
    // scope on the way out of this branch.
    return false;
  }

  // Past the point of no return, so the mapping becomes permanent: bundled
  // bytecode is executed in place out of it and stays reachable from the
  // runtime for as long as the process lives (see the header). This is the
  // one caller that releases; a tool's mapping dies with the call.
  file->release();

  // realpath, so that a bundle reached through a symlinked directory
  // resolves identities against the directory the file really lives in.
  std::error_code ec;
  fs::path canonical = fs::canonical(fs::path(path), ec);
  fs::path rootPath =
      ec ? fs::absolute(fs::path(path)).parent_path() : canonical.parent_path();

  state.reader = std::move(reader);
  state.root = rootPath.string();
  state.byIdentity.reserve(state.reader->moduleCount());
  for (uint32_t i = 0; i < state.reader->moduleCount(); ++i)
    state.byIdentity.emplace(state.reader->identity(i), i);

  return true;
}

napi_status installBundleGlobals(napi_env env, napi_value *bundleObject) {
  napi_value global;
  napi_status status = napi_get_global(env, &global);
  if (status != napi_ok)
    return status;

  napi_value bundle;
  status = napi_create_object(env, &bundle);
  if (status != napi_ok)
    return status;

  status = defineNative(
      env, global, bundle, "__bundleLookup", "lookup", bundleLookupCallback);
  if (status != napi_ok)
    return status;
  status = defineNative(
      env, global, bundle, "__bundleLoad", "load", bundleLoadCallback);
  if (status != napi_ok)
    return status;
  status = defineNative(
      env, global, bundle, "__bundleEntry", "entry", bundleEntryCallback);
  if (status != napi_ok)
    return status;
  status = defineNative(
      env, global, bundle, "__bundleRoot", "root", bundleRootCallback);
  if (status != napi_ok)
    return status;

  *bundleObject = bundle;
  return napi_ok;
}

} // namespace node_compat
} // namespace hermes
