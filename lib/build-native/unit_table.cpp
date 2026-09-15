/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <hermes/node-compat/build-exe/build_exe.h>
#include <hermes/node-compat/build-native/build_native.h>

namespace hermes {
namespace node_compat {

std::vector<std::string> unitSymbolTable(
    uint32_t moduleCount,
    const std::vector<NativeModuleJob> &jobs) {
  std::vector<std::string> table(moduleCount);
  for (const NativeModuleJob &job : jobs)
    if (job.moduleIndex < moduleCount)
      table[job.moduleIndex] = nativeUnitName(job.moduleIndex);
  return table;
}

std::string nativePayloadAssembly(
    const std::string &containerPath,
    const std::vector<std::string> &unitSymbols) {
  // One generated .s carries both the container and the table, so the two
  // cannot get out of step and the link gains one object rather than two.
  return payloadAssembly(containerPath, unitSymbols, hostObjectFormat());
}

} // namespace node_compat
} // namespace hermes
