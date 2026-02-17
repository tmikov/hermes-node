// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include <gtest/gtest.h>
#include <llhttp.h>
#include <cstring>
#include <string>

namespace {

struct ParserState {
  std::string url;
  std::string headerField;
  std::string headerValue;
  bool messageComplete = false;
  int headersCompleteCount = 0;
};

int onUrl(llhttp_t *parser, const char *at, size_t length) {
  auto *state = static_cast<ParserState *>(parser->data);
  state->url.append(at, length);
  return 0;
}

int onHeaderField(llhttp_t *parser, const char *at, size_t length) {
  auto *state = static_cast<ParserState *>(parser->data);
  state->headerField.append(at, length);
  return 0;
}

int onHeaderValue(llhttp_t *parser, const char *at, size_t length) {
  auto *state = static_cast<ParserState *>(parser->data);
  state->headerValue.append(at, length);
  return 0;
}

int onHeadersComplete(llhttp_t *parser) {
  auto *state = static_cast<ParserState *>(parser->data);
  state->headersCompleteCount++;
  return 0;
}

int onMessageComplete(llhttp_t *parser) {
  auto *state = static_cast<ParserState *>(parser->data);
  state->messageComplete = true;
  return 0;
}

} // namespace

TEST(LlhttpIntegration, ParseSimpleGetRequest) {
  llhttp_settings_t settings;
  llhttp_settings_init(&settings);
  settings.on_url = onUrl;
  settings.on_header_field = onHeaderField;
  settings.on_header_value = onHeaderValue;
  settings.on_headers_complete = onHeadersComplete;
  settings.on_message_complete = onMessageComplete;

  llhttp_t parser;
  llhttp_init(&parser, HTTP_REQUEST, &settings);

  ParserState state;
  parser.data = &state;

  const char *request = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
  llhttp_errno_t err = llhttp_execute(&parser, request, std::strlen(request));

  ASSERT_EQ(err, HPE_OK);
  EXPECT_EQ(parser.method, HTTP_GET);
  EXPECT_EQ(state.url, "/");
  EXPECT_EQ(state.headerField, "Host");
  EXPECT_EQ(state.headerValue, "localhost");
  EXPECT_EQ(state.headersCompleteCount, 1);
  EXPECT_TRUE(state.messageComplete);
  EXPECT_EQ(parser.http_major, 1);
  EXPECT_EQ(parser.http_minor, 1);
}

TEST(LlhttpIntegration, ParseSimpleResponse) {
  llhttp_settings_t settings;
  llhttp_settings_init(&settings);
  settings.on_message_complete = onMessageComplete;

  llhttp_t parser;
  llhttp_init(&parser, HTTP_RESPONSE, &settings);

  ParserState state;
  parser.data = &state;

  const char *response = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n";
  llhttp_errno_t err = llhttp_execute(&parser, response, std::strlen(response));

  ASSERT_EQ(err, HPE_OK);
  EXPECT_EQ(parser.status_code, 200);
  EXPECT_TRUE(state.messageComplete);
}

TEST(LlhttpIntegration, VersionCheck) {
  EXPECT_EQ(LLHTTP_VERSION_MAJOR, 9);
  EXPECT_EQ(LLHTTP_VERSION_MINOR, 3);
  EXPECT_EQ(LLHTTP_VERSION_PATCH, 0);
}
