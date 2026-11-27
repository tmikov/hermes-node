/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_NODE_UI_NAMED_ADDONS_H
#define HERMES_NODE_UI_NAMED_ADDONS_H

#include <hermes/node-compat/runtime/hermes_node_runtime.h>

#include <string>

/// Registers one embedder module per named addon: `hermes-node:<name>`,
/// whose init loads `<file>` from the directory of the running executable
/// (realpath'd) through the loader `process.dlopen` uses, and returns that
/// file's exports. The table is in named_addons.cpp; it has one row,
/// `imgui` -> `hermes-node-imgui.node`.
///
/// An absent file throws `MODULE_NOT_FOUND` naming the full path tried; a
/// file that exists and does not load is `ERR_DLOPEN_FAILED`, as from
/// `process.dlopen`. Nothing is cached on failure, so a later `require()`
/// tries again.
///
/// Returns false, having printed why, when a registration is refused (see
/// `registerEmbedderModule()`). Call before the runtime starts.
bool registerNamedAddons(hermes::node_compat::HermesNodeConfig &config);

/// The path of \p fileName in the directory that holds \p exePath: the
/// directory part of \p exePath, separator included, then \p fileName, so
/// an executable directly in `/` gives `/<fileName>`. A path with no
/// separator at all is taken to be in the current directory. Pure, so a
/// unit test reaches it without an executable in `/`.
inline std::string pathBesideExecutable(
    const std::string &exePath,
    const char *fileName) {
  const size_t slash = exePath.find_last_of('/');
  if (slash == std::string::npos)
    return std::string("./") + fileName;
  return exePath.substr(0, slash + 1) + fileName;
}

#endif // HERMES_NODE_UI_NAMED_ADDONS_H
