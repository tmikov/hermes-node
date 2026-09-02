/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/node-compat/vm-options/vm_options.h"

// Drags in hermes/ConsoleHost/ConsoleHost.h, and through it
// hermes/VM/Runtime.h, whose member layout branches on
// HERMES_CHECK_NATIVE_STACK and HERMES_MEMORY_INSTRUMENTATION. Hermes sets
// both with a directory-scoped add_definitions() that does not reach this
// directory, so lib/vm-options/CMakeLists.txt propagates them from the same
// CMake variables -- see the comment there. NO TYPE FROM Runtime.h MAY BE
// NAMED IN THIS TU: only the cl::opt members of hermes::cli::RuntimeFlags
// and the vm::RuntimeConfig / vm::GCConfig builders are used here, and
// keeping it that way is what makes the layout question a build-system
// detail rather than a correctness one.
#include "hermes/VM/RuntimeFlags.h"

#include "llvh/Support/CommandLine.h"
#include "llvh/Support/raw_ostream.h"

#include <algorithm>
#include <array>
#include <sstream>
#include <unordered_map>

namespace hermes {
namespace node_compat {

namespace {

/// The flags that reach vm::RuntimeConfig or vm::GCConfig, mirroring the
/// mapping the hermes binary itself uses at
/// hermes/tools/hermes/hermes.cpp:110-141. When Hermes moves, diff against
/// that function: it is the authority, not
/// hermes::cli::buildRuntimeConfig(), which omits ES6BlockScoping,
/// EnableAsyncGenerators, Test262 and every JIT field.
constexpr std::array<std::string_view, 25> kHonoured = {
    // GCConfig.
    "gc-init-heap",
    "gc-max-heap",
    "occupancy-target",
    "gc-sanitize-handles",
    "gc-sanitize-handles-random-seed",
    "gc-alloc-young",
    "gc-revert-to-yg-at-tti",
    // RuntimeConfig.
    "max-register-stack",
    "Xjit",
    "Xjit-threshold",
    "Xjit-memory-limit",
    "Xes6-proxy",
    "Xes6-block-scoping",
    "Xasync-generators",
    "Xintl",
    "Xmicrotask-queue",
    "Xvm-experiment-flags",
    "Xrandomize-memory-layout",
    "enable-hermes-internal",
    "Xhermes-internal-test-methods",
    "enable-eval",
    "verify-ir",
    "optimized-eval",
    "emit-async-break-check",
    "test262",
};

/// Real Hermes flags whose effect hermes-node cannot deliver, because the
/// half that reports it lives in ConsoleHost. Listed rather than lumped
/// into kUnknown so the error can say why the flag is refused instead of
/// implying the user misspelled it.
///
/// Most of them only ever reach ExecuteOptions, a ConsoleHost concept.
/// Three do set a real RuntimeConfig or GCConfig bit and would parse and
/// configure cleanly -- and are refused anyway, because the only reader of
/// that bit's result is ConsoleHost, so accepting them would mean real
/// work done and nothing reported:
///   - sample-profiling sets withEnableSampleProfiling; the half that
///     writes the profile out is ConsoleHost.cpp:1094-1112.
///   - gc-print-stats sets GCConfig::shouldRecordStats, which makes GCBase
///     record statistics (GCBase.cpp:51) that only
///     Runtime::printHeapStats prints -- reached from ConsoleHost.cpp:1137,
///     from jsi::Instrumentation::getRecordedGCStats and from Android
///     platform logging, none of which this runtime uses. Its name
///     promises printed output, which makes it the worst of the three to
///     accept in silence.
///   - track-io sets RuntimeConfig::trackIO, which makes Runtime attach a
///     page-access tracker (Runtime.cpp:1191) that only
///     Runtime::getIOTrackingInfoJSON reports -- reached from
///     ConsoleHost.cpp:1140 and from jsi::Instrumentation, neither of
///     which this runtime uses.
/// Supporting any of the three means deciding where its output goes, which
/// is a feature in its own right and out of scope here.
constexpr std::array<std::string_view, 13> kConsoleHostOnly = {
    "sample-profiling",
    "sample-profiling-freq",
    "gc-print-stats",
    "track-io",
    "stop-after-module-init",
    "Xheap-timeline",
    "Xdump-jitcode",
    "Xperf-prof",
    "Xperf-prof-dir",
    "Xjit-crash-on-error",
    "Xjit-emit-asserts",
    "Xjit-emit-counters",
    "Xjit-hc-id-limit",
};

} // namespace

std::string_view vmOptionFlagName(std::string_view arg) {
  while (!arg.empty() && arg.front() == '-')
    arg.remove_prefix(1);
  size_t eq = arg.find('=');
  if (eq != std::string_view::npos)
    arg = arg.substr(0, eq);
  return arg;
}

VmOptionStatus classifyVmOption(std::string_view flagName) {
  if (std::find(kHonoured.begin(), kHonoured.end(), flagName) !=
      kHonoured.end())
    return VmOptionStatus::kHonoured;
  if (std::find(kConsoleHostOnly.begin(), kConsoleHostOnly.end(), flagName) !=
      kConsoleHostOnly.end())
    return VmOptionStatus::kConsoleHost;
  return VmOptionStatus::kUnknown;
}

std::vector<std::string_view> honouredVmOptionNames() {
  return {kHonoured.begin(), kHonoured.end()};
}

std::vector<std::string_view> refusedVmOptionNames() {
  return {kConsoleHostOnly.begin(), kConsoleHostOnly.end()};
}

namespace {

/// hermes-node's own defaults, applied to any field the user did not name.
/// These are the whole reason this function does not simply call
/// hermes::cli::buildRuntimeConfig(): Hermes defaults the first two to
/// false, and turning them off would silently remove async generators and
/// block scoping from every program.
constexpr bool kDefaultES6BlockScoping = true;
constexpr bool kDefaultAsyncGenerators = true;
constexpr bool kDefaultMicrotaskQueue = true;

/// One instance for the process. Constructing it registers every VM option
/// with llvh::cl, which is a process-global registry, so this must happen
/// exactly once -- hence the function-local static.
hermes::cli::RuntimeFlags &runtimeFlags() {
  static hermes::cli::RuntimeFlags flags;
  return flags;
}

/// True when the user named \p opt on the command line, rather than it
/// merely holding its default.
template <typename T>
bool wasGiven(const T &opt) {
  return opt.getNumOccurrences() > 0;
}

} // namespace

bool buildVmRuntimeConfig(
    const std::vector<std::string> &options,
    hermes::vm::RuntimeConfig *out,
    std::string *error) {
  hermes::cli::RuntimeFlags &flags = runtimeFlags();

  // ParseCommandLineOptions does not clear prior state, so a second call
  // in the same process would otherwise see leftovers from the first.
  // Two separate resets are needed, because they undo two separate kinds
  // of leftover and neither does the other's job:
  //
  //  - ResetAllOptionOccurrences() restores each option's *occurrence
  //    count* to zero. This one is load-bearing for correctness:
  //    wasGiven() below is getNumOccurrences() > 0, and every field in
  //    the mapping is read only inside a wasGiven() check (see the
  //    comment at the mapping), so a stale occurrence count from an
  //    earlier call would make this call apply a value the caller never
  //    passed.
  //  - setDefault() restores each option's *value* to its cl::init(...).
  //    Given the point above, a stale value can no longer leak into the
  //    output on its own -- it is only ever read when wasGiven() is true,
  //    which (after the occurrence reset) means this call's own parse
  //    just set it. This sweep is kept anyway, as a second line of
  //    defense: it is what makes a future field added to the mapping
  //    *without* a wasGiven() guard merely wrong-by-omission (missing an
  //    override) instead of wrong in a way that depends on process
  //    history (which --vm= list happened to run first). Note that it is
  //    a silent no-op for a class-typed option value (no OptionValue<T>
  //    specialization -- InitHeapSize and MaxHeapSize, both
  //    llvh::cl::opt<MemorySize, ...>, are the two such options here); see
  //    the comment at their use below for why their gating cannot rely on
  //    this sweep at all.
  for (auto &entry : llvh::cl::getRegisteredOptions())
    entry.second->setDefault();
  llvh::cl::ResetAllOptionOccurrences();

  // Classify before parsing. Three things depend on doing it in this
  // order: a refused flag gets our message instead of silently doing
  // nothing, an unknown flag gets our message instead of LLVM's, and
  // -help never reaches llvh::cl's help printer, which would dump the
  // whole registry and exit the process.
  for (const std::string &opt : options) {
    std::string_view name = vmOptionFlagName(opt);
    switch (classifyVmOption(name)) {
      case VmOptionStatus::kHonoured:
        break;
      case VmOptionStatus::kConsoleHost:
        *error = "'-" + std::string(name) +
            "' is not supported by hermes-node.\n"
            "       It is implemented by Hermes's ConsoleHost, which this "
            "runtime does not use.\n"
            "       Run --vm-help for the flags that are supported.";
        return false;
      case VmOptionStatus::kUnknown:
        *error = "unknown VM option '" + opt +
            "'.\n       Run --vm-help for the flags that are supported.";
        return false;
    }
  }

  // Deduplicate repeated flags, keeping the last occurrence of each --
  // "last occurrence wins," per this function's contract. Applied
  // uniformly, regardless of any individual option's occurrence policy:
  // most of the VM options are a plain (non-list) llvh::cl::opt, which
  // defaults to NumOccurrencesFlag::Optional and errors with "may only
  // occur zero or one times!" on a second sighting (Option::addOccurrence,
  // CommandLine.cpp:1439-1442), so for those a real repeat must never
  // reach llvh::cl as more than one occurrence. (Xjit is the one
  // exception -- it declares cl::ZeroOrMore and would tolerate repeats on
  // its own -- but deduplicating it too is harmless and keeps this pass
  // from needing to special-case one option.)
  std::vector<std::string> deduped;
  std::unordered_map<std::string_view, size_t> indexOfName;
  for (const std::string &opt : options) {
    std::string_view name = vmOptionFlagName(opt);
    auto it = indexOfName.find(name);
    if (it != indexOfName.end())
      deduped[it->second] = opt;
    else {
      indexOfName.emplace(name, deduped.size());
      deduped.push_back(opt);
    }
  }

  // A synthesized argv: llvh::cl sees only what --vm= collected, never
  // hermes-node's own arguments.
  std::vector<const char *> argv;
  argv.push_back("hermes-node --vm");
  for (const std::string &opt : deduped)
    argv.push_back(opt.c_str());

  std::string errText;
  llvh::raw_string_ostream errStream(errText);
  // Passing errStream is load-bearing for one thing only: it makes
  // ParseCommandLineOptions return false instead of calling exit() on any
  // failure (CommandLine.cpp:1108-1110, IgnoreErrors = Errs != nullptr).
  // It is NOT load-bearing for capturing the message. Errs is threaded
  // through only to the handful of errors ParseCommandLineOptions prints
  // itself (unknown argument, wrong positional-argument count); a
  // per-option failure -- a repeat occurrence, a bad value such as
  // "-gc-max-heap=banana" -- goes through Option::error(), whose Errs
  // parameter defaults to llvh::errs() (real process stderr) and is never
  // given ours (CommandLine.cpp:366, 1422-1432; every call site in
  // ProvideOption and Option::addOccurrence uses that default). So
  // errText can be empty even though the parse genuinely failed.
  if (!llvh::cl::ParseCommandLineOptions(
          static_cast<int>(argv.size()), argv.data(), "", &errStream)) {
    errStream.flush();
    *error = errText.empty()
        ? "invalid value for one or more --vm options; see stderr for "
          "details"
        : errText;
    return false;
  }

  // The mapping mirrors hermes/tools/hermes/hermes.cpp:110-141, which is
  // what the hermes binary itself uses. Diff against that function when
  // Hermes moves; the with...() calls below are kept in that function's
  // order for exactly that purpose.
  //
  // THE INVARIANT: a with...() call happens IF AND ONLY IF the caller
  // actually named that flag (wasGiven()) -- never unconditionally, and
  // never gated by comparing the flag's current value against some
  // expected default. With no --vm= options at all, nothing below is
  // called, every field keeps the Builder's own compiled-in default, and
  // the result is *by construction* identical to the hardcoded
  // three-call RuntimeConfig::Builder() this function replaces -- not by
  // an audit of which of Hermes's ~25 cl::init(...) values happen to
  // match RuntimeConfig's/GCConfig's own defaults.
  //
  // That audit was tried first and failed three times before this
  // invariant replaced it: gc-init-heap/gc-max-heap (MemorySize fields
  // whose setDefault() is a silent no-op -- see the note below, this one
  // needed occurrence-gating regardless), gc-sanitize-handles/
  // -random-seed (ASAN builds default the rate to 0.01, GCSanitizeConfig's
  // own bare default is 0.0), and emit-async-break-check/verify-ir
  // (cl::init false/HERMES_SLOW_DEBUG-true, where RuntimeConfig's own
  // defaults are true/false -- the opposite mismatch, still a mismatch).
  // A field whose cl::init happens to already equal the builder's default
  // (ES6Proxy, Intl, MicrotaskQueue, MaxNumRegisters all initialize from
  // vm::RuntimeConfig::getDefault*() directly, RuntimeFlags.h) is safe
  // either way, but nothing here depends on knowing which fields are
  // currently safe: gating all of them is what survives Hermes changing
  // one cl::init and silently adding a fifth instance of the same bug.
  //
  // The three exceptions are hermes-node's own defaults -- ES6BlockScoping,
  // EnableAsyncGenerators and MicrotaskQueue, all forced to true, where
  // Hermes defaults the first two to false. Those three are applied
  // unconditionally and then overridden when the caller actually names
  // the flag, which is what "hermes-node's own default" means; see
  // kDefaultES6BlockScoping et al. above. Nothing else gets this
  // treatment.
  hermes::vm::GCConfig::Builder gcBuilder;
  // InitHeapSize and MaxHeapSize are cl::opt<MemorySize, ...>, and
  // MemorySize is a plain struct with no OptionValue<MemorySize>
  // specialization. It therefore falls through to llvh's generic
  // OptionValueBase<DataType, /*isClass=*/true> primary template, whose
  // hasValue() always returns false and whose setValue() is an empty
  // no-op (CommandLine.h:470-489); setDefaultImpl() only restores when
  // getDefault().hasValue() is true (CommandLine.h:1329-1333). So for
  // these two specifically, wasGiven() gating is not merely this
  // function's general invariant -- it is the *only* thing standing
  // between a value parsed by an earlier call and this call's output,
  // since the setDefault() sweep above cannot touch them. Every other
  // field below is protected by both the gating and that sweep.
  if (wasGiven(flags.InitHeapSize))
    gcBuilder.withInitHeapSize(flags.InitHeapSize.bytes);
  if (wasGiven(flags.MaxHeapSize))
    gcBuilder.withMaxHeapSize(flags.MaxHeapSize.bytes);
  if (wasGiven(flags.OccupancyTarget))
    gcBuilder.withOccupancyTarget(flags.OccupancyTarget);
  if (wasGiven(flags.GCSanitizeRate) || wasGiven(flags.GCSanitizeRandomSeed))
    gcBuilder.withSanitizeConfig(hermes::vm::GCSanitizeConfig::Builder()
                                     .withSanitizeRate(flags.GCSanitizeRate)
                                     .withRandomSeed(flags.GCSanitizeRandomSeed)
                                     .build());
  // ShouldReleaseUnused is deliberately NOT copied from hermes.cpp:93,
  // which sets kReleaseUnusedNone. There is no --vm= flag behind it --
  // no entry in kHonoured, no cl::opt to read -- and this mapping's job
  // is to honour flags the caller passed, not to import every constant
  // the hermes binary happens to pick. Setting it here would change
  // hermes-node's GC policy for every plain run with nothing on the
  // command line asking for it: GCConfig::Builder's own default is
  // kReleaseUnusedOld (GCConfig.h), which is what the hardcoded builder
  // this function replaced produced, since that builder never touched
  // GCConfig at all. Leaving it alone is what keeps "empty options are
  // identical to the hardcoded builder" true by construction rather than
  // true with an exception.
  if (wasGiven(flags.GCAllocYoung))
    gcBuilder.withAllocInYoung(flags.GCAllocYoung);
  if (wasGiven(flags.GCRevertToYGAtTTI))
    gcBuilder.withRevertToYGAtTTI(flags.GCRevertToYGAtTTI);
  auto gcConfig = gcBuilder.build();

  using JITMode = hermes::cli::VMOnlyRuntimeFlags::JITMode;
  hermes::vm::RuntimeConfig::Builder rtBuilder;
  // Not gated: this is the built GCConfig object above, not a --vm= flag
  // value, and always needs recording regardless of what --vm= named.
  rtBuilder.withGCConfig(gcConfig);
  if (wasGiven(flags.MaxNumRegisters))
    rtBuilder.withMaxNumRegisters(flags.MaxNumRegisters);
  // EnableJIT and ForceJIT are both derived from the one Xjit flag, so
  // they share its single occurrence check rather than each pretending to
  // be independently gated.
  if (wasGiven(flags.JIT)) {
    rtBuilder.withEnableJIT(flags.JIT != JITMode::Off);
    rtBuilder.withForceJIT(flags.JIT == JITMode::Force);
  }
  if (wasGiven(flags.JITThreshold))
    rtBuilder.withJITThreshold(flags.JITThreshold);
  if (wasGiven(flags.JITMemoryLimit))
    rtBuilder.withJITMemoryLimit(flags.JITMemoryLimit);
  if (wasGiven(flags.EnableEval))
    rtBuilder.withEnableEval(flags.EnableEval);
  if (wasGiven(flags.VerifyIR))
    rtBuilder.withVerifyEvalIR(flags.VerifyIR);
  if (wasGiven(flags.OptimizedEval))
    rtBuilder.withOptimizedEval(flags.OptimizedEval);
  if (wasGiven(flags.EmitAsyncBreakCheck))
    rtBuilder.withAsyncBreakCheckInEval(flags.EmitAsyncBreakCheck);
  if (wasGiven(flags.VMExperimentFlags))
    rtBuilder.withVMExperimentFlags(flags.VMExperimentFlags);
  if (wasGiven(flags.ES6Proxy))
    rtBuilder.withES6Proxy(flags.ES6Proxy);
  if (wasGiven(flags.Intl))
    rtBuilder.withIntl(flags.Intl);
  if (wasGiven(flags.RandomizeMemoryLayout))
    rtBuilder.withRandomizeMemoryLayout(flags.RandomizeMemoryLayout);
  if (wasGiven(flags.EnableHermesInternal))
    rtBuilder.withEnableHermesInternal(flags.EnableHermesInternal);
  if (wasGiven(flags.EnableHermesInternalTestMethods))
    rtBuilder.withEnableHermesInternalTestMethods(
        flags.EnableHermesInternalTestMethods);
  if (wasGiven(flags.Test262))
    rtBuilder.withTest262(flags.Test262);
  // The three fields where hermes-node's default differs from, or
  // deliberately restates, Hermes's: applied unconditionally, then
  // overridden when the caller actually names the flag. The only three
  // with...() calls in this function that are not behind a bare
  // wasGiven() check.
  rtBuilder.withES6BlockScoping(
      wasGiven(flags.ES6BlockScoping) ? (bool)flags.ES6BlockScoping
                                      : kDefaultES6BlockScoping);
  rtBuilder.withEnableAsyncGenerators(
      wasGiven(flags.EnableAsyncGenerators) ? (bool)flags.EnableAsyncGenerators
                                            : kDefaultAsyncGenerators);
  rtBuilder.withMicrotaskQueue(
      wasGiven(flags.MicrotaskQueue) ? (bool)flags.MicrotaskQueue
                                     : kDefaultMicrotaskQueue);
  *out = rtBuilder.build();
  return true;
}

std::string vmOptionsHelpText() {
  runtimeFlags();
  std::ostringstream os;
  auto &registry = llvh::cl::getRegisteredOptions();
  for (std::string_view name : honouredVmOptionNames()) {
    auto it = registry.find(llvh::StringRef(name.data(), name.size()));
    os << "  -" << name;
    if (it != registry.end() && !it->second->HelpStr.empty())
      os << "\n      " << it->second->HelpStr.str();
    os << "\n";
  }
  return os.str();
}

std::vector<std::string> splitVmOptionsEnv(const char *value) {
  std::vector<std::string> result;
  if (value == nullptr)
    return result;
  std::istringstream stream{std::string(value)};
  std::string word;
  while (stream >> word)
    result.push_back(std::move(word));
  return result;
}

} // namespace node_compat
} // namespace hermes
