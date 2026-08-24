/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_NODE_COMPAT_BUILD_EXE_KIT_MANIFEST_H
#define HERMES_NODE_COMPAT_BUILD_EXE_KIT_MANIFEST_H

#include <optional>
#include <string>
#include <vector>

namespace hermes {
namespace node_compat {

/// The parsed form of a kit's `kit.manifest` file (written by
/// utils/make-kit.py; see the format comment in kit_manifest.cpp). Pure
/// data -- no filesystem or linker access happens past readKitManifest()
/// returning it.
struct KitManifest {
  /// The directory this was read from. Carried on the struct because every
  /// consumer needs it -- {kit} substitution above, and the entry object at
  /// <kitDir>/hermes-node-bundle-main.o -- and threading it separately
  /// through each call is one more chance for the two to disagree.
  std::string kitDir;
  std::string version;
  std::string cc;
  std::vector<std::string> driverFlags;
  std::vector<std::string> linkArgs; // {kit} already substituted
};

/// Reads and parses `<kitDir>/kit.manifest`. Returns std::nullopt with a
/// human-readable message on \p error if the file cannot be opened, a
/// required key (`version`, `cc`) is missing, or a line's key is not one
/// this reader knows -- an unknown key means the kit was cut by a newer
/// make-kit.py recording something this reader would otherwise silently
/// drop.
std::optional<KitManifest> readKitManifest(
    const std::string &kitDir,
    std::string *error);

} // namespace node_compat
} // namespace hermes

#endif // HERMES_NODE_COMPAT_BUILD_EXE_KIT_MANIFEST_H
