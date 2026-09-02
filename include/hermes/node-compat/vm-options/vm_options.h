/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_NODE_COMPAT_VM_OPTIONS_VM_OPTIONS_H
#define HERMES_NODE_COMPAT_VM_OPTIONS_VM_OPTIONS_H

#include "hermes/Public/RuntimeConfig.h"

#include <string>
#include <string_view>
#include <vector>

namespace hermes {
namespace node_compat {

/// What hermes-node does with a Hermes VM flag named on --vm=.
enum class VmOptionStatus {
  /// Reaches vm::RuntimeConfig or vm::GCConfig. Accepted.
  kHonoured,
  /// A real Hermes flag, but one the hermes binary implements through
  /// ConsoleHost -- by calling methods on a live vm::Runtime, which this
  /// codebase deliberately never touches (see the comment above the
  /// hermes_napi_create_env() call in lib/runtime/hermes_node_runtime.cpp;
  /// named by that call rather than by line, since the line moves).
  /// Accepting it would parse cleanly and then do nothing, which is the
  /// silent no-op this project has spent several rounds removing. Refused
  /// by name.
  kConsoleHost,
  /// Not a flag we recognise. Includes llvh::cl's own -help and -version,
  /// which are deliberately not reachable: they print the entire option
  /// registry, LLVM's internal options included, and exit the process.
  kUnknown,
};

/// The bare flag name of \p arg: leading dashes and any "=value" removed.
/// "-gc-max-heap=1g" -> "gc-max-heap". Never throws; an argument with no
/// name at all yields an empty string, which classifies as kUnknown.
std::string_view vmOptionFlagName(std::string_view arg);

/// Classifies a bare flag name, as returned by vmOptionFlagName().
VmOptionStatus classifyVmOption(std::string_view flagName);

/// Every honoured flag name, in the order --vm-help prints them. Exactly
/// the names classifyVmOption() answers kHonoured for; VmOptionsTest pins
/// the two to each other.
std::vector<std::string_view> honouredVmOptionNames();

/// Every refused-by-name flag name -- exactly the names classifyVmOption()
/// answers kConsoleHost for. Exposed for the same reason the honoured list
/// is: VmOptionsTest resolves each of these against llvh::cl's registry, so
/// a Hermes rename or a typo here fails a test instead of degrading the
/// flag at run time. The refused list needs that check more than the
/// honoured one, not less: a name Hermes no longer registers would be
/// refused with a confident explanation of why hermes-node cannot support
/// a flag that no longer exists.
std::vector<std::string_view> refusedVmOptionNames();

/// Parses \p options -- each element one Hermes VM flag, exactly as spelled
/// on --vm= -- and writes the resulting configuration to \p out.
///
/// THE INVARIANT: a field of \p out is written if and only if the caller
/// actually named that flag -- llvh::cl::opt::getNumOccurrences() > 0,
/// never a comparison against the option's current or default value. So
/// with an empty \p options, nothing is written at all, and every field
/// keeps hermes::vm::RuntimeConfig::Builder's (or GCConfig::Builder's)
/// own compiled-in default -- which is *by construction* identical to
/// what the hardcoded three-call builder this function replaces produced,
/// since that builder never touched any of these fields either. Three
/// fields are the deliberate exception: ES6BlockScoping,
/// EnableAsyncGenerators and MicrotaskQueue are hermes-node's own
/// defaults (all `true`, where Hermes defaults the first two to `false`)
/// and are written unconditionally, then overridden when the caller
/// actually names the flag.
///
/// Why "named it" and not "differs from Hermes's default" is the test
/// everywhere, not just for those three: several honoured flags'
/// llvh::cl::init(...) do not match RuntimeConfig's/GCConfig's own
/// default at all (gc-sanitize-handles defaults to 0.01 in a
/// handle-sanitizer build where GCSanitizeConfig's bare default is 0.0;
/// emit-async-break-check defaults to `false` where RuntimeConfig's own
/// default is `true`; verify-ir defaults to `true` under
/// HERMES_SLOW_DEBUG where RuntimeConfig's own default is `false`) --
/// each of those was, in turn, a real bug when this function applied the
/// field unconditionally instead of gating it: a plain hermes-node run
/// silently stopped matching the process this function replaces. See the
/// comment at the mapping in vm_options.cpp for the full list and the
/// citations. Gating every field, not auditing which ones currently
/// happen to be safe, is what survives Hermes changing one cl::init
/// value without this function needing to change at all.
///
/// Every flag is classified before llvh::cl sees it, so a refused or
/// unknown flag produces our message rather than LLVM's, and -help never
/// reaches the help printer.
///
/// Safe to call more than once. llvh::cl's option registry is a process
/// global and ParseCommandLineOptions does not clear it, so this function
/// calls llvh::cl::ResetAllOptionOccurrences() -- load-bearing, since
/// getNumOccurrences() is what the invariant above reads -- and also
/// resets every registered option's *value* to its declared default
/// (Option::setDefault(), a no-op for a class-typed option value; see the
/// comment at the mapping in vm_options.cpp for the two options this
/// means the invariant above is the *only* protection for). That value
/// reset is not load-bearing for the invariant -- a value is read only
/// when its flag was named this call, and a real occurrence always comes
/// from a fresh parse -- but is kept as a second line of defense against
/// a future field being added to the mapping without the wasGiven()
/// guard. The unit tests parse several different option lists in one
/// process and are what this guarantee is for.
///
/// Returns false and sets \p error on any failure. Never exits the
/// process: the llvh::cl parse is given an error stream, which is what
/// makes it return rather than call exit().
bool buildVmRuntimeConfig(
    const std::vector<std::string> &options,
    hermes::vm::RuntimeConfig *out,
    std::string *error);

/// The --vm-help body: every honoured flag with the description Hermes
/// itself gives it, read out of the llvh::cl registry so the help cannot
/// drift from what is accepted.
std::string vmOptionsHelpText();

/// Splits a HERMES_NODE_VM_OPTIONS value on whitespace. An environment
/// variable has no other shape, so a value containing a space cannot be
/// expressed there; --vm= has no such limit and is the answer when one is
/// needed.
///
/// Empty vector for a null or all-whitespace value, which is what lets a
/// caller distinguish "not set" from "set to something".
///
/// Shared rather than duplicated: tools/hermes-node/hermes-node.cpp and
/// tools/hermes-node/bundle_main.cpp both split this variable, and two
/// copies of the rule in two translation units is how it would come to
/// differ between a --bundle run and an executable.
std::vector<std::string> splitVmOptionsEnv(const char *value);

} // namespace node_compat
} // namespace hermes

#endif
