/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <hermes/node-compat/process/node_process.h>

#include "hermes_napi.h"

#include "hermes/Public/RuntimeConfig.h"
#include "hermes/VM/Runtime.h"

#include <uv.h>

#include <gtest/gtest.h>

#include <unistd.h>

#include <cstdlib>
#include <memory>
#include <string>
#include <thread>

using namespace hermes::node_compat;

/// Test fixture that creates a Hermes Runtime, napi_env, and a NodeProcess.
class NodeProcessTest : public ::testing::Test {
 protected:
  std::shared_ptr<hermes::vm::Runtime> rt_;
  napi_env env_ = nullptr;
  napi_handle_scope scope_ = nullptr;
  NodeProcess proc_;
  napi_value processObj_ = nullptr;

  void SetUp() override {
    auto config = hermes::vm::RuntimeConfig::Builder()
                      .withGCConfig(hermes::vm::GCConfig::Builder()
                                        .withInitHeapSize(1 << 16)
                                        .withMaxHeapSize(1 << 20)
                                        .build())
                      .build();
    rt_ = hermes::vm::Runtime::create(config);
    env_ = hermes_napi_create_env(*rt_);
    ASSERT_EQ(napi_open_handle_scope(env_, &scope_), napi_ok);

    proc_.setArgv({"hermes-node", "test-script.js", "--flag"});
    proc_.setExecPath("/usr/local/bin/hermes-node");
    ASSERT_EQ(proc_.create(env_, &processObj_), napi_ok);

    // Set process as a global so JS can access it.
    napi_value global;
    ASSERT_EQ(napi_get_global(env_, &global), napi_ok);
    ASSERT_EQ(
        napi_set_named_property(env_, global, "process", processObj_),
        napi_ok);
  }

  void TearDown() override {
    proc_.detach(env_);
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
        napi_create_string_utf8(
            env_, script, NAPI_AUTO_LENGTH, &scriptStr),
        napi_ok);
    napi_value result;
    napi_status status = napi_run_script(env_, scriptStr, &result);
    if (status != napi_ok) {
      // Clear any pending exception so subsequent tests work.
      bool pending = false;
      napi_is_exception_pending(env_, &pending);
      if (pending) {
        napi_value exc;
        napi_get_and_clear_last_exception(env_, &exc);
        // Try to get the message for debugging.
        napi_value msg;
        napi_coerce_to_string(env_, exc, &msg);
        char buf[512];
        size_t len = 0;
        napi_get_value_string_utf8(env_, msg, buf, sizeof(buf), &len);
        ADD_FAILURE() << "JS exception: " << std::string(buf, len)
                      << "\n  script: " << script;
      } else {
        ADD_FAILURE() << "napi_run_script failed with status " << status
                      << "\n  script: " << script;
      }
      napi_value undef;
      napi_get_undefined(env_, &undef);
      return undef;
    }
    return result;
  }

  /// Helper: evaluate JS that returns a boolean.
  bool evalBool(const char *script) {
    napi_value result = eval(script);
    bool val = false;
    napi_get_value_bool(env_, result, &val);
    return val;
  }

  /// Helper: evaluate JS that returns a string.
  std::string evalString(const char *script) {
    napi_value result = eval(script);
    size_t len;
    napi_get_value_string_utf8(env_, result, nullptr, 0, &len);
    std::string str(len, '\0');
    napi_get_value_string_utf8(env_, result, &str[0], len + 1, &len);
    return str;
  }

  /// Helper: evaluate JS that returns a number (double).
  double evalDouble(const char *script) {
    napi_value result = eval(script);
    double val = 0;
    napi_get_value_double(env_, result, &val);
    return val;
  }

  /// Helper: evaluate JS that returns an int32.
  int32_t evalInt32(const char *script) {
    napi_value result = eval(script);
    int32_t val = 0;
    napi_get_value_int32(env_, result, &val);
    return val;
  }
};

// ===========================================================================
// Tests
// ===========================================================================

TEST_F(NodeProcessTest, PidIsPositive) {
  int32_t pid = evalInt32("process.pid");
  EXPECT_GT(pid, 0);
  EXPECT_EQ(pid, static_cast<int32_t>(getpid()));
}

TEST_F(NodeProcessTest, PpidIsPositive) {
  int32_t ppid = evalInt32("process.ppid");
  EXPECT_GT(ppid, 0);
  EXPECT_EQ(ppid, static_cast<int32_t>(getppid()));
}

TEST_F(NodeProcessTest, PlatformIsString) {
  std::string platform = evalString("process.platform");
  EXPECT_FALSE(platform.empty());
#if defined(__linux__)
  EXPECT_EQ(platform, "linux");
#elif defined(__APPLE__)
  EXPECT_EQ(platform, "darwin");
#endif
}

TEST_F(NodeProcessTest, ArchIsString) {
  std::string arch = evalString("process.arch");
  EXPECT_FALSE(arch.empty());
#if defined(__x86_64__)
  EXPECT_EQ(arch, "x64");
#elif defined(__aarch64__)
  EXPECT_EQ(arch, "arm64");
#endif
}

TEST_F(NodeProcessTest, VersionIsString) {
  std::string version = evalString("process.version");
  EXPECT_EQ(version, "v0.1.0-hermes");
}

TEST_F(NodeProcessTest, VersionsObject) {
  EXPECT_TRUE(evalBool("typeof process.versions === 'object'"));
  EXPECT_TRUE(evalBool("typeof process.versions.hermes === 'string'"));
  EXPECT_TRUE(evalBool("typeof process.versions.uv === 'string'"));
  EXPECT_TRUE(evalBool("typeof process.versions.node === 'string'"));
  EXPECT_EQ(evalString("process.versions.node"), "24.13.0");
}

TEST_F(NodeProcessTest, ArgvArray) {
  EXPECT_TRUE(evalBool("Array.isArray(process.argv)"));
  EXPECT_EQ(evalInt32("process.argv.length"), 3);
  EXPECT_EQ(evalString("process.argv[0]"), "hermes-node");
  EXPECT_EQ(evalString("process.argv[1]"), "test-script.js");
  EXPECT_EQ(evalString("process.argv[2]"), "--flag");
}

TEST_F(NodeProcessTest, ExecPathIsString) {
  std::string execPath = evalString("process.execPath");
  EXPECT_EQ(execPath, "/usr/local/bin/hermes-node");
}

TEST_F(NodeProcessTest, CwdReturnsString) {
  std::string cwd = evalString("process.cwd()");
  EXPECT_FALSE(cwd.empty());

  // Compare with libuv's result.
  char buf[4096];
  size_t size = sizeof(buf);
  ASSERT_EQ(uv_cwd(buf, &size), 0);
  EXPECT_EQ(cwd, std::string(buf, size));
}

TEST_F(NodeProcessTest, ChdirWorks) {
  std::string origCwd = evalString("process.cwd()");

  eval("process.chdir('/tmp')");
  std::string newCwd = evalString("process.cwd()");
  EXPECT_EQ(newCwd, "/tmp");

  // Restore.
  std::string script = "process.chdir('" + origCwd + "')";
  eval(script.c_str());
  EXPECT_EQ(evalString("process.cwd()"), origCwd);
}

TEST_F(NodeProcessTest, HrtimeReturnsArray) {
  EXPECT_TRUE(evalBool("Array.isArray(process.hrtime())"));
  EXPECT_EQ(evalInt32("process.hrtime().length"), 2);
  EXPECT_TRUE(
      evalBool("process.hrtime()[0] >= 0 && process.hrtime()[1] >= 0"));
}

TEST_F(NodeProcessTest, HrtimeDelta) {
  // The delta between two hrtime calls should be non-negative.
  EXPECT_TRUE(evalBool(
      "(function() {"
      "  var t1 = process.hrtime();"
      "  var t2 = process.hrtime(t1);"
      "  return t2[0] >= 0 && (t2[0] > 0 || t2[1] >= 0);"
      "})()"));
}

TEST_F(NodeProcessTest, HrtimeBigint) {
  EXPECT_TRUE(evalBool("typeof process.hrtime.bigint() === 'bigint'"));
  // Two successive calls should be increasing.
  EXPECT_TRUE(evalBool(
      "(function() {"
      "  var t1 = process.hrtime.bigint();"
      "  var t2 = process.hrtime.bigint();"
      "  return t2 >= t1;"
      "})()"));
}

TEST_F(NodeProcessTest, CpuUsageReturnsObject) {
  EXPECT_TRUE(evalBool("typeof process.cpuUsage() === 'object'"));
  EXPECT_TRUE(evalBool("typeof process.cpuUsage().user === 'number'"));
  EXPECT_TRUE(evalBool("typeof process.cpuUsage().system === 'number'"));
  EXPECT_TRUE(evalBool("process.cpuUsage().user >= 0"));
  EXPECT_TRUE(evalBool("process.cpuUsage().system >= 0"));
}

TEST_F(NodeProcessTest, CpuUsageDelta) {
  EXPECT_TRUE(evalBool(
      "(function() {"
      "  var prev = process.cpuUsage();"
      "  var delta = process.cpuUsage(prev);"
      "  return delta.user >= 0 && delta.system >= 0;"
      "})()"));
}

TEST_F(NodeProcessTest, MemoryUsageReturnsObject) {
  EXPECT_TRUE(evalBool("typeof process.memoryUsage() === 'object'"));
  EXPECT_TRUE(evalBool("process.memoryUsage().rss > 0"));
  EXPECT_TRUE(
      evalBool("typeof process.memoryUsage().heapTotal === 'number'"));
}

TEST_F(NodeProcessTest, UptimeReturnsNumber) {
  double uptime = evalDouble("process.uptime()");
  EXPECT_GE(uptime, 0.0);
  EXPECT_LT(uptime, 60.0); // Test shouldn't take a minute.
}

TEST_F(NodeProcessTest, UmaskQueryAndSet) {
  // Query current umask.
  EXPECT_TRUE(evalBool("typeof process.umask() === 'number'"));

  // Set and restore.
  EXPECT_TRUE(evalBool(
      "(function() {"
      "  var old = process.umask(0o077);"
      "  var cur = process.umask(old);"
      "  return cur === 0o077;"
      "})()"));
}

TEST_F(NodeProcessTest, EnvGetSetDelete) {
  // Set an env var.
  eval("process.env.HERMES_TEST_VAR = 'hello123'");
  EXPECT_EQ(evalString("process.env.HERMES_TEST_VAR"), "hello123");

  // Verify with getenv.
  const char *val = getenv("HERMES_TEST_VAR");
  ASSERT_NE(val, nullptr);
  EXPECT_STREQ(val, "hello123");

  // Delete it.
  eval("delete process.env.HERMES_TEST_VAR");
  EXPECT_TRUE(evalBool("process.env.HERMES_TEST_VAR === undefined"));
  EXPECT_EQ(getenv("HERMES_TEST_VAR"), nullptr);
}

TEST_F(NodeProcessTest, EnvHas) {
  setenv("HERMES_TEST_HAS", "yes", 1);
  EXPECT_TRUE(evalBool("'HERMES_TEST_HAS' in process.env"));
  EXPECT_FALSE(evalBool("'HERMES_NONEXISTENT_VAR_12345' in process.env"));
  unsetenv("HERMES_TEST_HAS");
}

TEST_F(NodeProcessTest, EnvOwnKeys) {
  setenv("HERMES_TEST_KEYS", "val", 1);
  EXPECT_TRUE(evalBool(
      "(function() {"
      "  var keys = Object.keys(process.env);"
      "  return Array.isArray(keys) && keys.length > 0;"
      "})()"));
  EXPECT_TRUE(evalBool(
      "Object.keys(process.env).indexOf('HERMES_TEST_KEYS') >= 0"));
  unsetenv("HERMES_TEST_KEYS");
}

TEST_F(NodeProcessTest, EnvGetOwnPropertyDescriptor) {
  setenv("HERMES_TEST_DESC", "descval", 1);
  EXPECT_TRUE(evalBool(
      "(function() {"
      "  var d = Object.getOwnPropertyDescriptor(process.env, 'HERMES_TEST_DESC');"
      "  return d && d.value === 'descval' && d.writable === true"
      "    && d.enumerable === true && d.configurable === true;"
      "})()"));
  unsetenv("HERMES_TEST_DESC");
}

TEST_F(NodeProcessTest, TitleGetterSetter) {
  EXPECT_TRUE(evalBool("typeof process.title === 'string'"));
  // We can't reliably test setting process title on all platforms,
  // but at least verify it doesn't throw.
  eval("process.title = 'hermes-test'");
  // The value may or may not round-trip depending on OS.
}

TEST_F(NodeProcessTest, KillSendSignalZero) {
  // signal 0 checks if process exists without sending a signal.
  EXPECT_TRUE(evalBool(
      "(function() {"
      "  try { process.kill(process.pid, 0); return true; }"
      "  catch(e) { return false; }"
      "})()"));
}
