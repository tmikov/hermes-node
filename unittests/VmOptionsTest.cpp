/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/node-compat/vm-options/vm_options.h"

#include "hermes/Public/RuntimeConfig.h"

#include "llvh/Support/CommandLine.h"

#include "gtest/gtest.h"

using namespace hermes::node_compat;

namespace {

TEST(VmOptionsTest, FlagNameStripsDashesAndValue) {
  EXPECT_EQ(vmOptionFlagName("-gc-max-heap=1g"), "gc-max-heap");
  EXPECT_EQ(vmOptionFlagName("--gc-max-heap=1g"), "gc-max-heap");
  EXPECT_EQ(vmOptionFlagName("-Xjit"), "Xjit");
  EXPECT_EQ(vmOptionFlagName("-Xjit=force"), "Xjit");
  // No leading dash at all is still a name; llvh::cl would reject it as a
  // positional, but classification happens first and must not crash.
  EXPECT_EQ(vmOptionFlagName("gc-max-heap=1g"), "gc-max-heap");
}

TEST(VmOptionsTest, HonouredFlagsAreHonoured) {
  // One from each family in the spec's table.
  EXPECT_EQ(classifyVmOption("gc-max-heap"), VmOptionStatus::kHonoured);
  EXPECT_EQ(classifyVmOption("gc-init-heap"), VmOptionStatus::kHonoured);
  EXPECT_EQ(classifyVmOption("gc-sanitize-handles"), VmOptionStatus::kHonoured);
  EXPECT_EQ(classifyVmOption("max-register-stack"), VmOptionStatus::kHonoured);
  EXPECT_EQ(classifyVmOption("Xjit"), VmOptionStatus::kHonoured);
  EXPECT_EQ(classifyVmOption("Xes6-block-scoping"), VmOptionStatus::kHonoured);
  EXPECT_EQ(classifyVmOption("Xasync-generators"), VmOptionStatus::kHonoured);
  EXPECT_EQ(classifyVmOption("enable-eval"), VmOptionStatus::kHonoured);
  EXPECT_EQ(classifyVmOption("test262"), VmOptionStatus::kHonoured);
}

TEST(VmOptionsTest, ConsoleHostFlagsAreRefused) {
  EXPECT_EQ(classifyVmOption("Xdump-jitcode"), VmOptionStatus::kConsoleHost);
  EXPECT_EQ(
      classifyVmOption("Xjit-crash-on-error"), VmOptionStatus::kConsoleHost);
  EXPECT_EQ(
      classifyVmOption("Xjit-emit-asserts"), VmOptionStatus::kConsoleHost);
  EXPECT_EQ(
      classifyVmOption("Xjit-emit-counters"), VmOptionStatus::kConsoleHost);
  EXPECT_EQ(classifyVmOption("Xjit-hc-id-limit"), VmOptionStatus::kConsoleHost);
  EXPECT_EQ(
      classifyVmOption("stop-after-module-init"), VmOptionStatus::kConsoleHost);
  EXPECT_EQ(classifyVmOption("Xheap-timeline"), VmOptionStatus::kConsoleHost);
  EXPECT_EQ(classifyVmOption("Xperf-prof"), VmOptionStatus::kConsoleHost);
  EXPECT_EQ(classifyVmOption("Xperf-prof-dir"), VmOptionStatus::kConsoleHost);
  EXPECT_EQ(classifyVmOption("sample-profiling"), VmOptionStatus::kConsoleHost);
  EXPECT_EQ(
      classifyVmOption("sample-profiling-freq"), VmOptionStatus::kConsoleHost);
  // These two set a real GCConfig/RuntimeConfig bit and would parse
  // cleanly, so they read as honourable; they are refused because the only
  // reader of what that bit produces is ConsoleHost, which this runtime
  // does not use. gc-print-stats is the sharper case: its name promises
  // printed output.
  EXPECT_EQ(classifyVmOption("gc-print-stats"), VmOptionStatus::kConsoleHost);
  EXPECT_EQ(classifyVmOption("track-io"), VmOptionStatus::kConsoleHost);
}

TEST(VmOptionsTest, UnknownFlagsAreUnknown) {
  EXPECT_EQ(classifyVmOption("gc-max-hep"), VmOptionStatus::kUnknown);
  EXPECT_EQ(classifyVmOption(""), VmOptionStatus::kUnknown);
  // llvh::cl's own help and version printers must never be reachable: they
  // would dump the whole option registry and exit the process.
  EXPECT_EQ(classifyVmOption("help"), VmOptionStatus::kUnknown);
  EXPECT_EQ(classifyVmOption("version"), VmOptionStatus::kUnknown);
}

TEST(VmOptionsTest, HonouredListMatchesClassifier) {
  auto names = honouredVmOptionNames();
  EXPECT_EQ(names.size(), 25u);
  for (const auto &n : names)
    EXPECT_EQ(classifyVmOption(n), VmOptionStatus::kHonoured) << n;
}

TEST(VmOptionsTest, EmptyOptionsPreserveHermesNodeDefaults) {
  hermes::vm::RuntimeConfig config;
  std::string error;
  ASSERT_TRUE(buildVmRuntimeConfig({}, &config, &error)) << error;
  // The three hermes-node forces on. Hermes defaults the first two to
  // false; regressing them is the failure mode this whole test exists for.
  EXPECT_TRUE(config.getES6BlockScoping());
  EXPECT_TRUE(config.getEnableAsyncGenerators());
  EXPECT_TRUE(config.getMicrotaskQueue());
}

TEST(VmOptionsTest, HonouredFlagChangesTheConfig) {
  hermes::vm::RuntimeConfig config;
  std::string error;
  ASSERT_TRUE(buildVmRuntimeConfig({"-Xes6-proxy=false"}, &config, &error))
      << error;
  EXPECT_FALSE(config.getES6Proxy());
  // Untouched fields keep hermes-node's defaults.
  EXPECT_TRUE(config.getES6BlockScoping());
}

TEST(VmOptionsTest, HermesNodeDefaultsCanBeOverridden) {
  hermes::vm::RuntimeConfig config;
  std::string error;
  ASSERT_TRUE(
      buildVmRuntimeConfig({"-Xasync-generators=false"}, &config, &error))
      << error;
  EXPECT_FALSE(config.getEnableAsyncGenerators());
}

TEST(VmOptionsTest, LastOccurrenceWins) {
  hermes::vm::RuntimeConfig config;
  std::string error;
  ASSERT_TRUE(buildVmRuntimeConfig(
      {"-Xes6-proxy=false", "-Xes6-proxy=true"}, &config, &error));
  EXPECT_TRUE(config.getES6Proxy());
}

TEST(VmOptionsTest, ValuesDoNotLeakBetweenCalls) {
  // ES6Proxy is an ordinary honoured flag, gated by wasGiven() like every
  // other field in the mapping (see the invariant documented at
  // buildVmRuntimeConfig() and the comment at the mapping in
  // vm_options.cpp). A first call sets it away from Hermes's own
  // default...
  hermes::vm::RuntimeConfig first;
  std::string error;
  ASSERT_TRUE(buildVmRuntimeConfig({"-Xes6-proxy=false"}, &first, &error))
      << error;
  ASSERT_FALSE(first.getES6Proxy());

  // ...and a second call that never mentions -Xes6-proxy at all must see
  // Hermes's own default (true), not the value the first call left
  // behind. llvh::cl::ResetAllOptionOccurrences() alone does not
  // guarantee this: it clears occurrence counts, not values.
  hermes::vm::RuntimeConfig second;
  ASSERT_TRUE(buildVmRuntimeConfig({}, &second, &error)) << error;
  EXPECT_TRUE(second.getES6Proxy());
}

TEST(VmOptionsTest, MemorySizeValuesDoNotLeakBetweenCalls) {
  // gc-max-heap is llvh::cl::opt<MemorySize, ...>, and MemorySize is a
  // plain struct with no OptionValue<MemorySize> specialization -- so
  // llvh's setDefault() cannot restore it (see the comment at the
  // gcBuilder mapping in vm_options.cpp). Every field in the mapping is
  // wasGiven()-gated now (ValuesDoNotLeakBetweenCalls above covers an
  // ordinary bool field), but this one is the reason the gating cannot
  // rely on the setDefault() sweep at all -- it is the sole protection.
  hermes::vm::RuntimeConfig first;
  std::string error;
  ASSERT_TRUE(buildVmRuntimeConfig({"-gc-max-heap=64m"}, &first, &error))
      << error;
  ASSERT_EQ(first.getGCConfig().getMaxHeapSize(), 64u * 1024 * 1024);

  hermes::vm::RuntimeConfig second;
  ASSERT_TRUE(buildVmRuntimeConfig({}, &second, &error)) << error;
  EXPECT_EQ(
      second.getGCConfig().getMaxHeapSize(),
      hermes::vm::GCConfig::getDefaultMaxHeapSize());
}

TEST(VmOptionsTest, MemorySizeSuffixesParse) {
  hermes::vm::RuntimeConfig config;
  std::string error;
  ASSERT_TRUE(buildVmRuntimeConfig({"-gc-max-heap=64m"}, &config, &error))
      << error;
  EXPECT_EQ(config.getGCConfig().getMaxHeapSize(), 64u * 1024 * 1024);
}

TEST(VmOptionsTest, RefusedFlagReportsWhy) {
  hermes::vm::RuntimeConfig config;
  std::string error;
  EXPECT_FALSE(buildVmRuntimeConfig({"-Xdump-jitcode=1"}, &config, &error));
  EXPECT_NE(error.find("Xdump-jitcode"), std::string::npos);
  EXPECT_NE(error.find("ConsoleHost"), std::string::npos);
}

TEST(VmOptionsTest, UnknownFlagIsAnError) {
  hermes::vm::RuntimeConfig config;
  std::string error;
  EXPECT_FALSE(buildVmRuntimeConfig({"-gc-max-hep=1g"}, &config, &error));
  EXPECT_NE(error.find("gc-max-hep"), std::string::npos);
}

TEST(VmOptionsTest, BadValueIsAnErrorAndDoesNotExit) {
  hermes::vm::RuntimeConfig config;
  std::string error;
  // The point of this case is as much that the process survives as that it
  // returns false: llvh::cl calls exit() unless an Errs stream is passed.
  EXPECT_FALSE(buildVmRuntimeConfig({"-gc-max-heap=banana"}, &config, &error));
  EXPECT_FALSE(error.empty());
}

namespace {

/// Constructs the RuntimeFlags instance (a side effect of any entry point
/// that parses) so llvh::cl's registry holds Hermes's VM options, then
/// answers whether \p name is one of them.
bool registeredWithLlvh(std::string_view name) {
  hermes::vm::RuntimeConfig ignored;
  std::string error;
  EXPECT_TRUE(buildVmRuntimeConfig({}, &ignored, &error)) << error;
  auto &registry = llvh::cl::getRegisteredOptions();
  return registry.find(llvh::StringRef(name.data(), name.size())) !=
      registry.end();
}

} // namespace

// The feature's stated value is that spellings come from Hermes and cannot
// drift from the binary they are supposed to match. Nothing enforced that:
// HelpTextNamesEveryHonouredFlag below passes for a name Hermes does not
// register, because vmOptionsHelpText() prints "  -" << name
// unconditionally and only the *description* comes from the registry. So a
// Hermes rename, or a typo in kHonoured, would degrade the flag to a
// confusing double error at run time -- our classifier says "honoured",
// llvh::cl then says "unknown argument" -- with no test failing.
TEST(VmOptionsTest, EveryHonouredNameIsRegisteredWithHermes) {
  for (const auto &n : honouredVmOptionNames())
    EXPECT_TRUE(registeredWithLlvh(n)) << n;
}

// The refused list has the same exposure with a worse failure mode: a name
// Hermes no longer registers would be refused with a confident explanation
// of why hermes-node cannot support a flag that does not exist.
//
// Two names are exempt, and only these two. Xperf-prof and Xperf-prof-dir
// are declared inside "#ifdef HERMES_ENABLE_PERF_PROF"
// (hermes/include/hermes/VM/RuntimeFlags.h:261), which no configuration
// this repo builds defines, so they are genuinely absent from the registry
// here. They stay in kConsoleHostOnly all the same: they are real Hermes
// flags, ConsoleHost is what would implement them (ConsoleHost.cpp:974),
// and naming them is a better answer than "unknown VM option" whether or
// not this build of Hermes compiled them in. Testing them by define is not
// an option either, since HERMES_ENABLE_PERF_PROF does not reach this
// directory. The exemption is spelled out rather than made general so that
// a third name joining it is a deliberate edit.
TEST(VmOptionsTest, EveryRefusedNameIsRegisteredWithHermes) {
  for (const auto &n : refusedVmOptionNames()) {
    if (n == "Xperf-prof" || n == "Xperf-prof-dir")
      continue;
    EXPECT_TRUE(registeredWithLlvh(n)) << n;
  }
}

TEST(VmOptionsTest, RefusedListMatchesClassifier) {
  auto names = refusedVmOptionNames();
  EXPECT_EQ(names.size(), 13u);
  for (const auto &n : names)
    EXPECT_EQ(classifyVmOption(n), VmOptionStatus::kConsoleHost) << n;
}

TEST(VmOptionsTest, HelpTextNamesEveryHonouredFlag) {
  std::string help = vmOptionsHelpText();
  for (const auto &n : honouredVmOptionNames())
    EXPECT_NE(help.find(std::string(n)), std::string::npos) << n;
}

// buildVmRuntimeConfig({}) must equal, field by field, the hardcoded
// three-call builder it replaced (the exact code that was at
// hermes_node_runtime.cpp:691 before this feature existed). This is the
// test none of the four default-divergence bugs found in review had:
// gc-init-heap/gc-max-heap, gc-sanitize-handles/-random-seed and
// emit-async-break-check/verify-ir each silently changed hermes-node's
// default behavior for months of not being wired to a real runtime, and
// each would have failed exactly one EXPECT_EQ below the moment it was
// introduced, rather than surfacing later as an unrelated test failing
// under check-hermes-node (or, for the first three, not surfacing until
// this test was written at all). Written out one field per line rather
// than in a loop or a table: the verbosity is the point, since a future
// field added to the mapping without a matching line here is a gap this
// test cannot see, and that gap should be visible in a diff.
TEST(VmOptionsTest, EmptyOptionsMatchHardcodedBuilderFieldByField) {
  hermes::vm::RuntimeConfig expected = hermes::vm::RuntimeConfig::Builder()
                                           .withMicrotaskQueue(true)
                                           .withEnableAsyncGenerators(true)
                                           .withES6BlockScoping(true)
                                           .build();

  hermes::vm::RuntimeConfig actual;
  std::string error;
  ASSERT_TRUE(buildVmRuntimeConfig({}, &actual, &error)) << error;

  // RuntimeConfig fields the mapping writes, plus TrackIO, which it no
  // longer writes (-track-io moved to kConsoleHostOnly): the line is kept
  // because the field is still one a future edit could reach.
  EXPECT_EQ(actual.getMaxNumRegisters(), expected.getMaxNumRegisters());
  EXPECT_EQ(actual.getEnableJIT(), expected.getEnableJIT());
  EXPECT_EQ(actual.getForceJIT(), expected.getForceJIT());
  EXPECT_EQ(actual.getJITThreshold(), expected.getJITThreshold());
  EXPECT_EQ(actual.getJITMemoryLimit(), expected.getJITMemoryLimit());
  EXPECT_EQ(actual.getEnableEval(), expected.getEnableEval());
  EXPECT_EQ(actual.getVerifyEvalIR(), expected.getVerifyEvalIR());
  EXPECT_EQ(actual.getOptimizedEval(), expected.getOptimizedEval());
  EXPECT_EQ(
      actual.getAsyncBreakCheckInEval(), expected.getAsyncBreakCheckInEval());
  EXPECT_EQ(actual.getVMExperimentFlags(), expected.getVMExperimentFlags());
  EXPECT_EQ(actual.getES6Proxy(), expected.getES6Proxy());
  EXPECT_EQ(actual.getIntl(), expected.getIntl());
  EXPECT_EQ(
      actual.getRandomizeMemoryLayout(), expected.getRandomizeMemoryLayout());
  EXPECT_EQ(actual.getTrackIO(), expected.getTrackIO());
  EXPECT_EQ(
      actual.getEnableHermesInternal(), expected.getEnableHermesInternal());
  EXPECT_EQ(
      actual.getEnableHermesInternalTestMethods(),
      expected.getEnableHermesInternalTestMethods());
  EXPECT_EQ(actual.getTest262(), expected.getTest262());
  EXPECT_EQ(actual.getES6BlockScoping(), expected.getES6BlockScoping());
  EXPECT_EQ(
      actual.getEnableAsyncGenerators(), expected.getEnableAsyncGenerators());
  EXPECT_EQ(actual.getMicrotaskQueue(), expected.getMicrotaskQueue());

  // GCConfig fields the mapping writes, plus ShouldRecordStats, which it
  // no longer writes (-gc-print-stats moved to kConsoleHostOnly), kept for
  // the same reason as TrackIO above.
  const hermes::vm::GCConfig &actualGC = actual.getGCConfig();
  const hermes::vm::GCConfig &expectedGC = expected.getGCConfig();
  EXPECT_EQ(actualGC.getInitHeapSize(), expectedGC.getInitHeapSize());
  EXPECT_EQ(actualGC.getMaxHeapSize(), expectedGC.getMaxHeapSize());
  EXPECT_EQ(actualGC.getOccupancyTarget(), expectedGC.getOccupancyTarget());
  EXPECT_EQ(
      actualGC.getSanitizeConfig().getSanitizeRate(),
      expectedGC.getSanitizeConfig().getSanitizeRate());
  EXPECT_EQ(
      actualGC.getSanitizeConfig().getRandomSeed(),
      expectedGC.getSanitizeConfig().getRandomSeed());
  EXPECT_EQ(actualGC.getShouldRecordStats(), expectedGC.getShouldRecordStats());
  EXPECT_EQ(actualGC.getAllocInYoung(), expectedGC.getAllocInYoung());
  EXPECT_EQ(actualGC.getRevertToYGAtTTI(), expectedGC.getRevertToYGAtTTI());
  // ShouldReleaseUnused is not a --vm= flag -- there is no cl::opt behind
  // it -- and the mapping deliberately does not write it, so it keeps
  // GCConfig::Builder()'s own default (kReleaseUnusedOld) on both sides.
  // It was briefly copied from hermes.cpp, which sets kReleaseUnusedNone;
  // that made every plain hermes-node run stop returning old-generation
  // memory to the OS with nothing on the command line asking for it, and
  // it is the reason this line asserts equality rather than documenting
  // an exception. See the comment where the call used to be, in
  // vm_options.cpp.
  EXPECT_EQ(
      actualGC.getShouldReleaseUnused(), expectedGC.getShouldReleaseUnused());
}

TEST(VmOptionsTest, SplitVmOptionsEnvNull) {
  EXPECT_TRUE(splitVmOptionsEnv(nullptr).empty());
}

TEST(VmOptionsTest, SplitVmOptionsEnvEmpty) {
  EXPECT_TRUE(splitVmOptionsEnv("").empty());
}

TEST(VmOptionsTest, SplitVmOptionsEnvAllWhitespace) {
  // All-whitespace is indistinguishable from "not set" to a caller that
  // only cares whether there is anything to apply, which is the point of
  // returning an empty vector rather than a vector holding one empty
  // string.
  EXPECT_TRUE(splitVmOptionsEnv("   \t  ").empty());
}

TEST(VmOptionsTest, SplitVmOptionsEnvOneFlag) {
  std::vector<std::string> result = splitVmOptionsEnv("-Xes6-proxy=true");
  ASSERT_EQ(result.size(), 1u);
  EXPECT_EQ(result[0], "-Xes6-proxy=true");
}

TEST(VmOptionsTest, SplitVmOptionsEnvSeveralFlags) {
  std::vector<std::string> result =
      splitVmOptionsEnv("-Xes6-proxy=true -gc-max-heap=1g");
  ASSERT_EQ(result.size(), 2u);
  EXPECT_EQ(result[0], "-Xes6-proxy=true");
  EXPECT_EQ(result[1], "-gc-max-heap=1g");
}

TEST(VmOptionsTest, SplitVmOptionsEnvLeadingTrailingRepeatedWhitespace) {
  std::vector<std::string> result =
      splitVmOptionsEnv("  \t -Xes6-proxy=true \t\t  -gc-max-heap=1g  ");
  ASSERT_EQ(result.size(), 2u);
  EXPECT_EQ(result[0], "-Xes6-proxy=true");
  EXPECT_EQ(result[1], "-gc-max-heap=1g");
}

} // namespace
