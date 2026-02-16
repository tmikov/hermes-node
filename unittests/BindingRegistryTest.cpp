/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <hermes/node-compat/binding-registry/binding_registry.h>

#include "napi/hermes_napi.h"

#include "hermes/Public/RuntimeConfig.h"
#include "hermes/VM/Runtime.h"

#include <gtest/gtest.h>

#include <memory>

using namespace hermes::node_compat;

/// Test fixture that creates a Hermes Runtime and napi_env.
class BindingRegistryTest : public ::testing::Test {
 protected:
  std::shared_ptr<hermes::vm::Runtime> rt_;
  napi_env env_ = nullptr;
  napi_handle_scope scope_ = nullptr;

  void SetUp() override {
    auto config = hermes::vm::RuntimeConfig::Builder()
                      .withGCConfig(hermes::vm::GCConfig::Builder()
                                        .withInitHeapSize(1 << 16)
                                        .withMaxHeapSize(1 << 19)
                                        .build())
                      .build();
    rt_ = hermes::vm::Runtime::create(config);
    env_ = hermes_napi_create_env(rt_.get());
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

  /// Helper: run a JS expression and return its result as a napi_value.
  napi_value eval(const char *script) {
    napi_value scriptStr;
    EXPECT_EQ(
        napi_create_string_utf8(env_, script, NAPI_AUTO_LENGTH, &scriptStr),
        napi_ok);
    napi_value result;
    EXPECT_EQ(napi_run_script(env_, scriptStr, &result), napi_ok);
    return result;
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
// Dummy binding init functions for testing
//===========================================================================

static napi_value initDummy(napi_env env, napi_value exports) {
  napi_value val;
  napi_create_int32(env, 42, &val);
  napi_set_named_property(env, exports, "testValue", val);
  return exports;
}

static napi_value initAnother(napi_env env, napi_value exports) {
  napi_value val;
  napi_create_string_utf8(env, "hello", NAPI_AUTO_LENGTH, &val);
  napi_set_named_property(env, exports, "greeting", val);
  return exports;
}

static napi_value initThrowing(napi_env env, napi_value exports) {
  napi_throw_error(env, nullptr, "init failed on purpose");
  return nullptr;
}

//===========================================================================
// Tests
//===========================================================================

TEST_F(BindingRegistryTest, GetBindingReturnsObject) {
  BindingRegistry registry;
  registry.registerBinding("dummy", initDummy);
  registry.attach(env_);

  napi_value result;
  ASSERT_EQ(registry.getBinding(env_, "dummy", &result), napi_ok);

  // The binding object should have testValue === 42.
  napi_value testVal = getProp(result, "testValue");
  EXPECT_EQ(toInt32(testVal), 42);

  registry.detach(env_);
}

TEST_F(BindingRegistryTest, GetBindingCachesResult) {
  BindingRegistry registry;
  registry.registerBinding("dummy", initDummy);
  registry.attach(env_);

  napi_value first;
  ASSERT_EQ(registry.getBinding(env_, "dummy", &first), napi_ok);

  napi_value second;
  ASSERT_EQ(registry.getBinding(env_, "dummy", &second), napi_ok);

  // Both calls should return the same object.
  bool strictEqual;
  ASSERT_EQ(napi_strict_equals(env_, first, second, &strictEqual), napi_ok);
  EXPECT_TRUE(strictEqual);

  registry.detach(env_);
}

TEST_F(BindingRegistryTest, GetBindingUnknownNameThrows) {
  BindingRegistry registry;
  registry.attach(env_);

  napi_value result;
  napi_status status = registry.getBinding(env_, "nonexistent", &result);
  EXPECT_EQ(status, napi_pending_exception);

  // Clear the exception.
  EXPECT_TRUE(clearException());

  registry.detach(env_);
}

TEST_F(BindingRegistryTest, MultipleBindings) {
  BindingRegistry registry;
  registry.registerBinding("dummy", initDummy);
  registry.registerBinding("another", initAnother);
  registry.attach(env_);

  napi_value dummyResult;
  ASSERT_EQ(registry.getBinding(env_, "dummy", &dummyResult), napi_ok);
  EXPECT_EQ(toInt32(getProp(dummyResult, "testValue")), 42);

  napi_value anotherResult;
  ASSERT_EQ(registry.getBinding(env_, "another", &anotherResult), napi_ok);
  EXPECT_EQ(toString(getProp(anotherResult, "greeting")), "hello");

  registry.detach(env_);
}

TEST_F(BindingRegistryTest, InitFunctionThrows) {
  BindingRegistry registry;
  registry.registerBinding("throwing", initThrowing);
  registry.attach(env_);

  napi_value result;
  napi_status status = registry.getBinding(env_, "throwing", &result);
  EXPECT_EQ(status, napi_pending_exception);

  // Clear the exception.
  EXPECT_TRUE(clearException());

  registry.detach(env_);
}

TEST_F(BindingRegistryTest, CreateInternalBindingFunction) {
  BindingRegistry registry;
  registry.registerBinding("dummy", initDummy);
  registry.attach(env_);

  napi_value fn;
  ASSERT_EQ(registry.createInternalBindingFunction(env_, &fn), napi_ok);

  // Verify it's a function.
  napi_valuetype type;
  ASSERT_EQ(napi_typeof(env_, fn, &type), napi_ok);
  EXPECT_EQ(type, napi_function);

  // Set it as a global so we can call from JS.
  napi_value global;
  ASSERT_EQ(napi_get_global(env_, &global), napi_ok);
  ASSERT_EQ(
      napi_set_named_property(env_, global, "internalBinding", fn), napi_ok);

  // Call from JS.
  napi_value result = eval("internalBinding('dummy').testValue");
  EXPECT_EQ(toInt32(result), 42);

  registry.detach(env_);
}

TEST_F(BindingRegistryTest, InternalBindingFunctionThrowsOnUnknown) {
  BindingRegistry registry;
  registry.attach(env_);

  napi_value fn;
  ASSERT_EQ(registry.createInternalBindingFunction(env_, &fn), napi_ok);

  napi_value global;
  ASSERT_EQ(napi_get_global(env_, &global), napi_ok);
  ASSERT_EQ(
      napi_set_named_property(env_, global, "internalBinding", fn), napi_ok);

  // Calling with an unknown name should throw.
  napi_value script;
  ASSERT_EQ(
      napi_create_string_utf8(
          env_,
          "try { internalBinding('bogus'); 'no-throw'; } catch(e) { e.message; }",
          NAPI_AUTO_LENGTH,
          &script),
      napi_ok);

  napi_value result;
  ASSERT_EQ(napi_run_script(env_, script, &result), napi_ok);

  std::string msg = toString(result);
  EXPECT_NE(msg.find("No such binding"), std::string::npos)
      << "Expected error about missing binding, got: " << msg;

  registry.detach(env_);
}

TEST_F(BindingRegistryTest, DetachAndReattach) {
  BindingRegistry registry;
  registry.registerBinding("dummy", initDummy);
  registry.attach(env_);

  // Get the binding once to cache it.
  napi_value result;
  ASSERT_EQ(registry.getBinding(env_, "dummy", &result), napi_ok);
  EXPECT_EQ(toInt32(getProp(result, "testValue")), 42);

  // Detach clears cache.
  registry.detach(env_);

  // Reattach and get binding again — should reinitialize.
  registry.attach(env_);

  napi_value result2;
  ASSERT_EQ(registry.getBinding(env_, "dummy", &result2), napi_ok);
  EXPECT_EQ(toInt32(getProp(result2, "testValue")), 42);

  registry.detach(env_);
}
