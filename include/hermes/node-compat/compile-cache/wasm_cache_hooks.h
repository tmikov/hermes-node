/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <hermes/node-compat/bundle/wasm_record.h>
#include <hermes/node-compat/compile-cache/compile_cache.h>

#include <node_api.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace hermes {
namespace node_compat {

/// One entry of the payload's native WebAssembly table (payloadAssembly()):
/// SHA-256 over a module's bytes, and the creator of the unit compiled from
/// them. 40 bytes, sorted by digest. The creator is opaque here, as it is in
/// HermesNodeConfig, so this header stays free of Hermes VM headers.
struct NativeWasmUnitEntry {
  uint8_t digest[kNativeDigestBytes];
  const void *creator;
};
static_assert(
    sizeof(NativeWasmUnitEntry) == 40,
    "payloadAssembly() emits 32 digest bytes then an 8-byte .quad");

/// The entry of \p table (\p count entries, sorted by digest) whose digest is
/// \p digest (kNativeDigestBytes raw bytes), or null.
const NativeWasmUnitEntry *findNativeWasmUnit(
    const NativeWasmUnitEntry *table,
    size_t count,
    const uint8_t *digest);

/// Everything the Wasm cache hooks can reach. Each member is optional and
/// they are independent: a run can have a disk cache and no recorder, a
/// recorder and no cache, a container and neither, or all three.
struct WasmCacheContext {
  /// The disk compile cache tier, or null when this run has none
  /// (--no-compile-cache, --inspect, no writable cache root).
  CompileCache *cache = nullptr;

  /// The --record-wasm writer, or null when nothing is being recorded.
  WasmRecordWriter *recorder = nullptr;

  /// HERMES_NODE_DEBUG_NATIVE=COMPILE_CACHE. Kept here rather than read from
  /// `cache` because the container tier traces even when there is no disk
  /// cache at all to ask.
  bool tracing = false;

  /// True when this run will open an AOT container, so a lookup should ask
  /// the container tier and report on it. Set by installWasmCacheHooks()
  /// from its \p bundleMode argument -- callers do not fill it in.
  ///
  /// It is a property of the configuration, not of the moment: the container
  /// is opened after the runtime exists, so a lookup cannot decide "is there
  /// a tier here?" by asking whether one happens to be open yet. What it
  /// separates is "the container did not bake this module" from "there is no
  /// container", which is the difference between tracing a miss and tracing
  /// nothing.
  bool container = false;

  /// Set when a record file could not be written, after the reason has been
  /// printed. Read by the runtime, which fails the run: a recording run
  /// whose file is wrong must not look successful.
  bool recordingFailed = false;

  /// The native WebAssembly table a build-native artifact links, or null.
  const NativeWasmUnitEntry *nativeWasmUnits = nullptr;
  size_t nativeWasmUnitCount = 0;

  /// Report on stderr, once per module, a module the native tier did not
  /// serve. The artifact's policy word, not a run-time choice.
  bool warnOnNativeFallback = false;

  /// Per raw sha256Digest() of a module: whether the native tier offered a
  /// creator for it. Read by the bytecode-tier lookup that follows a native
  /// miss, to tell "not in the table" from "offered but refused by Hermes".
  /// One runtime, one thread: no lock.
  std::unordered_map<std::string, bool> nativeAnswered;
  /// Modules already warned about.
  std::unordered_set<std::string> nativeWarned;
};

/// Install \p ctx as the WebAssembly bytecode cache for \p env, so that
/// Hermes consults it before compiling a module and reports the result
/// afterwards. \p bundleMode says whether this run will open a container
/// (--bundle=<f>, or a payload linked into this executable).
///
/// Installs when any tier could answer -- a cache, a recorder, a container,
/// or a native table or warning policy. Not when there is merely a cache: a
/// baked container has to stay reachable under --no-compile-cache, under a
/// missing $HOME/.cache and in a shipped executable whose user has the cache
/// disabled, which is precisely the deployment this feature exists for.
///
/// Nor unconditionally, which costs more than it looks: Hermes decides
/// whether a cache is usable from whether hooks are installed, not from what
/// a lookup returns, so an installed hook that can never answer still makes
/// every Wasm compile serialize its bytecode, hash the module and call
/// store().
///
/// Installing is also a no-op when Hermes itself was built without
/// WebAssembly (hermes_set_wasm_cache reports failure in that configuration,
/// which this function swallows -- the cache tiers are best effort).
///
/// \p ctx must outlive \p env.
void installWasmCacheHooks(
    napi_env env,
    WasmCacheContext *ctx,
    bool bundleMode);

} // namespace node_compat
} // namespace hermes
