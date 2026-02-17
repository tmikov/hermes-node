/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// Native 'url' binding backed by the Ada URL parser.
// Provides: parse, update, canParse, domainToASCII, domainToUnicode,
//           getOrigin, format, pathToFileURL, urlComponents shared array.
// Mirrors Node's src/node_url.cc interface so that internal/url.js works.

#include "hermes/node-compat/bindings/node_url.h"

#include <ada.h>
#include <node_api.h>

#include <cstring>
#include <string>
#include <string_view>

namespace hermes {
namespace node_compat {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static constexpr int kURLComponentsLength = 9;

// Update action enum matching internal/url.js updateActions.
enum UrlUpdateAction {
  kProtocol = 0,
  kHost = 1,
  kHostname = 2,
  kPort = 3,
  kUsername = 4,
  kPassword = 5,
  kPathname = 6,
  kSearch = 7,
  kHash = 8,
  kHref = 9,
};

// Per-env binding data stored via napi_wrap so all functions can access it.
struct UrlBindingData {
  napi_ref componentsRef; // reference to the Int32Array
  int32_t *componentsData; // raw pointer into the ArrayBuffer backing store
};

// Retrieve the UrlBindingData pointer from callback data.
static UrlBindingData *getBindingData(napi_env env, napi_callback_info info) {
  void *data;
  napi_get_cb_info(env, info, nullptr, nullptr, nullptr, &data);
  return static_cast<UrlBindingData *>(data);
}

// Extract a UTF-8 string from a JS value. Returns empty string if not a string.
static std::string getUtf8String(napi_env env, napi_value value) {
  napi_valuetype type;
  napi_typeof(env, value, &type);
  if (type != napi_string)
    return "";
  size_t len = 0;
  napi_get_value_string_utf8(env, value, nullptr, 0, &len);
  std::string result(len, '\0');
  napi_get_value_string_utf8(env, value, &result[0], len + 1, &len);
  return result;
}

// Create a JS string from a std::string_view.
static napi_value makeJsString(napi_env env, std::string_view str) {
  napi_value result;
  napi_create_string_utf8(env, str.data(), str.size(), &result);
  return result;
}

// Write URL components from an ada::url_aggregator into the shared Int32Array.
static void updateComponents(
    UrlBindingData *bd,
    const ada::url_aggregator &url) {
  auto c = url.get_components();
  bd->componentsData[0] = static_cast<int32_t>(c.protocol_end);
  bd->componentsData[1] = static_cast<int32_t>(c.username_end);
  bd->componentsData[2] = static_cast<int32_t>(c.host_start);
  bd->componentsData[3] = static_cast<int32_t>(c.host_end);
  bd->componentsData[4] = static_cast<int32_t>(c.port);
  bd->componentsData[5] = static_cast<int32_t>(c.pathname_start);
  bd->componentsData[6] = static_cast<int32_t>(c.search_start);
  bd->componentsData[7] = static_cast<int32_t>(c.hash_start);
  bd->componentsData[8] = static_cast<int32_t>(url.type);
}

// ---------------------------------------------------------------------------
// parse(input, base?, raiseException?)
// Returns href string on success, undefined on failure (unless raiseException).
// Side effect: populates urlComponents.
// ---------------------------------------------------------------------------
static napi_value urlParse(napi_env env, napi_callback_info info) {
  auto *bd = getBindingData(env, info);

  size_t argc = 3;
  napi_value argv[3];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  std::string input = getUtf8String(env, argv[0]);

  bool raiseException = false;
  if (argc > 2) {
    napi_get_value_bool(env, argv[2], &raiseException);
  }

  // Try parsing with base if provided.
  bool hasBase = false;
  std::string base;
  if (argc > 1) {
    napi_valuetype baseType;
    napi_typeof(env, argv[1], &baseType);
    if (baseType == napi_string) {
      base = getUtf8String(env, argv[1]);
      hasBase = true;
    }
  }

  ada::result<ada::url_aggregator> out;
  if (hasBase) {
    auto baseResult = ada::parse<ada::url_aggregator>(base);
    if (!baseResult) {
      if (raiseException) {
        napi_throw_error(env, "ERR_INVALID_URL", "Invalid URL");
      }
      return nullptr;
    }
    out = ada::parse<ada::url_aggregator>(input, &baseResult.value());
  } else {
    out = ada::parse<ada::url_aggregator>(input);
  }

  if (!out) {
    if (raiseException) {
      napi_throw_error(env, "ERR_INVALID_URL", "Invalid URL");
    }
    return nullptr;
  }

  updateComponents(bd, out.value());
  return makeJsString(env, out->get_href());
}

// ---------------------------------------------------------------------------
// update(href, action, newValue)
// Returns new href string on success, false on failure.
// Side effect: populates urlComponents.
// ---------------------------------------------------------------------------
static napi_value urlUpdate(napi_env env, napi_callback_info info) {
  auto *bd = getBindingData(env, info);

  size_t argc = 3;
  napi_value argv[3];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  std::string href = getUtf8String(env, argv[0]);
  int32_t action;
  napi_get_value_int32(env, argv[1], &action);
  std::string newValue = getUtf8String(env, argv[2]);

  auto out = ada::parse<ada::url_aggregator>(href);
  if (!out) {
    napi_value falseval;
    napi_get_boolean(env, false, &falseval);
    return falseval;
  }

  bool success = true;
  std::string_view nv(newValue);

  switch (static_cast<UrlUpdateAction>(action)) {
    case kProtocol:
      success = out->set_protocol(nv);
      break;
    case kHost:
      success = out->set_host(nv);
      break;
    case kHostname:
      success = out->set_hostname(nv);
      break;
    case kPort:
      success = out->set_port(nv);
      break;
    case kUsername:
      success = out->set_username(nv);
      break;
    case kPassword:
      success = out->set_password(nv);
      break;
    case kPathname:
      success = out->set_pathname(nv);
      break;
    case kSearch:
      out->set_search(nv);
      break;
    case kHash:
      out->set_hash(nv);
      break;
    case kHref:
      success = out->set_href(nv);
      break;
    default:
      napi_throw_error(env, nullptr, "Unsupported URL update action");
      return nullptr;
  }

  if (!success) {
    napi_value falseval;
    napi_get_boolean(env, false, &falseval);
    return falseval;
  }

  updateComponents(bd, out.value());
  return makeJsString(env, out->get_href());
}

// ---------------------------------------------------------------------------
// canParse(url, base?)
// Returns boolean.
// ---------------------------------------------------------------------------
static napi_value urlCanParse(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  std::string input = getUtf8String(env, argv[0]);
  bool canParse;

  if (argc > 1) {
    napi_valuetype baseType;
    napi_typeof(env, argv[1], &baseType);
    if (baseType == napi_string) {
      std::string base = getUtf8String(env, argv[1]);
      std::string_view baseView(base);
      canParse = ada::can_parse(input, &baseView);
    } else {
      canParse = ada::can_parse(input);
    }
  } else {
    canParse = ada::can_parse(input);
  }

  napi_value result;
  napi_get_boolean(env, canParse, &result);
  return result;
}

// ---------------------------------------------------------------------------
// domainToASCII(domain)
// Uses Ada's URL parser to normalize the domain.
// ---------------------------------------------------------------------------
static napi_value urlDomainToASCII(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  std::string domain = getUtf8String(env, argv[0]);
  if (domain.empty()) {
    return makeJsString(env, "");
  }

  // Parse a URL with a special scheme so set_hostname follows spec.
  auto out = ada::parse<ada::url>("ws://x");
  if (!out || !out->set_hostname(domain)) {
    return makeJsString(env, "");
  }

  return makeJsString(env, out->get_hostname());
}

// ---------------------------------------------------------------------------
// domainToUnicode(domain)
// ---------------------------------------------------------------------------
static napi_value urlDomainToUnicode(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  std::string domain = getUtf8String(env, argv[0]);
  if (domain.empty()) {
    return makeJsString(env, "");
  }

  // Parse a URL with a special scheme so set_hostname follows spec.
  auto out = ada::parse<ada::url>("ws://x");
  if (!out || !out->set_hostname(domain)) {
    return makeJsString(env, "");
  }

  std::string result = ada::idna::to_unicode(out->get_hostname());
  return makeJsString(env, result);
}

// ---------------------------------------------------------------------------
// getOrigin(href)
// Returns the origin string for a parsed URL.
// ---------------------------------------------------------------------------
static napi_value urlGetOrigin(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  std::string href = getUtf8String(env, argv[0]);
  auto out = ada::parse<ada::url_aggregator>(href);
  if (!out) {
    napi_throw_error(env, "ERR_INVALID_URL", "Invalid URL");
    return nullptr;
  }

  std::string origin = out->get_origin();
  return makeJsString(env, origin);
}

// ---------------------------------------------------------------------------
// format(href, hash, unicode, search, auth)
// Returns a formatted URL string with optional component removal.
// ---------------------------------------------------------------------------
static napi_value urlFormat(napi_env env, napi_callback_info info) {
  size_t argc = 5;
  napi_value argv[5];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  std::string href = getUtf8String(env, argv[0]);

  bool hash = false, unicode = false, search = false, auth = false;
  napi_get_value_bool(env, argv[1], &hash);
  napi_get_value_bool(env, argv[2], &unicode);
  napi_get_value_bool(env, argv[3], &search);
  napi_get_value_bool(env, argv[4], &auth);

  // Use ada::url (not url_aggregator) for direct component manipulation.
  auto out = ada::parse<ada::url>(href);
  if (!out) {
    napi_throw_error(env, "ERR_INVALID_URL", "Invalid URL");
    return nullptr;
  }

  if (!hash) {
    out->hash = std::nullopt;
  }

  if (unicode && out->has_hostname()) {
    out->host = ada::idna::to_unicode(out->get_hostname());
  }

  if (!search) {
    out->query = std::nullopt;
  }

  if (!auth) {
    out->username = "";
    out->password = "";
  }

  return makeJsString(env, out->get_href());
}

// ---------------------------------------------------------------------------
// pathToFileURL(input, isWindows, hostname?)
// Converts a file path to a file:// URL.
// Side effect: populates urlComponents.
// ---------------------------------------------------------------------------

// RFC1738 percent-encoding for path characters.
static std::string encodePathChars(std::string_view input, bool isWindows) {
  std::string encoded = "file://";
  encoded.reserve(input.size() + 7);
  for (unsigned char c : input) {
    if (isWindows && c == '\\') {
      encoded.push_back('/');
      continue;
    }
    // Encode unsafe characters per RFC1738.
    switch (c) {
      case '\0':
        encoded += "%00";
        break;
      case '\t':
        encoded += "%09";
        break;
      case '\n':
        encoded += "%0A";
        break;
      case '\r':
        encoded += "%0D";
        break;
      case ' ':
        encoded += "%20";
        break;
      case '"':
        encoded += "%22";
        break;
      case '#':
        encoded += "%23";
        break;
      case '%':
        encoded += "%25";
        break;
      case '?':
        encoded += "%3F";
        break;
      case '[':
        encoded += "%5B";
        break;
      case '\\':
        encoded += "%5C";
        break;
      case ']':
        encoded += "%5D";
        break;
      case '^':
        encoded += "%5E";
        break;
      case '|':
        encoded += "%7C";
        break;
      case '~':
        encoded += "%7E";
        break;
      default:
        encoded.push_back(static_cast<char>(c));
        break;
    }
  }
  return encoded;
}

static napi_value urlPathToFileURL(napi_env env, napi_callback_info info) {
  auto *bd = getBindingData(env, info);

  size_t argc = 3;
  napi_value argv[3];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  std::string input = getUtf8String(env, argv[0]);
  bool isWindows = false;
  napi_get_value_bool(env, argv[1], &isWindows);

  std::string fileUrl = encodePathChars(input, isWindows);
  auto out = ada::parse<ada::url_aggregator>(fileUrl);

  if (!out) {
    napi_throw_error(env, "ERR_INVALID_URL", "Invalid URL");
    return nullptr;
  }

  // Set hostname if provided (Windows UNC path).
  if (argc > 2) {
    napi_valuetype hostnameType;
    napi_typeof(env, argv[2], &hostnameType);
    if (hostnameType == napi_string) {
      std::string hostname = getUtf8String(env, argv[2]);
      out->set_hostname(hostname);
    }
  }

  updateComponents(bd, out.value());
  return makeJsString(env, out->get_href());
}

// ---------------------------------------------------------------------------
// initUrlBinding
// ---------------------------------------------------------------------------

static void bindingDataCleanup(napi_env env, void *data, void * /*hint*/) {
  auto *bd = static_cast<UrlBindingData *>(data);
  if (bd->componentsRef) {
    napi_delete_reference(env, bd->componentsRef);
  }
  delete bd;
}

napi_value initUrlBinding(napi_env env, napi_value exports) {
  // Create shared Int32Array(9) for URL components.
  auto *bd = new UrlBindingData();

  void *arrayBufData;
  napi_value arrayBuf;
  napi_create_arraybuffer(
      env, kURLComponentsLength * sizeof(int32_t), &arrayBufData, &arrayBuf);
  bd->componentsData = static_cast<int32_t *>(arrayBufData);
  memset(bd->componentsData, 0, kURLComponentsLength * sizeof(int32_t));

  napi_value urlComponents;
  napi_create_typedarray(
      env, napi_int32_array, kURLComponentsLength, arrayBuf, 0, &urlComponents);

  napi_set_named_property(env, exports, "urlComponents", urlComponents);

  // Keep a reference to prevent GC.
  napi_create_reference(env, urlComponents, 1, &bd->componentsRef);

  // Attach cleanup via napi_wrap on the exports object.
  napi_wrap(env, exports, bd, bindingDataCleanup, nullptr, nullptr);

  // Register functions with bd as callback data.
  napi_value fn;

#define REGISTER_FN(name, func)                                     \
  napi_create_function(env, name, NAPI_AUTO_LENGTH, func, bd, &fn); \
  napi_set_named_property(env, exports, name, fn)

  REGISTER_FN("parse", urlParse);
  REGISTER_FN("update", urlUpdate);
  REGISTER_FN("canParse", urlCanParse);
  REGISTER_FN("domainToASCII", urlDomainToASCII);
  REGISTER_FN("domainToUnicode", urlDomainToUnicode);
  REGISTER_FN("getOrigin", urlGetOrigin);
  REGISTER_FN("format", urlFormat);
  REGISTER_FN("pathToFileURL", urlPathToFileURL);

#undef REGISTER_FN

  return exports;
}

// ---------------------------------------------------------------------------
// url_pattern stub binding
// ---------------------------------------------------------------------------

napi_value initUrlPatternBinding(napi_env env, napi_value exports) {
  // URLPattern is not yet implemented. Export undefined so destructuring
  // { URLPattern } = internalBinding('url_pattern') yields undefined rather
  // than throwing.
  napi_value undef;
  napi_get_undefined(env, &undef);
  napi_set_named_property(env, exports, "URLPattern", undef);
  return exports;
}

} // namespace node_compat
} // namespace hermes
