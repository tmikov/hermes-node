/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <hermes/node-compat/build-native/build_native.h>

#include <hermes/node-compat/bundle/atomic_write.h>
#include <hermes/node-compat/bundle/cjs_wrapper.h>

#include <iomanip>
#include <sstream>

namespace hermes {
namespace node_compat {

namespace {
/// The six-digit stem every staged file for a module shares. std::setw is a
/// MINIMUM width, so an index past six digits widens rather than truncates.
std::string stem(uint32_t moduleIndex) {
  std::ostringstream os;
  os << std::setw(6) << std::setfill('0') << moduleIndex;
  return os.str();
}
} // namespace

std::string nativeUnitName(uint32_t moduleIndex) {
  return "hn_m" + stem(moduleIndex);
}

std::string stagedSourcePath(const std::string &tempDir, uint32_t i) {
  return tempDir + "/" + stem(i) + ".js";
}

std::string stagedCPath(const std::string &tempDir, uint32_t i) {
  return tempDir + "/" + stem(i) + ".c";
}

std::string stagedObjectPath(const std::string &tempDir, uint32_t i) {
  return tempDir + "/" + stem(i) + ".o";
}

bool stageModule(
    const std::string &tempDir,
    uint32_t i,
    std::string_view source,
    std::string *error) {
  // The same wrapper the scanner parsed and the bytecode producer compiles,
  // from the one header both already share -- so what shermes sees is what
  // the scan resolved require() bindings in.
  std::string wrapped = wrapCJS(source);
  // writeFileAtomically() reports to an ostream, not a std::string*; adapt
  // rather than reimplement the atomic-write sequence with std::ofstream,
  // which would lose the temp-file-then-rename guarantee the other bundle
  // writers rely on.
  std::ostringstream err;
  if (!writeFileAtomically(
          stagedSourcePath(tempDir, i), wrapped.data(), wrapped.size(), err)) {
    *error = err.str();
    return false;
  }
  return true;
}

} // namespace node_compat
} // namespace hermes
