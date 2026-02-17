// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include <ares.h>
#include <gtest/gtest.h>

TEST(CAresIntegration, LibraryInitAndCleanup) {
  int status = ares_library_init(ARES_LIB_INIT_ALL);
  ASSERT_EQ(status, ARES_SUCCESS);
  ares_library_cleanup();
}

TEST(CAresIntegration, VersionIsPositive) {
  int version = 0;
  const char *str = ares_version(&version);
  EXPECT_GT(version, 0);
  ASSERT_NE(str, nullptr);
  EXPECT_GT(strlen(str), 0u);
}

TEST(CAresIntegration, ChannelInitAndDestroy) {
  int status = ares_library_init(ARES_LIB_INIT_ALL);
  ASSERT_EQ(status, ARES_SUCCESS);

  ares_channel_t *channel = nullptr;
  struct ares_options opts = {};
  status = ares_init_options(&channel, &opts, 0);
  ASSERT_EQ(status, ARES_SUCCESS);
  ASSERT_NE(channel, nullptr);

  ares_destroy(channel);
  ares_library_cleanup();
}
