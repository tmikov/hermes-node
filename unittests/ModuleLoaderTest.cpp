/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <hermes/node-compat/embedded-modules/embedded_modules.h>
#include <hermes/node-compat/module-loader/module_loader.h>

#include "napi/hermes_napi.h"

#include "hermes/Public/RuntimeConfig.h"
#include "hermes/hermes.h"

#include <gtest/gtest.h>

#include <memory>

using namespace hermes::node_compat;

/// Test fixture with a Hermes Runtime and napi_env.
///
/// Uses HermesRuntime (JSI) rather than vm::Runtime; see
/// hermes_node_runtime.cpp for the ODR rationale.
class ModuleLoaderTest : public ::testing::Test {
 protected:
  std::unique_ptr<facebook::hermes::HermesRuntime> rt_;
  napi_env env_ = nullptr;
  napi_handle_scope scope_ = nullptr;

  void SetUp() override {
    auto config = hermes::vm::RuntimeConfig::Builder()
                      .withGCConfig(hermes::vm::GCConfig::Builder()
                                        .withInitHeapSize(1 << 20)
                                        .withMaxHeapSize(1 << 24)
                                        .build())
                      .withES6BlockScoping(true)
                      .withEnableAsyncGenerators(true)
                      .build();
    rt_ = facebook::hermes::makeHermesRuntime(config);
    env_ = hermes_napi_create_env(rt_->getVMRuntimeUnsafe());
    ASSERT_EQ(napi_open_handle_scope(env_, &scope_), napi_ok);
  }

  void TearDown() override {
    if (scope_) {
      napi_close_handle_scope(env_, scope_);
      scope_ = nullptr;
    }
    if (env_) {
      hermes_napi_destroy_env(env_);
      env_ = nullptr;
    }
    rt_.reset();
  }

  /// Helper: check if exception is pending and clear it.
  bool clearException() {
    bool pending = false;
    napi_is_exception_pending(env_, &pending);
    if (pending) {
      napi_value exc;
      napi_get_and_clear_last_exception(env_, &exc);
    }
    return pending;
  }
};

//===========================================================================
// Tests
//===========================================================================

TEST_F(ModuleLoaderTest, FindEmbeddedModule) {
  // Verify that embedded modules can be found.
  EXPECT_NE(findEmbeddedModule("events"), nullptr);
  EXPECT_NE(findEmbeddedModule("internal/errors"), nullptr);
  EXPECT_NE(findEmbeddedModule("primordials"), nullptr);
  EXPECT_NE(findEmbeddedModule("loader"), nullptr);
  EXPECT_NE(findEmbeddedModule("path"), nullptr);
  EXPECT_NE(findEmbeddedModule("buffer"), nullptr);
  EXPECT_NE(findEmbeddedModule("internal/validators"), nullptr);
  EXPECT_EQ(findEmbeddedModule("nonexistent"), nullptr);
}

TEST_F(ModuleLoaderTest, BootstrapModuleFlag) {
  // Verify bootstrap flag is set correctly.
  const auto *primordials = findEmbeddedModule("primordials");
  ASSERT_NE(primordials, nullptr);
  EXPECT_TRUE(primordials->isBootstrap);

  const auto *loaderMod = findEmbeddedModule("loader");
  ASSERT_NE(loaderMod, nullptr);
  EXPECT_TRUE(loaderMod->isBootstrap);

  const auto *events = findEmbeddedModule("events");
  ASSERT_NE(events, nullptr);
  EXPECT_FALSE(events->isBootstrap);

  const auto *errors = findEmbeddedModule("internal/errors");
  ASSERT_NE(errors, nullptr);
  EXPECT_FALSE(errors->isBootstrap);
}

TEST_F(ModuleLoaderTest, EmbeddedModuleHasData) {
  // Verify that embedded modules have non-null data and non-zero size.
  const auto *mod = findEmbeddedModule("events");
  ASSERT_NE(mod, nullptr);
  EXPECT_NE(mod->data, nullptr);
  EXPECT_GT(mod->size, 0u);
}

TEST_F(ModuleLoaderTest, RunBootstrapModule) {
  // Verify that primordials can be executed from embedded bytecode.
  napi_value result;
  ASSERT_EQ(runEmbeddedModule(env_, "primordials", &result), napi_ok);

  // primordials should set globalThis.primordials.
  napi_value global;
  ASSERT_EQ(napi_get_global(env_, &global), napi_ok);
  napi_value primordials;
  ASSERT_EQ(
      napi_get_named_property(env_, global, "primordials", &primordials),
      napi_ok);
  napi_valuetype type;
  ASSERT_EQ(napi_typeof(env_, primordials, &type), napi_ok);
  EXPECT_EQ(type, napi_object);

  // Primordials should have ArrayIsArray.
  napi_value arrayIsArray;
  ASSERT_EQ(
      napi_get_named_property(env_, primordials, "ArrayIsArray", &arrayIsArray),
      napi_ok);
  ASSERT_EQ(napi_typeof(env_, arrayIsArray, &type), napi_ok);
  EXPECT_EQ(type, napi_function);
}

TEST_F(ModuleLoaderTest, RunLoaderModule) {
  // Verify that loader.js can be executed from embedded bytecode.
  // It should return a setup function.
  napi_value setupFn;
  ASSERT_EQ(runEmbeddedModule(env_, "loader", &setupFn), napi_ok);
  napi_valuetype type;
  ASSERT_EQ(napi_typeof(env_, setupFn, &type), napi_ok);
  EXPECT_EQ(type, napi_function);
}

TEST_F(ModuleLoaderTest, RunNonexistentModuleFails) {
  napi_value result;
  EXPECT_EQ(
      runEmbeddedModule(env_, "nonexistent", &result), napi_generic_failure);
}

TEST_F(ModuleLoaderTest, LoadBytecodeModuleCallback) {
  // Test the loadBytecodeModule callback directly.
  // Create the callback function.
  napi_value loadFn;
  ASSERT_EQ(
      napi_create_function(
          env_,
          "loadBytecodeModule",
          NAPI_AUTO_LENGTH,
          loadBytecodeModuleCallback,
          nullptr,
          &loadFn),
      napi_ok);

  // Call with a known embedded module ID -- should return a function.
  napi_value global;
  ASSERT_EQ(napi_get_global(env_, &global), napi_ok);

  napi_value idStr;
  ASSERT_EQ(
      napi_create_string_utf8(env_, "events", NAPI_AUTO_LENGTH, &idStr),
      napi_ok);
  napi_value result;
  ASSERT_EQ(
      napi_call_function(env_, global, loadFn, 1, &idStr, &result), napi_ok);
  napi_valuetype type;
  ASSERT_EQ(napi_typeof(env_, result, &type), napi_ok);
  EXPECT_EQ(type, napi_function);

  // Call with a nonexistent module -- should return undefined.
  napi_value badId;
  ASSERT_EQ(
      napi_create_string_utf8(env_, "nonexistent", NAPI_AUTO_LENGTH, &badId),
      napi_ok);
  napi_value result2;
  ASSERT_EQ(
      napi_call_function(env_, global, loadFn, 1, &badId, &result2), napi_ok);
  ASSERT_EQ(napi_typeof(env_, result2, &type), napi_ok);
  EXPECT_EQ(type, napi_undefined);
}
