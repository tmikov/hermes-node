/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "named_addons.h"

#include <hermes/node-compat/process/node_process.h>
#include <hermes/node-compat/runtime/embedder_module_prefix.h>

#include <node_api.h>
#include <uv.h>

#include <cerrno>
#include <climits>
#include <cstdlib>
#include <string>

#include <sys/stat.h>

namespace {

napi_value initImgui(napi_env env, napi_value exports);

/// A name a program can require and the file in the executable's directory
/// that provides it. `init` is the embedder init for this row: an
/// EmbedderModule's init takes no user data, so each row needs a function
/// of its own, which forwards to loadNamedAddon() with its row.
struct NamedAddon {
  const char *name;
  const char *fileName;
  napi_addon_register_func init;
};

/// The whole table. A product addon adds a row; anything else is loaded by
/// path, through process.dlopen or require(), as before.
const NamedAddon kNamedAddons[] = {
    {"imgui", "hermes-node-imgui.node", initImgui},
};

/// The running executable's path, realpath'd -- the rule resolveKitDir()
/// in tools/hermes-node/hermes-node.cpp uses, and for the same reason:
/// argv[0] is whatever the caller chose to exec with. Empty when the
/// executable's path cannot be determined at all.
std::string executablePath() {
  char execBuf[PATH_MAX];
  size_t execSize = sizeof(execBuf);
  if (uv_exepath(execBuf, &execSize) != 0)
    return std::string();
  std::string exePath(execBuf, execSize);

  char realBuf[PATH_MAX];
  if (const char *resolved = realpath(exePath.c_str(), realBuf))
    exePath = resolved;
  return exePath;
}

napi_value
loadNamedAddon(napi_env env, const NamedAddon &addon, napi_value exports) {
  const std::string specifier =
      std::string(hermes::node_compat::kEmbedderModulePrefix) + addon.name;

  const std::string exePath = executablePath();
  if (exePath.empty()) {
    const std::string message = "Cannot find module '" + specifier +
        "': the directory of the running executable could not be determined";
    napi_throw_error(env, "MODULE_NOT_FOUND", message.c_str());
    return nullptr;
  }
  const std::string path = pathBesideExecutable(exePath, addon.fileName);

  // The one case classified before the shared loader runs: an absent file
  // is MODULE_NOT_FOUND, which is what an optional-dependency probe
  // branches on, where dlopen would report it as ERR_DLOPEN_FAILED. Any
  // other stat() failure (a permission problem, say) is left for dlopen to
  // report, since the file may well be there.
  struct stat st;
  if (::stat(path.c_str(), &st) != 0 && (errno == ENOENT || errno == ENOTDIR)) {
    const std::string message =
        "Cannot find module '" + specifier + "': " + path + " does not exist";
    napi_throw_error(env, "MODULE_NOT_FOUND", message.c_str());
    return nullptr;
  }

  napi_value result = nullptr;
  if (!hermes::node_compat::loadNativeAddon(
          env, path.c_str(), exports, &result))
    return nullptr; // exception pending
  return result;
}

napi_value initImgui(napi_env env, napi_value exports) {
  return loadNamedAddon(env, kNamedAddons[0], exports);
}

} // namespace

bool registerNamedAddons(hermes::node_compat::HermesNodeConfig &config) {
  for (const NamedAddon &addon : kNamedAddons) {
    if (!hermes::node_compat::registerEmbedderModule(
            config,
            {std::string(hermes::node_compat::kEmbedderModulePrefix) +
                 addon.name,
             addon.init}))
      return false;
  }
  return true;
}
