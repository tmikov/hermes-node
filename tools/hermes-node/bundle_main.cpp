/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/// main() for an executable produced by --build-exe.
///
/// This is the whole difference between hermes-node and an app built from
/// it. There is no flag parsing: the bundle is the program, so every
/// argument belongs to it, exactly as a positional argument does under
/// --bundle. And there is no fuse -- the payload symbols below are resolved
/// by the object --build-exe generates, so a binary that links this file
/// always has a bundle and a binary that does not never sees these symbols.

#include <hermes/node-compat/bundle/bundle_run.h>
#include <hermes/node-compat/runtime/hermes_node_runtime.h>
#include <hermes/node-compat/vm-options/vm_options.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using hermes::node_compat::BundleVmOptions;
using hermes::node_compat::HermesNodeConfig;
using hermes::node_compat::readEmbeddedBundleVmOptions;
using hermes::node_compat::runHermesNode;
using hermes::node_compat::splitVmOptionsEnv;

extern "C" {
/// Defined by the generated payload object. Arrays rather than pointers:
/// the symbols mark positions in a read-only section, and the linker
/// computes the length as their difference, so no stored size can disagree
/// with the bytes.
extern const uint8_t hermesNodeBundleStart[];
extern const uint8_t hermesNodeBundleEnd[];
}

int main(int argc, char **argv) {
  HermesNodeConfig config;
  config.embeddedBundleData = hermesNodeBundleStart;
  config.embeddedBundleSize =
      static_cast<size_t>(hermesNodeBundleEnd - hermesNodeBundleStart);

  // The container's options, then HERMES_NODE_VM_OPTIONS if the container
  // allows it. There is no --vm= here: every argument this binary receives
  // belongs to the program, which is what makes process.argv.slice(2) mean
  // the same thing it means under --bundle=<f> arg.
  BundleVmOptions bundleVm;
  std::string vmError;
  if (!readEmbeddedBundleVmOptions(
          config.embeddedBundleData,
          config.embeddedBundleSize,
          &bundleVm,
          &vmError)) {
    std::fprintf(stderr, "error: %s\n", vmError.c_str());
    return 1;
  }
  std::vector<std::string> envVm =
      splitVmOptionsEnv(std::getenv("HERMES_NODE_VM_OPTIONS"));
  if (!bundleVm.allowOverride && !envVm.empty()) {
    std::fprintf(
        stderr,
        "Error: this executable's VM options are locked and cannot be "
        "overridden.\n"
        "       HERMES_NODE_VM_OPTIONS is set in the environment.\n"
        "       Rebuild the container with: --build-bundle "
        "--allow-vm-options-override\n"
        "       then link it again with: --build-exe\n");
    return 1;
  }
  config.process.vmOptions = std::move(bundleVm.options);
  config.process.vmOptions.insert(
      config.process.vmOptions.end(), envVm.begin(), envVm.end());

  // process.argv is [exePath, exePath, ...userArgs]: the executable's own
  // path occupies the slot a script path occupies everywhere else, so user
  // arguments start at index 2 and `process.argv.slice(2)` -- what every
  // argument parser in the ecosystem does -- means the same thing here as
  // it does under `--bundle=<f> arg`, where the slot holds the container
  // path. Node's single-executable applications fill it the same way and
  // for the same reason. Pushing argv verbatim instead would shift every
  // user argument down one and silently break the first one.
  if (argc > 0)
    config.argv.push_back(argv[0]);
  for (int i = 0; i < argc; ++i)
    config.argv.push_back(argv[i]);

  return runHermesNode(config);
}
