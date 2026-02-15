// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include <gtest/gtest.h>
#include <uv.h>

TEST(UvIntegration, VersionIsPositive) {
  unsigned version = uv_version();
  EXPECT_GT(version, 0u);
}

TEST(UvIntegration, VersionStringNotEmpty) {
  const char *str = uv_version_string();
  ASSERT_NE(str, nullptr);
  EXPECT_GT(strlen(str), 0u);
}

TEST(UvIntegration, LoopInitAndClose) {
  uv_loop_t loop;
  int rc = uv_loop_init(&loop);
  ASSERT_EQ(rc, 0);
  rc = uv_loop_close(&loop);
  EXPECT_EQ(rc, 0);
}
