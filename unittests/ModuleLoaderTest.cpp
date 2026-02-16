/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <hermes/node-compat/binding-registry/binding_registry.h>
#include <hermes/node-compat/module-loader/module_loader.h>

#include "napi/hermes_napi.h"

#include "hermes/Public/RuntimeConfig.h"
#include "hermes/VM/Runtime.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>

using namespace hermes::node_compat;

// Paths set by CMake as compile definitions.
// LIBJS_PATH = path to libjs/ (our JS: loader.js, primordials.js)
// TEST_DATA_PATH = path to unittests/module-loader-testdata/

/// Dummy binding init function for testing.
static napi_value initDummy(napi_env env, napi_value exports) {
  napi_value val;
  napi_create_int32(env, 42, &val);
  napi_set_named_property(env, exports, "testValue", val);
  return exports;
}

/// Test fixture that creates a Hermes Runtime, napi_env, and sets up
/// primordials and internalBinding for module loading.
class ModuleLoaderTest : public ::testing::Test {
 protected:
  std::shared_ptr<hermes::vm::Runtime> rt_;
  napi_env env_ = nullptr;
  napi_handle_scope scope_ = nullptr;
  BindingRegistry registry_;
  ModuleLoader loader_;

  void SetUp() override {
    auto config = hermes::vm::RuntimeConfig::Builder()
                      .withGCConfig(hermes::vm::GCConfig::Builder()
                                        .withInitHeapSize(1 << 20)
                                        .withMaxHeapSize(1 << 24)
                                        .build())
                      .build();
    rt_ = hermes::vm::Runtime::create(config);
    env_ = hermes_napi_create_env(rt_.get());
    ASSERT_EQ(napi_open_handle_scope(env_, &scope_), napi_ok);

    // Register a dummy binding.
    registry_.registerBinding("dummy", initDummy);
    registry_.attach(env_);

    // Load and execute primordials.js to set globalThis.primordials.
    loadPrimordials();

    // Get primordials from global.
    napi_value global;
    ASSERT_EQ(napi_get_global(env_, &global), napi_ok);
    napi_value primordials;
    ASSERT_EQ(
        napi_get_named_property(env_, global, "primordials", &primordials),
        napi_ok);

    // Create internalBinding function.
    napi_value internalBindingFn;
    ASSERT_EQ(
        registry_.createInternalBindingFunction(env_, &internalBindingFn),
        napi_ok);

    // Initialize the module loader.
    loader_.setLibJsPath(LIBJS_PATH);
    loader_.setLibJsNodePath(TEST_DATA_PATH);
    ASSERT_EQ(loader_.init(env_, primordials, internalBindingFn), napi_ok);
  }

  void TearDown() override {
    loader_.detach(env_);
    registry_.detach(env_);
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

  /// Load and execute primordials.js.
  void loadPrimordials() {
    std::string path = std::string(LIBJS_PATH) + "primordials.js";
    std::string source = readTestFile(path);
    ASSERT_FALSE(source.empty()) << "Failed to read primordials.js at " << path;

    napi_value scriptStr;
    ASSERT_EQ(
        napi_create_string_utf8(
            env_, source.c_str(), source.size(), &scriptStr),
        napi_ok);
    napi_value result;
    napi_status status = napi_run_script(env_, scriptStr, &result);
    if (status != napi_ok) {
      // Print any pending exception.
      bool hasPending = false;
      napi_is_exception_pending(env_, &hasPending);
      if (hasPending) {
        napi_value exc;
        napi_get_and_clear_last_exception(env_, &exc);
        napi_value msg;
        napi_coerce_to_string(env_, exc, &msg);
        char buf[1024];
        size_t len;
        napi_get_value_string_utf8(env_, msg, buf, sizeof(buf), &len);
        FAIL() << "primordials.js execution failed: " << buf;
      }
      FAIL() << "primordials.js execution failed with status: " << status;
    }
  }

  /// Read a file into a string.
  static std::string readTestFile(const std::string &path) {
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f)
      return "";
    std::fseek(f, 0, SEEK_END);
    long size = std::ftell(f);
    std::rewind(f);
    std::string content(static_cast<size_t>(size), '\0');
    std::fread(&content[0], 1, static_cast<size_t>(size), f);
    std::fclose(f);
    return content;
  }

  /// Helper: get a named property from an object.
  napi_value getProp(napi_value obj, const char *name) {
    napi_value result;
    EXPECT_EQ(napi_get_named_property(env_, obj, name, &result), napi_ok);
    return result;
  }

  /// Helper: get an int32 from a napi_value.
  int32_t toInt32(napi_value val) {
    int32_t result;
    EXPECT_EQ(napi_get_value_int32(env_, val, &result), napi_ok);
    return result;
  }

  /// Helper: get a string from a napi_value.
  std::string toString(napi_value val) {
    size_t len;
    EXPECT_EQ(napi_get_value_string_utf8(env_, val, nullptr, 0, &len), napi_ok);
    std::string result(len, '\0');
    EXPECT_EQ(
        napi_get_value_string_utf8(env_, val, &result[0], len + 1, &len),
        napi_ok);
    return result;
  }

  /// Helper: get a bool from a napi_value.
  bool toBool(napi_value val) {
    bool result;
    EXPECT_EQ(napi_get_value_bool(env_, val, &result), napi_ok);
    return result;
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

TEST_F(ModuleLoaderTest, BasicRequire) {
  // Require test_module_b which exports { name: 'b', value: 42 }.
  napi_value exports;
  ASSERT_EQ(loader_.require(env_, "test_module_b", &exports), napi_ok);

  EXPECT_EQ(toString(getProp(exports, "name")), "b");
  EXPECT_EQ(toInt32(getProp(exports, "value")), 42);
}

TEST_F(ModuleLoaderTest, RequireCaches) {
  // Require the same module twice — should get the same object.
  napi_value first;
  ASSERT_EQ(loader_.require(env_, "test_module_b", &first), napi_ok);

  napi_value second;
  ASSERT_EQ(loader_.require(env_, "test_module_b", &second), napi_ok);

  bool strictEqual;
  ASSERT_EQ(napi_strict_equals(env_, first, second, &strictEqual), napi_ok);
  EXPECT_TRUE(strictEqual);
}

TEST_F(ModuleLoaderTest, TransitiveRequire) {
  // test_module_a requires test_module_b.
  napi_value exports;
  ASSERT_EQ(loader_.require(env_, "test_module_a", &exports), napi_ok);

  EXPECT_EQ(toString(getProp(exports, "name")), "a");

  // exports.b should be test_module_b's exports.
  napi_value bExports = getProp(exports, "b");
  EXPECT_EQ(toString(getProp(bExports, "name")), "b");
  EXPECT_EQ(toInt32(getProp(bExports, "value")), 42);
}

TEST_F(ModuleLoaderTest, CircularRequire) {
  // circ_a requires circ_b which requires circ_a (circular).
  // circ_a sets exports.name = 'circ_a' before requiring circ_b,
  // so circ_b sees circ_a's partial exports (with 'name' set).
  napi_value exports;
  ASSERT_EQ(loader_.require(env_, "circ_a", &exports), napi_ok);

  EXPECT_EQ(toString(getProp(exports, "name")), "circ_a");
  // circ_a gets circ_b's name via circB.name.
  EXPECT_EQ(toString(getProp(exports, "b_name")), "circ_b");

  // Also check circ_b's view: it should have a_name from circ_a's partial
  // exports (only 'name' was set at the time of circular require).
  napi_value bExports;
  ASSERT_EQ(loader_.require(env_, "circ_b", &bExports), napi_ok);
  EXPECT_EQ(toString(getProp(bExports, "a_name")), "circ_a");
}

TEST_F(ModuleLoaderTest, NestedRequire) {
  // uses_nested requires internal/nested.
  napi_value exports;
  ASSERT_EQ(loader_.require(env_, "uses_nested", &exports), napi_ok);

  EXPECT_EQ(toString(getProp(exports, "nestedName")), "nested");
  EXPECT_EQ(toString(getProp(exports, "nestedLocation")), "internal/nested");
}

TEST_F(ModuleLoaderTest, PrimordialsAccessible) {
  // uses_primordials accesses primordials.ArrayIsArray.
  napi_value exports;
  ASSERT_EQ(loader_.require(env_, "uses_primordials", &exports), napi_ok);

  EXPECT_TRUE(toBool(getProp(exports, "testResult")));
}

TEST_F(ModuleLoaderTest, InternalBindingAccessible) {
  // uses_binding accesses internalBinding('dummy').
  napi_value exports;
  ASSERT_EQ(loader_.require(env_, "uses_binding", &exports), napi_ok);

  EXPECT_EQ(toInt32(getProp(exports, "testValue")), 42);
}

TEST_F(ModuleLoaderTest, RequireNonexistentThrows) {
  napi_value exports;
  napi_status status = loader_.require(env_, "nonexistent_module", &exports);
  EXPECT_EQ(status, napi_pending_exception);
  EXPECT_TRUE(clearException());
}
