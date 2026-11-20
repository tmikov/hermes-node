/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// uiTrace (lib/compositor-gpu/ui_trace.cpp) promises that a line reaches
// stderr as ONE write: three threads trace, and the end-to-end tests match
// "[ui] " adjacent to its body. These tests take the place of fd 2 and look
// at what arrives.
//
// Every test below names, in a comment, the mutation that must break it.

#include "ui_trace.h"

#include "gtest/gtest.h"

#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

using namespace hermes::node_compat;

namespace {

// uiTracingEnabled() reads HERMES_NODE_DEBUG_NATIVE once, on first use, so
// it has to be set before anything in this process traces. A namespace-scope
// initialiser runs before main() and therefore before every test; nothing in
// the library traces during static initialisation.
const int kTracingOn = ::setenv("HERMES_NODE_DEBUG_NATIVE", "UI", 1);

/// Points fd 2 at \p fd for its lifetime and restores it afterwards. gtest
/// reports on stdout, so a failure inside the scope is still printed.
class StderrRedirect {
 public:
  explicit StderrRedirect(int fd) : saved_(::dup(STDERR_FILENO)) {
    std::fflush(stderr);
    ::dup2(fd, STDERR_FILENO);
  }
  ~StderrRedirect() {
    std::fflush(stderr);
    ::dup2(saved_, STDERR_FILENO);
    ::close(saved_);
  }

 private:
  int saved_;
};

/// A temporary file, open read-write and already unlinked.
int openScratchFile() {
  std::FILE *file = std::tmpfile();
  if (file == nullptr)
    return -1;
  int fd = ::dup(::fileno(file));
  std::fclose(file);
  return fd;
}

std::string readWholeFile(int fd) {
  std::string out;
  ::lseek(fd, 0, SEEK_SET);
  char chunk[65536];
  ssize_t n;
  while ((n = ::read(fd, chunk, sizeof(chunk))) > 0)
    out.append(chunk, static_cast<size_t>(n));
  return out;
}

std::vector<std::string> splitLines(const std::string &text) {
  std::vector<std::string> lines;
  size_t start = 0;
  while (start < text.size()) {
    size_t end = text.find('\n', start);
    if (end == std::string::npos)
      end = text.size();
    lines.push_back(text.substr(start, end - start));
    start = end + 1;
  }
  return lines;
}

TEST(UiTraceTest, TracingIsEnabledForThisBinary) {
  // Everything below passes vacuously -- on an empty stream -- if the
  // environment variable did not take.
  ASSERT_EQ(kTracingOn, 0);
  ASSERT_TRUE(uiTracingEnabled());
}

// The decisive one, and deterministic: stderr is a datagram socket, so each
// write(2) arrives as one message with its boundaries intact.
// Mutation: emit the prefix, the body and the newline as three stdio calls.
// The first message is then "[ui] " alone.
TEST(UiTraceTest, OneLineIsOneWrite) {
  int fds[2];
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_DGRAM, 0, fds), 0);
  {
    StderrRedirect redirect(fds[1]);
    uiTrace("publish generation %d publication %d", 7, 42);
    uiTrace("second %s", "line");
    // Longer than the 128 bytes glibc formats an unbuffered stream in, which
    // a single fprintf would split, and still well under the smallest
    // datagram limit this runs under (2048 bytes on macOS).
    uiTrace("%s", std::string(300, 'x').c_str());
    uiStderrLine("[sokol-app:error] item %d", 3);
  }
  ::close(fds[1]);

  const std::string expected[] = {
      "[ui] publish generation 7 publication 42\n",
      "[ui] second line\n",
      "[ui] " + std::string(300, 'x') + "\n",
      "[sokol-app:error] item 3\n",
  };
  for (const std::string &want : expected) {
    char buf[1024];
    ssize_t n = ::recv(fds[0], buf, sizeof(buf), MSG_DONTWAIT);
    ASSERT_GT(n, 0) << "no write for: " << want;
    EXPECT_EQ(std::string(buf, static_cast<size_t>(n)), want);
  }
  char buf[16];
  EXPECT_LT(::recv(fds[0], buf, sizeof(buf), MSG_DONTWAIT), 0)
      << "more writes than lines";
  ::close(fds[0]);
}

// Mutation: format into the stack buffer alone and emit what fitted. The
// line then arrives cut short.
TEST(UiTraceTest, LineLongerThanTheStackBufferIsNotTruncated) {
  int fd = openScratchFile();
  ASSERT_GE(fd, 0);
  // Sizes around the 512-byte stack buffer, where the prefix, the newline
  // and vsnprintf's terminator each cost a byte, and one far beyond it.
  const size_t sizes[] = {0, 1, 504, 505, 506, 507, 508, 511, 512, 513, 20000};
  {
    StderrRedirect redirect(fd);
    for (size_t size : sizes)
      uiTrace("<%s>", std::string(size, 'y').c_str());
  }
  std::string want;
  for (size_t size : sizes)
    want += "[ui] <" + std::string(size, 'y') + ">\n";
  EXPECT_EQ(readWholeFile(fd), want);
  ::close(fd);
}

// What the one-write rule is for. Probabilistic where OneLineIsOneWrite is
// not: with the three-call mutation this fails only when two threads
// actually collide, which is most runs but not every one.
TEST(UiTraceTest, ConcurrentLinesDoNotInterleave) {
  constexpr int kThreads = 8;
  constexpr int kLines = 2000;
  int fd = openScratchFile();
  ASSERT_GE(fd, 0);
  {
    StderrRedirect redirect(fd);
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
      threads.emplace_back([t] {
        for (int i = 0; i < kLines; ++i)
          uiTrace("thread %d line %d", t, i);
      });
    }
    for (std::thread &thread : threads)
      thread.join();
  }
  std::vector<std::string> lines = splitLines(readWholeFile(fd));
  ::close(fd);

  ASSERT_EQ(lines.size(), static_cast<size_t>(kThreads) * kLines);
  // Every line whole, and each thread's lines in the order it wrote them.
  std::vector<int> next(kThreads, 0);
  size_t bad = 0;
  for (const std::string &line : lines) {
    int t = -1;
    int i = -1;
    int consumed = 0;
    if (std::sscanf(line.c_str(), "[ui] thread %d line %d%n", &t, &i, &consumed) !=
            2 ||
        static_cast<size_t>(consumed) != line.size() || t < 0 ||
        t >= kThreads || i != next[t]) {
      if (bad++ < 5)
        ADD_FAILURE() << "malformed or out-of-order line: \"" << line << "\"";
      continue;
    }
    ++next[t];
  }
  EXPECT_EQ(bad, 0u);
}

} // namespace
