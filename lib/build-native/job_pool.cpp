/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <hermes/node-compat/build-native/build_native.h>

#include <sys/stat.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

namespace hermes {
namespace node_compat {

namespace {

size_t fileSize(const std::string &path) {
  struct stat st;
  return ::stat(path.c_str(), &st) == 0 ? (size_t)st.st_size : 0;
}

/// One module, start to object file.
NativeModuleResult runOne(
    const NativeModuleJob &job,
    const std::string &tempDir,
    const std::string &shermesPath,
    const KitManifest &manifest,
    const std::string &driver,
    bool driverIsClang,
    OptLevel opt,
    const CommandRunner &run) {
  NativeModuleResult result;
  result.moduleIndex = job.moduleIndex;
  result.sourceBytes = job.source.size();
  auto start = std::chrono::steady_clock::now();

  std::string error;
  if (!stageModule(tempDir, job.moduleIndex, job.source, &error)) {
    result.failedStage = NativeStage::Stage;
    result.message = error;
    return result;
  }

  std::string cPath = stagedCPath(tempDir, job.moduleIndex);
  std::vector<std::string> shermesArgv = buildShermesCommand(
      shermesPath,
      stagedSourcePath(tempDir, job.moduleIndex),
      cPath,
      job.identity,
      nativeUnitName(job.moduleIndex),
      job.typeScript,
      opt);
  CommandResult sh = run(shermesArgv);
  // Recorded before either branch below, so the exited-0-but-no-output-file
  // case two lines down carries the same (Exited, 0) pair the orchestrator
  // would see if it inspected sh directly -- see NativeModuleResult's own
  // doc comment for why that pair, not just diagnostics, is what the
  // stub-or-fail classification needs.
  result.outcome = sh.outcome;
  result.exitStatus = sh.status;
  if (!sh.ok()) {
    result.failedStage = NativeStage::Shermes;
    result.diagnostics = sh.output;
    result.failedCommand = shermesArgv;
    result.message = describeCommandResult(sh, "shermes");
    return result;
  }
  // A tool that exits 0 without producing its output is a failure of THIS
  // module at THIS stage. fileSize() cannot tell "missing" from "empty", so
  // check for a non-zero size: without this the module sails through and
  // the absence surfaces much later as an undefined symbol at link time,
  // attributed to nothing.
  result.cBytes = fileSize(cPath);
  if (result.cBytes == 0) {
    result.failedStage = NativeStage::Shermes;
    result.failedCommand = shermesArgv;
    result.diagnostics = sh.output;
    result.message = "shermes exited 0 but wrote no output file";
    return result;
  }

  std::string objPath = stagedObjectPath(tempDir, job.moduleIndex);
  std::vector<std::string> ccArgv =
      buildCompileCommand(manifest, driver, driverIsClang, cPath, objPath, opt);
  CommandResult cc = run(ccArgv);
  result.outcome = cc.outcome;
  result.exitStatus = cc.status;
  if (!cc.ok()) {
    result.failedStage = NativeStage::Compile;
    result.diagnostics = cc.output;
    result.failedCommand = ccArgv;
    result.message = describeCommandResult(cc, "the C compiler");
    return result;
  }
  result.objectBytes = fileSize(objPath);
  if (result.objectBytes == 0) {
    result.failedStage = NativeStage::Compile;
    result.failedCommand = ccArgv;
    result.diagnostics = cc.output;
    result.message = "the C compiler exited 0 but wrote no object file";
    return result;
  }

  result.ok = true;
  result.milliseconds = std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - start)
                            .count();
  return result;
}

} // namespace

std::vector<NativeModuleResult> compileModules(
    const std::vector<NativeModuleJob> &jobs,
    const std::string &tempDir,
    const std::string &shermesPath,
    const KitManifest &manifest,
    const std::string &driver,
    bool driverIsClang,
    OptLevel opt,
    unsigned parallelism,
    const CommandRunner &run) {
  // Slots pre-sized and written by index, so results come back in job order
  // whatever order the workers finish in -- no mutex on the output, and no
  // sort afterwards.
  std::vector<NativeModuleResult> results(jobs.size());

  if (parallelism == 0)
    parallelism = 1;
  unsigned workers =
      (unsigned)std::min<size_t>(parallelism, std::max<size_t>(jobs.size(), 1));

  std::atomic<size_t> next{0};
  auto worker = [&]() {
    for (;;) {
      size_t i = next.fetch_add(1);
      if (i >= jobs.size())
        return;
      results[i] = runOne(
          jobs[i],
          tempDir,
          shermesPath,
          manifest,
          driver,
          driverIsClang,
          opt,
          run);
    }
  };

  if (workers <= 1) {
    worker();
  } else {
    std::vector<std::thread> pool;
    pool.reserve(workers);
    for (unsigned i = 0; i < workers; ++i)
      pool.emplace_back(worker);
    for (std::thread &t : pool)
      t.join();
  }
  return results;
}

bool isNativeSourceRejection(
    bool isEntry,
    bool isPreload,
    const NativeModuleResult &result) {
  return !isEntry && !isPreload && result.failedStage == NativeStage::Shermes &&
      result.outcome == CommandResult::Outcome::Exited &&
      result.exitStatus != 0 && !result.diagnostics.empty();
}

} // namespace node_compat
} // namespace hermes
