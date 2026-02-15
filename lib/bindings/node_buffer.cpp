/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <hermes/node-compat/bindings/node_buffer.h>
#include <node_api.h>

#include <algorithm>
#include <climits>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace hermes {
namespace node_compat {

// ---------------------------------------------------------------------------
// Encoding enum (must match Node's enum encoding in node.h)
// ---------------------------------------------------------------------------

enum Encoding {
  ASCII = 0,
  UTF8 = 1,
  BASE64 = 2,
  UCS2 = 3,
  BINARY = 4,
  HEX = 5,
  BUFFER = 6,
  BASE64URL = 7,
  LATIN1 = BINARY,
};

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

// Maximum Buffer size. Node uses 2^31 - 1 on 64-bit.
static constexpr size_t kMaxLength =
    sizeof(void *) >= 8 ? (static_cast<size_t>(1) << 31) - 1 : (1u << 30) - 1;

// Maximum string length (matches V8's String::kMaxLength on 64-bit).
static constexpr int32_t kStringMaxLength =
    sizeof(void *) >= 8 ? (1 << 29) - 24 : (1 << 28) - 16;

// ---------------------------------------------------------------------------
// Helpers: extract typed array / arraybuffer data pointer and length
// ---------------------------------------------------------------------------

struct BufInfo {
  uint8_t *data;
  size_t length;
};

/// Get data pointer and byte length from a TypedArray argument.
static bool getBufInfo(napi_env env, napi_value val, BufInfo *out) {
  napi_typedarray_type type;
  size_t length;
  void *data;
  napi_value arrBuf;
  size_t byteOffset;
  napi_status st = napi_get_typedarray_info(
      env, val, &type, &length, &data, &arrBuf, &byteOffset);
  if (st == napi_ok) {
    out->data = static_cast<uint8_t *>(data);
    out->length = length;
    return true;
  }
  // Try DataView.
  size_t byteLength;
  st = napi_get_dataview_info(env, val, &byteLength, &data, nullptr, nullptr);
  if (st == napi_ok) {
    out->data = static_cast<uint8_t *>(data);
    out->length = byteLength;
    return true;
  }
  return false;
}

/// Get data from ArrayBuffer or SharedArrayBuffer.
static bool getArrayBufferInfo(napi_env env, napi_value val, BufInfo *out) {
  void *data;
  size_t length;
  napi_status st = napi_get_arraybuffer_info(env, val, &data, &length);
  if (st == napi_ok) {
    out->data = static_cast<uint8_t *>(data);
    out->length = length;
    return true;
  }
  return false;
}

/// Get buffer info from either TypedArray, DataView, ArrayBuffer.
static bool getAnyBufInfo(napi_env env, napi_value val, BufInfo *out) {
  if (getBufInfo(env, val, out))
    return true;
  return getArrayBufferInfo(env, val, out);
}

// ---------------------------------------------------------------------------
// Helpers: base64 encode/decode
// ---------------------------------------------------------------------------

static const char kBase64Table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static const char kBase64UrlTable[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

static std::string base64Encode(const uint8_t *data, size_t len) {
  std::string out;
  out.reserve(((len + 2) / 3) * 4);
  size_t i = 0;
  while (i + 2 < len) {
    auto a = data[i++];
    auto b = data[i++];
    auto c = data[i++];
    out.push_back(kBase64Table[a >> 2]);
    out.push_back(kBase64Table[((a & 0x03) << 4) | (b >> 4)]);
    out.push_back(kBase64Table[((b & 0x0f) << 2) | (c >> 6)]);
    out.push_back(kBase64Table[c & 0x3f]);
  }
  if (i < len) {
    auto a = data[i++];
    out.push_back(kBase64Table[a >> 2]);
    if (i < len) {
      auto b = data[i];
      out.push_back(kBase64Table[((a & 0x03) << 4) | (b >> 4)]);
      out.push_back(kBase64Table[(b & 0x0f) << 2]);
    } else {
      out.push_back(kBase64Table[(a & 0x03) << 4]);
      out.push_back('=');
    }
    out.push_back('=');
  }
  return out;
}

static std::string base64UrlEncode(const uint8_t *data, size_t len) {
  std::string out;
  out.reserve(((len + 2) / 3) * 4);
  size_t i = 0;
  while (i + 2 < len) {
    auto a = data[i++];
    auto b = data[i++];
    auto c = data[i++];
    out.push_back(kBase64UrlTable[a >> 2]);
    out.push_back(kBase64UrlTable[((a & 0x03) << 4) | (b >> 4)]);
    out.push_back(kBase64UrlTable[((b & 0x0f) << 2) | (c >> 6)]);
    out.push_back(kBase64UrlTable[c & 0x3f]);
  }
  if (i < len) {
    auto a = data[i++];
    out.push_back(kBase64UrlTable[a >> 2]);
    if (i < len) {
      auto b = data[i];
      out.push_back(kBase64UrlTable[((a & 0x03) << 4) | (b >> 4)]);
      out.push_back(kBase64UrlTable[(b & 0x0f) << 2]);
    } else {
      out.push_back(kBase64UrlTable[(a & 0x03) << 4]);
    }
  }
  return out;
}

/// Build a base64 decode table. Returns -1 for invalid chars, -2 for padding.
static const int8_t *base64DecodeTable() {
  static int8_t table[256];
  static bool initialized = false;
  if (!initialized) {
    std::memset(table, -1, sizeof(table));
    for (int i = 0; i < 64; i++)
      table[static_cast<uint8_t>(kBase64Table[i])] = static_cast<int8_t>(i);
    // URL-safe variants
    table[static_cast<uint8_t>('-')] = 62;
    table[static_cast<uint8_t>('_')] = 63;
    table[static_cast<uint8_t>('=')] = -2; // padding
    initialized = true;
  }
  return table;
}

/// Decode base64/base64url. Returns decoded bytes, or negative error code.
/// -1 = single char remained (input remainder)
/// -2 = invalid character
/// -3 = overflow
static int base64Decode(
    const char *input,
    size_t inputLen,
    std::vector<uint8_t> &out) {
  const int8_t *table = base64DecodeTable();
  out.clear();
  out.reserve((inputLen / 4) * 3);

  uint32_t accum = 0;
  int bits = 0;
  size_t padding = 0;

  for (size_t i = 0; i < inputLen; i++) {
    auto c = static_cast<uint8_t>(input[i]);
    // Skip whitespace
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f')
      continue;

    int8_t val = table[c];
    if (val == -2) {
      // padding
      padding++;
      continue;
    }
    if (val == -1) {
      return -2; // invalid character
    }
    if (padding > 0) {
      return -2; // data after padding
    }

    accum = (accum << 6) | static_cast<uint32_t>(val);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<uint8_t>((accum >> bits) & 0xFF));
    }
  }

  // Check for remainder (incomplete group)
  if (bits == 2 || bits == 4) {
    // 1 or 2 extra base64 digits without padding — this is valid for base64url
    // but "remainder" for strict base64. Node returns -1 for this case.
    // However, Node only returns -1 for atob with exactly 1 char remainder.
    // bits==2 means 1 extra char (6 bits, used 0), bits==4 means we already
    // output the byte above. Actually:
    // 1 char = 6 bits = 0 bytes (remainder of 1 char)
    // 2 chars = 12 bits = 1 byte + 4 remainder bits
    // 3 chars = 18 bits = 2 bytes + 2 remainder bits
    // With no padding:
    //   bits==6 after the loop: 1 char remainder -> error -1
    //   bits==4: consumed 2 chars (got 1 byte) or 3+1 chars, fine.
    //   bits==2: consumed 3 chars (got 2 bytes), fine.
    // Actually bits tracking: bits accumulates mod 8. Let me re-think.
    // After processing N valid base64 chars: total bits = N*6
    //   N%4==0: bits=0 (all consumed)
    //   N%4==1: bits=6 -> remainder. This is the -1 error case.
    //   N%4==2: bits=4 (12 bits, 1 byte emitted, 4 remainder) -> valid
    //   N%4==3: bits=2 (18 bits, 2 bytes emitted, 2 remainder) -> valid
  }

  // bits == 6 means exactly 1 char remains (N%4==1) — this is invalid
  if (bits == 6) {
    return -1;
  }

  return 0; // success
}

// ---------------------------------------------------------------------------
// Helpers: hex encode/decode
// ---------------------------------------------------------------------------

static const char kHexChars[] = "0123456789abcdef";

static std::string hexEncode(const uint8_t *data, size_t len) {
  std::string out(len * 2, '\0');
  for (size_t i = 0; i < len; i++) {
    out[i * 2] = kHexChars[data[i] >> 4];
    out[i * 2 + 1] = kHexChars[data[i] & 0x0f];
  }
  return out;
}

static int hexVal(char c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}

/// Decode hex string into output buffer. Returns number of bytes written.
static size_t hexDecode(const char *src, size_t srcLen, uint8_t *dst, size_t dstLen) {
  size_t written = 0;
  // Process pairs of hex digits
  for (size_t i = 0; i + 1 < srcLen && written < dstLen; i += 2) {
    int hi = hexVal(src[i]);
    int lo = hexVal(src[i + 1]);
    if (hi < 0 || lo < 0)
      break;
    dst[written++] = static_cast<uint8_t>((hi << 4) | lo);
  }
  return written;
}

// ---------------------------------------------------------------------------
// Helpers: UTF-8 validation
// ---------------------------------------------------------------------------

static bool isValidUtf8(const uint8_t *data, size_t len) {
  size_t i = 0;
  while (i < len) {
    uint8_t b = data[i];
    if (b < 0x80) {
      i++;
    } else if ((b & 0xE0) == 0xC0) {
      if (i + 1 >= len || (data[i + 1] & 0xC0) != 0x80)
        return false;
      if (b < 0xC2) // overlong
        return false;
      i += 2;
    } else if ((b & 0xF0) == 0xE0) {
      if (i + 2 >= len || (data[i + 1] & 0xC0) != 0x80 ||
          (data[i + 2] & 0xC0) != 0x80)
        return false;
      uint32_t cp = ((b & 0x0F) << 12) | ((data[i + 1] & 0x3F) << 6) |
          (data[i + 2] & 0x3F);
      if (cp < 0x0800 || (cp >= 0xD800 && cp <= 0xDFFF))
        return false;
      i += 3;
    } else if ((b & 0xF8) == 0xF0) {
      if (i + 3 >= len || (data[i + 1] & 0xC0) != 0x80 ||
          (data[i + 2] & 0xC0) != 0x80 || (data[i + 3] & 0xC0) != 0x80)
        return false;
      uint32_t cp = ((b & 0x07) << 18) | ((data[i + 1] & 0x3F) << 12) |
          ((data[i + 2] & 0x3F) << 6) | (data[i + 3] & 0x3F);
      if (cp < 0x10000 || cp > 0x10FFFF)
        return false;
      i += 4;
    } else {
      return false;
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
// Helpers: search (Boyer-Moore-Horspool inspired, simplified)
// ---------------------------------------------------------------------------

/// Forward memchr-based search for a single byte.
static const uint8_t *memchrForward(
    const uint8_t *haystack,
    size_t haystackLen,
    uint8_t needle,
    size_t offset) {
  if (offset >= haystackLen)
    return nullptr;
  return static_cast<const uint8_t *>(
      std::memchr(haystack + offset, needle, haystackLen - offset));
}

/// Reverse search for a single byte.
static const uint8_t *memchrReverse(
    const uint8_t *haystack,
    uint8_t needle,
    size_t endOffset) {
  // Search from endOffset down to 0.
  for (size_t i = endOffset + 1; i > 0; i--) {
    if (haystack[i - 1] == needle)
      return haystack + i - 1;
  }
  return nullptr;
}

/// Forward search for a multi-byte needle in haystack.
static int64_t searchForward(
    const uint8_t *haystack,
    size_t haystackLen,
    const uint8_t *needle,
    size_t needleLen,
    size_t offset) {
  if (needleLen == 0)
    return static_cast<int64_t>(offset);
  if (needleLen > haystackLen)
    return -1;
  if (offset + needleLen > haystackLen)
    return -1;

  for (size_t i = offset; i <= haystackLen - needleLen; i++) {
    if (std::memcmp(haystack + i, needle, needleLen) == 0)
      return static_cast<int64_t>(i);
  }
  return -1;
}

/// Reverse search for a multi-byte needle.
static int64_t searchReverse(
    const uint8_t *haystack,
    size_t haystackLen,
    const uint8_t *needle,
    size_t needleLen,
    size_t offset) {
  if (needleLen == 0)
    return static_cast<int64_t>(offset);
  if (needleLen > haystackLen)
    return -1;

  size_t start = std::min(offset, haystackLen - needleLen);
  for (size_t i = start + 1; i > 0; i--) {
    if (std::memcmp(haystack + i - 1, needle, needleLen) == 0)
      return static_cast<int64_t>(i - 1);
  }
  return -1;
}

/// Compute offset for indexOf/lastIndexOf (matches Node's IndexOfOffset).
static int64_t indexOfOffset(
    size_t length,
    int64_t offset,
    int64_t needleLen,
    bool isForward) {
  int64_t len = static_cast<int64_t>(length);
  if (offset < 0) {
    if (offset + len >= 0)
      return len + offset;
    else if (isForward || needleLen == 0)
      return 0;
    else
      return -1;
  } else {
    if (offset + needleLen <= len)
      return offset;
    else if (needleLen == 0)
      return len;
    else if (isForward)
      return -1;
    else
      return len - 1;
  }
}

// ---------------------------------------------------------------------------
// Helpers: create JS strings from buffer data in various encodings
// ---------------------------------------------------------------------------

/// Create UTF-8 string from raw bytes.
static napi_status makeUtf8String(
    napi_env env,
    const uint8_t *data,
    size_t length,
    napi_value *result) {
  return napi_create_string_utf8(
      env, reinterpret_cast<const char *>(data), length, result);
}

// ---------------------------------------------------------------------------
// Helpers: write JS string into buffer in various encodings
// ---------------------------------------------------------------------------

/// Get a JS string's content as Latin1 bytes.
static std::string getStringLatin1(napi_env env, napi_value str) {
  size_t len = 0;
  napi_get_value_string_latin1(env, str, nullptr, 0, &len);
  std::string buf(len, '\0');
  napi_get_value_string_latin1(env, str, &buf[0], len + 1, &len);
  buf.resize(len);
  return buf;
}

/// Get a JS string's content as UTF-8 bytes.
static std::string getStringUtf8(napi_env env, napi_value str) {
  size_t len = 0;
  napi_get_value_string_utf8(env, str, nullptr, 0, &len);
  std::string buf(len, '\0');
  napi_get_value_string_utf8(env, str, &buf[0], len + 1, &len);
  buf.resize(len);
  return buf;
}

/// Get a JS string's content as UTF-16 code units.
static std::u16string getStringUtf16(napi_env env, napi_value str) {
  size_t len = 0;
  napi_get_value_string_utf16(env, str, nullptr, 0, &len);
  std::u16string buf(len, u'\0');
  napi_get_value_string_utf16(env, str, &buf[0], len + 1, &len);
  buf.resize(len);
  return buf;
}

// ---------------------------------------------------------------------------
// Callbacks: byteLengthUtf8
// ---------------------------------------------------------------------------

static napi_value byteLengthUtf8Cb(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  // Get UTF-8 byte length of the string.
  size_t len = 0;
  napi_get_value_string_utf8(env, argv[0], nullptr, 0, &len);

  napi_value result;
  napi_create_double(env, static_cast<double>(len), &result);
  return result;
}

// ---------------------------------------------------------------------------
// Callbacks: compare
// ---------------------------------------------------------------------------

static int normalizeCompareVal(int val, size_t aLen, size_t bLen) {
  if (val != 0)
    return val > 0 ? 1 : -1;
  if (aLen > bLen)
    return 1;
  if (aLen < bLen)
    return -1;
  return 0;
}

static napi_value compareCb(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  BufInfo a, b;
  if (!getBufInfo(env, argv[0], &a) || !getBufInfo(env, argv[1], &b)) {
    napi_throw_error(env, nullptr, "argument must be a Buffer");
    return nullptr;
  }

  size_t cmpLen = std::min(a.length, b.length);
  int val = normalizeCompareVal(
      cmpLen > 0 ? std::memcmp(a.data, b.data, cmpLen) : 0,
      a.length,
      b.length);

  napi_value result;
  napi_create_int32(env, val, &result);
  return result;
}

// ---------------------------------------------------------------------------
// Callbacks: compareOffset
// ---------------------------------------------------------------------------

static napi_value compareOffsetCb(napi_env env, napi_callback_info info) {
  size_t argc = 6;
  napi_value argv[6];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  BufInfo source, target;
  if (!getBufInfo(env, argv[0], &source) || !getBufInfo(env, argv[1], &target)) {
    napi_throw_error(env, nullptr, "argument must be a Buffer");
    return nullptr;
  }

  uint32_t targetStart = 0, sourceStart = 0, targetEnd = 0, sourceEnd = 0;
  napi_get_value_uint32(env, argv[2], &targetStart);
  napi_get_value_uint32(env, argv[3], &sourceStart);
  napi_get_value_uint32(env, argv[4], &targetEnd);
  napi_get_value_uint32(env, argv[5], &sourceEnd);

  if (targetEnd == 0 && argc >= 5) {
    // If targetEnd was not provided or is 0, default to target.length.
    // Actually in Node the JS layer always provides all 6 args.
    // But let's be safe with defaults.
  }

  if (sourceStart > source.length) {
    napi_throw_range_error(
        env, nullptr, "The value of \"sourceStart\" is out of range.");
    return nullptr;
  }
  if (targetStart > target.length) {
    napi_throw_range_error(
        env, nullptr, "The value of \"targetStart\" is out of range.");
    return nullptr;
  }

  if (sourceEnd > source.length)
    sourceEnd = static_cast<uint32_t>(source.length);
  if (targetEnd > target.length)
    targetEnd = static_cast<uint32_t>(target.length);

  size_t sourceLen = sourceEnd > sourceStart ? sourceEnd - sourceStart : 0;
  size_t targetLen = targetEnd > targetStart ? targetEnd - targetStart : 0;
  size_t toCmp = std::min(std::min(sourceLen, targetLen),
                          source.length - sourceStart);

  int val = normalizeCompareVal(
      toCmp > 0
          ? std::memcmp(
                source.data + sourceStart, target.data + targetStart, toCmp)
          : 0,
      sourceLen,
      targetLen);

  napi_value result;
  napi_create_int32(env, val, &result);
  return result;
}

// ---------------------------------------------------------------------------
// Callbacks: copy
// ---------------------------------------------------------------------------

static napi_value copyCb(napi_env env, napi_callback_info info) {
  size_t argc = 5;
  napi_value argv[5];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  BufInfo source, target;
  if (!getBufInfo(env, argv[0], &source) || !getBufInfo(env, argv[1], &target)) {
    napi_throw_error(env, nullptr, "argument must be a Buffer");
    return nullptr;
  }

  uint32_t targetStart = 0, sourceStart = 0, toCopy = 0;
  napi_get_value_uint32(env, argv[2], &targetStart);
  napi_get_value_uint32(env, argv[3], &sourceStart);
  napi_get_value_uint32(env, argv[4], &toCopy);

  std::memmove(target.data + targetStart, source.data + sourceStart, toCopy);

  napi_value result;
  napi_create_uint32(env, toCopy, &result);
  return result;
}

// ---------------------------------------------------------------------------
// Callbacks: fill
// ---------------------------------------------------------------------------

static napi_value fillCb(napi_env env, napi_callback_info info) {
  size_t argc = 5;
  napi_value argv[5];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  BufInfo buf;
  if (!getBufInfo(env, argv[0], &buf)) {
    napi_throw_error(env, nullptr, "argument must be a Buffer");
    return nullptr;
  }

  uint32_t start = 0, end = 0;
  napi_get_value_uint32(env, argv[2], &start);
  napi_get_value_uint32(env, argv[3], &end);

  size_t fillLen = end - start;

  // OOB check — return -2 to signal JS should throw.
  if (start > end || fillLen + start > buf.length) {
    napi_value result;
    napi_create_int32(env, -2, &result);
    return result;
  }

  // Check if value is a buffer (TypedArray).
  {
    BufInfo fillBuf;
    if (getBufInfo(env, argv[1], &fillBuf)) {
      size_t strLen = fillBuf.length;
      if (strLen == 0) {
        napi_value result;
        napi_create_int32(env, -1, &result);
        return result;
      }
      std::memcpy(
          buf.data + start, fillBuf.data, std::min(strLen, fillLen));
      // Repeat pattern
      if (strLen < fillLen) {
        size_t inThere = strLen;
        uint8_t *ptr = buf.data + start + strLen;
        while (inThere < fillLen - inThere) {
          std::memcpy(ptr, buf.data + start, inThere);
          ptr += inThere;
          inThere *= 2;
        }
        if (inThere < fillLen) {
          std::memcpy(ptr, buf.data + start, fillLen - inThere);
        }
      }
      return nullptr; // undefined return
    }
  }

  // Check if value is a number.
  napi_valuetype valType;
  napi_typeof(env, argv[1], &valType);
  if (valType == napi_number) {
    uint32_t val;
    napi_get_value_uint32(env, argv[1], &val);
    std::memset(buf.data + start, val & 0xFF, fillLen);
    return nullptr;
  }

  // String fill.
  int32_t enc = UTF8;
  if (argc >= 5) {
    napi_get_value_int32(env, argv[4], &enc);
  }

  size_t strLen = 0;

  if (enc == UTF8) {
    std::string str = getStringUtf8(env, argv[1]);
    strLen = str.size();
    if (strLen == 0) {
      napi_value result;
      napi_create_int32(env, -1, &result);
      return result;
    }
    std::memcpy(buf.data + start, str.data(), std::min(strLen, fillLen));
  } else if (enc == UCS2) {
    std::u16string str = getStringUtf16(env, argv[1]);
    strLen = str.size() * sizeof(char16_t);
    if (strLen == 0) {
      napi_value result;
      napi_create_int32(env, -1, &result);
      return result;
    }
    std::memcpy(buf.data + start, str.data(), std::min(strLen, fillLen));
  } else if (enc == LATIN1 || enc == ASCII) {
    std::string str = getStringLatin1(env, argv[1]);
    strLen = str.size();
    if (strLen == 0) {
      napi_value result;
      napi_create_int32(env, -1, &result);
      return result;
    }
    std::memcpy(buf.data + start, str.data(), std::min(strLen, fillLen));
  } else if (enc == HEX) {
    std::string str = getStringLatin1(env, argv[1]);
    strLen = hexDecode(str.data(), str.size(), buf.data + start, fillLen);
    if (strLen == 0) {
      napi_value result;
      napi_create_int32(env, -1, &result);
      return result;
    }
  } else if (enc == BASE64 || enc == BASE64URL) {
    std::string str = getStringLatin1(env, argv[1]);
    std::vector<uint8_t> decoded;
    int err = base64Decode(str.data(), str.size(), decoded);
    if (err < 0 || decoded.empty()) {
      napi_value result;
      napi_create_int32(env, -1, &result);
      return result;
    }
    strLen = decoded.size();
    std::memcpy(buf.data + start, decoded.data(), std::min(strLen, fillLen));
  } else {
    // Unknown encoding, treat as UTF8.
    std::string str = getStringUtf8(env, argv[1]);
    strLen = str.size();
    if (strLen == 0) {
      napi_value result;
      napi_create_int32(env, -1, &result);
      return result;
    }
    std::memcpy(buf.data + start, str.data(), std::min(strLen, fillLen));
  }

  // Repeat the pattern to fill the rest.
  if (strLen < fillLen) {
    size_t inThere = strLen;
    uint8_t *ptr = buf.data + start + strLen;
    while (inThere < fillLen - inThere) {
      std::memcpy(ptr, buf.data + start, inThere);
      ptr += inThere;
      inThere *= 2;
    }
    if (inThere < fillLen) {
      std::memcpy(ptr, buf.data + start, fillLen - inThere);
    }
  }

  return nullptr; // undefined return
}

// ---------------------------------------------------------------------------
// Callbacks: indexOfBuffer
// ---------------------------------------------------------------------------

static napi_value indexOfBufferCb(napi_env env, napi_callback_info info) {
  size_t argc = 5;
  napi_value argv[5];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  BufInfo haystack, needle;
  if (!getBufInfo(env, argv[0], &haystack) ||
      !getBufInfo(env, argv[1], &needle)) {
    napi_throw_error(env, nullptr, "argument must be a Buffer");
    return nullptr;
  }

  double offsetD = 0;
  napi_get_value_double(env, argv[2], &offsetD);
  int64_t offsetI = static_cast<int64_t>(offsetD);

  int32_t enc = UTF8;
  napi_get_value_int32(env, argv[3], &enc);

  bool isForward = true;
  napi_get_value_bool(env, argv[4], &isForward);

  int64_t optOffset = indexOfOffset(
      haystack.length, offsetI, static_cast<int64_t>(needle.length), isForward);

  if (needle.length == 0) {
    napi_value result;
    napi_create_double(env, static_cast<double>(optOffset), &result);
    return result;
  }

  if (haystack.length == 0 || optOffset <= -1) {
    napi_value result;
    napi_create_int32(env, -1, &result);
    return result;
  }

  size_t offset = static_cast<size_t>(optOffset);
  if ((isForward && needle.length + offset > haystack.length) ||
      needle.length > haystack.length) {
    napi_value result;
    napi_create_int32(env, -1, &result);
    return result;
  }

  int64_t pos;
  if (enc == UCS2) {
    if (haystack.length < 2 || needle.length < 2) {
      napi_value result;
      napi_create_int32(env, -1, &result);
      return result;
    }
    // UCS2 search operates on uint16_t units.
    const auto *h16 = reinterpret_cast<const uint16_t *>(haystack.data);
    const auto *n16 = reinterpret_cast<const uint16_t *>(needle.data);
    size_t h16Len = haystack.length / 2;
    size_t n16Len = needle.length / 2;

    if (isForward) {
      pos = searchForward(
          reinterpret_cast<const uint8_t *>(h16),
          h16Len * 2,
          reinterpret_cast<const uint8_t *>(n16),
          n16Len * 2,
          (offset / 2) * 2);
    } else {
      pos = searchReverse(
          reinterpret_cast<const uint8_t *>(h16),
          h16Len * 2,
          reinterpret_cast<const uint8_t *>(n16),
          n16Len * 2,
          (offset / 2) * 2);
    }
    // Result is already in byte units from our search.
  } else {
    if (isForward) {
      pos = searchForward(
          haystack.data, haystack.length, needle.data, needle.length, offset);
    } else {
      pos = searchReverse(
          haystack.data, haystack.length, needle.data, needle.length, offset);
    }
  }

  napi_value result;
  napi_create_int32(env, static_cast<int32_t>(pos), &result);
  return result;
}

// ---------------------------------------------------------------------------
// Callbacks: indexOfNumber
// ---------------------------------------------------------------------------

static napi_value indexOfNumberCb(napi_env env, napi_callback_info info) {
  size_t argc = 4;
  napi_value argv[4];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  BufInfo buf;
  if (!getBufInfo(env, argv[0], &buf)) {
    napi_throw_error(env, nullptr, "argument must be a Buffer");
    return nullptr;
  }

  uint32_t needle = 0;
  napi_get_value_uint32(env, argv[1], &needle);

  double offsetD = 0;
  napi_get_value_double(env, argv[2], &offsetD);
  int64_t offsetI = static_cast<int64_t>(offsetD);

  bool isForward = true;
  napi_get_value_bool(env, argv[3], &isForward);

  int64_t optOffset =
      indexOfOffset(buf.length, offsetI, 1, isForward);

  if (optOffset <= -1 || buf.length == 0) {
    napi_value result;
    napi_create_int32(env, -1, &result);
    return result;
  }

  size_t offset = static_cast<size_t>(optOffset);
  int32_t pos = -1;

  if (isForward) {
    const uint8_t *found = memchrForward(
        buf.data, buf.length, static_cast<uint8_t>(needle & 0xFF), offset);
    if (found)
      pos = static_cast<int32_t>(found - buf.data);
  } else {
    const uint8_t *found =
        memchrReverse(buf.data, static_cast<uint8_t>(needle & 0xFF), offset);
    if (found)
      pos = static_cast<int32_t>(found - buf.data);
  }

  napi_value result;
  napi_create_int32(env, pos, &result);
  return result;
}

// ---------------------------------------------------------------------------
// Callbacks: indexOfString
// ---------------------------------------------------------------------------

static napi_value indexOfStringCb(napi_env env, napi_callback_info info) {
  size_t argc = 5;
  napi_value argv[5];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  BufInfo buf;
  if (!getBufInfo(env, argv[0], &buf)) {
    napi_throw_error(env, nullptr, "argument must be a Buffer");
    return nullptr;
  }

  double offsetD = 0;
  napi_get_value_double(env, argv[2], &offsetD);
  int64_t offsetI = static_cast<int64_t>(offsetD);

  int32_t enc = UTF8;
  napi_get_value_int32(env, argv[3], &enc);

  bool isForward = true;
  napi_get_value_bool(env, argv[4], &isForward);

  // Convert the search string to bytes in the specified encoding.
  std::string needleBytes;
  std::u16string needleU16;
  const uint8_t *needlePtr = nullptr;
  size_t needleLen = 0;

  if (enc == UCS2) {
    needleU16 = getStringUtf16(env, argv[1]);
    needlePtr = reinterpret_cast<const uint8_t *>(needleU16.data());
    needleLen = needleU16.size() * sizeof(char16_t);
  } else if (enc == UTF8) {
    needleBytes = getStringUtf8(env, argv[1]);
    needlePtr = reinterpret_cast<const uint8_t *>(needleBytes.data());
    needleLen = needleBytes.size();
  } else if (enc == LATIN1 || enc == ASCII) {
    needleBytes = getStringLatin1(env, argv[1]);
    needlePtr = reinterpret_cast<const uint8_t *>(needleBytes.data());
    needleLen = needleBytes.size();
  } else if (enc == HEX) {
    std::string hexStr = getStringLatin1(env, argv[1]);
    needleBytes.resize(hexStr.size() / 2);
    size_t decoded = hexDecode(
        hexStr.data(),
        hexStr.size(),
        reinterpret_cast<uint8_t *>(&needleBytes[0]),
        needleBytes.size());
    needleBytes.resize(decoded);
    needlePtr = reinterpret_cast<const uint8_t *>(needleBytes.data());
    needleLen = needleBytes.size();
  } else if (enc == BASE64 || enc == BASE64URL) {
    std::string b64Str = getStringLatin1(env, argv[1]);
    std::vector<uint8_t> decoded;
    base64Decode(b64Str.data(), b64Str.size(), decoded);
    needleBytes.assign(decoded.begin(), decoded.end());
    needlePtr = reinterpret_cast<const uint8_t *>(needleBytes.data());
    needleLen = needleBytes.size();
  } else {
    needleBytes = getStringUtf8(env, argv[1]);
    needlePtr = reinterpret_cast<const uint8_t *>(needleBytes.data());
    needleLen = needleBytes.size();
  }

  size_t haystackLen = buf.length;
  if (enc == UCS2)
    haystackLen &= ~static_cast<size_t>(1); // round down to even

  int64_t optOffset =
      indexOfOffset(haystackLen, offsetI, static_cast<int64_t>(needleLen), isForward);

  if (needleLen == 0) {
    napi_value result;
    napi_create_double(env, static_cast<double>(optOffset), &result);
    return result;
  }
  if (haystackLen == 0 || optOffset <= -1) {
    napi_value result;
    napi_create_int32(env, -1, &result);
    return result;
  }

  size_t offset = static_cast<size_t>(optOffset);
  if ((isForward && needleLen + offset > haystackLen) ||
      needleLen > haystackLen) {
    napi_value result;
    napi_create_int32(env, -1, &result);
    return result;
  }

  int64_t pos;
  if (isForward) {
    pos = searchForward(buf.data, haystackLen, needlePtr, needleLen, offset);
  } else {
    pos = searchReverse(buf.data, haystackLen, needlePtr, needleLen, offset);
  }

  napi_value result;
  napi_create_int32(env, static_cast<int32_t>(pos), &result);
  return result;
}

// ---------------------------------------------------------------------------
// Callbacks: swap16, swap32, swap64
// ---------------------------------------------------------------------------

static napi_value swap16Cb(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  BufInfo buf;
  if (!getBufInfo(env, argv[0], &buf)) {
    napi_throw_error(env, nullptr, "argument must be a Buffer");
    return nullptr;
  }
  if (buf.length % 2 != 0) {
    napi_throw_range_error(env, nullptr, "Buffer size must be a multiple of 16-bits");
    return nullptr;
  }

  for (size_t i = 0; i < buf.length; i += 2) {
    uint8_t tmp = buf.data[i];
    buf.data[i] = buf.data[i + 1];
    buf.data[i + 1] = tmp;
  }

  return argv[0];
}

static napi_value swap32Cb(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  BufInfo buf;
  if (!getBufInfo(env, argv[0], &buf)) {
    napi_throw_error(env, nullptr, "argument must be a Buffer");
    return nullptr;
  }
  if (buf.length % 4 != 0) {
    napi_throw_range_error(env, nullptr, "Buffer size must be a multiple of 32-bits");
    return nullptr;
  }

  for (size_t i = 0; i < buf.length; i += 4) {
    uint8_t t0 = buf.data[i];
    uint8_t t1 = buf.data[i + 1];
    buf.data[i] = buf.data[i + 3];
    buf.data[i + 1] = buf.data[i + 2];
    buf.data[i + 2] = t1;
    buf.data[i + 3] = t0;
  }

  return argv[0];
}

static napi_value swap64Cb(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  BufInfo buf;
  if (!getBufInfo(env, argv[0], &buf)) {
    napi_throw_error(env, nullptr, "argument must be a Buffer");
    return nullptr;
  }
  if (buf.length % 8 != 0) {
    napi_throw_range_error(env, nullptr, "Buffer size must be a multiple of 64-bits");
    return nullptr;
  }

  for (size_t i = 0; i < buf.length; i += 8) {
    uint8_t t0 = buf.data[i];
    uint8_t t1 = buf.data[i + 1];
    uint8_t t2 = buf.data[i + 2];
    uint8_t t3 = buf.data[i + 3];
    buf.data[i] = buf.data[i + 7];
    buf.data[i + 1] = buf.data[i + 6];
    buf.data[i + 2] = buf.data[i + 5];
    buf.data[i + 3] = buf.data[i + 4];
    buf.data[i + 4] = t3;
    buf.data[i + 5] = t2;
    buf.data[i + 6] = t1;
    buf.data[i + 7] = t0;
  }

  return argv[0];
}

// ---------------------------------------------------------------------------
// Callbacks: isUtf8, isAscii
// ---------------------------------------------------------------------------

static napi_value isUtf8Cb(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  BufInfo buf;
  if (!getAnyBufInfo(env, argv[0], &buf)) {
    napi_throw_error(env, nullptr, "argument must be a Buffer or ArrayBuffer");
    return nullptr;
  }

  bool valid = isValidUtf8(buf.data, buf.length);

  napi_value result;
  napi_get_boolean(env, valid, &result);
  return result;
}

static napi_value isAsciiCb(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  BufInfo buf;
  if (!getAnyBufInfo(env, argv[0], &buf)) {
    napi_throw_error(env, nullptr, "argument must be a Buffer or ArrayBuffer");
    return nullptr;
  }

  bool valid = true;
  for (size_t i = 0; i < buf.length; i++) {
    if (buf.data[i] > 127) {
      valid = false;
      break;
    }
  }

  napi_value result;
  napi_get_boolean(env, valid, &result);
  return result;
}

// ---------------------------------------------------------------------------
// Callbacks: atob, btoa
// ---------------------------------------------------------------------------

static napi_value atobCb(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  std::string input = getStringLatin1(env, argv[0]);
  std::vector<uint8_t> decoded;
  int err = base64Decode(input.data(), input.size(), decoded);

  if (err < 0) {
    napi_value result;
    napi_create_int32(env, err, &result);
    return result;
  }

  // Return as a latin1 string (each byte is a code point).
  napi_value result;
  napi_create_string_latin1(
      env,
      reinterpret_cast<const char *>(decoded.data()),
      decoded.size(),
      &result);
  return result;
}

static napi_value btoaCb(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  // Get input as latin1 bytes. If any char > 255, return -1.
  std::u16string input16 = getStringUtf16(env, argv[0]);
  for (char16_t ch : input16) {
    if (ch > 0xFF) {
      napi_value result;
      napi_create_int32(env, -1, &result);
      return result;
    }
  }

  // Convert to bytes.
  std::vector<uint8_t> bytes(input16.size());
  for (size_t i = 0; i < input16.size(); i++) {
    bytes[i] = static_cast<uint8_t>(input16[i]);
  }

  std::string encoded = base64Encode(bytes.data(), bytes.size());

  napi_value result;
  napi_create_string_latin1(env, encoded.data(), encoded.size(), &result);
  return result;
}

// ---------------------------------------------------------------------------
// Callbacks: setBufferPrototype (no-op in our implementation)
// ---------------------------------------------------------------------------

static napi_value setBufferPrototypeCb(napi_env env, napi_callback_info info) {
  // In Node, this stores the prototype on the Realm for native Buffer creation.
  // We don't create buffers from native code, so this is a no-op.
  return nullptr;
}

// ---------------------------------------------------------------------------
// Callbacks: copyArrayBuffer
// ---------------------------------------------------------------------------

static napi_value copyArrayBufferCb(napi_env env, napi_callback_info info) {
  size_t argc = 5;
  napi_value argv[5];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  void *dstData;
  size_t dstLen;
  napi_get_arraybuffer_info(env, argv[0], &dstData, &dstLen);

  uint32_t dstOffset = 0;
  napi_get_value_uint32(env, argv[1], &dstOffset);

  void *srcData;
  size_t srcLen;
  napi_get_arraybuffer_info(env, argv[2], &srcData, &srcLen);

  uint32_t srcOffset = 0;
  napi_get_value_uint32(env, argv[3], &srcOffset);

  uint32_t bytesToCopy = 0;
  napi_get_value_uint32(env, argv[4], &bytesToCopy);

  std::memcpy(
      static_cast<uint8_t *>(dstData) + dstOffset,
      static_cast<uint8_t *>(srcData) + srcOffset,
      bytesToCopy);

  return nullptr;
}

// ---------------------------------------------------------------------------
// Callbacks: createUnsafeArrayBuffer
// ---------------------------------------------------------------------------

static napi_value createUnsafeArrayBufferCb(
    napi_env env,
    napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  double sizeD = 0;
  napi_get_value_double(env, argv[0], &sizeD);
  size_t size = static_cast<size_t>(sizeD);

  // Note: NAPI doesn't provide uninitialized ArrayBuffer creation.
  // We create a zero-initialized one, which is safe but slightly slower.
  napi_value result;
  void *data;
  napi_create_arraybuffer(env, size, &data, &result);
  return result;
}

// ---------------------------------------------------------------------------
// Callbacks: String slice methods (asciiSlice, utf8Slice, etc.)
// These are called as methods on Buffer: this.xxxSlice(start, end)
// ---------------------------------------------------------------------------

/// Helper to get `this` buffer + start/end from slice method args.
struct SliceArgs {
  BufInfo buf;
  size_t start;
  size_t end;
};

static bool getSliceArgs(
    napi_env env,
    napi_callback_info info,
    SliceArgs *out) {
  size_t argc = 2;
  napi_value argv[2];
  napi_value thisVal;
  napi_get_cb_info(env, info, &argc, argv, &thisVal, nullptr);

  if (!getBufInfo(env, thisVal, &out->buf))
    return false;

  out->start = 0;
  out->end = out->buf.length;

  if (argc >= 1) {
    uint32_t v;
    napi_get_value_uint32(env, argv[0], &v);
    out->start = v;
  }
  if (argc >= 2) {
    uint32_t v;
    napi_get_value_uint32(env, argv[1], &v);
    out->end = v;
  }

  if (out->end < out->start)
    out->end = out->start;
  if (out->end > out->buf.length)
    out->end = out->buf.length;

  return true;
}

static napi_value asciiSliceCb(napi_env env, napi_callback_info info) {
  SliceArgs args;
  if (!getSliceArgs(env, info, &args))
    return nullptr;
  size_t len = args.end - args.start;
  if (len == 0) {
    napi_value result;
    napi_create_string_utf8(env, "", 0, &result);
    return result;
  }
  // ASCII: mask out high bit.
  std::string out(len, '\0');
  for (size_t i = 0; i < len; i++) {
    out[i] = static_cast<char>(args.buf.data[args.start + i] & 0x7F);
  }
  napi_value result;
  napi_create_string_latin1(env, out.data(), out.size(), &result);
  return result;
}

static napi_value latin1SliceCb(napi_env env, napi_callback_info info) {
  SliceArgs args;
  if (!getSliceArgs(env, info, &args))
    return nullptr;
  size_t len = args.end - args.start;
  if (len == 0) {
    napi_value result;
    napi_create_string_utf8(env, "", 0, &result);
    return result;
  }
  napi_value result;
  napi_create_string_latin1(
      env,
      reinterpret_cast<const char *>(args.buf.data + args.start),
      len,
      &result);
  return result;
}

static napi_value utf8SliceCb(napi_env env, napi_callback_info info) {
  SliceArgs args;
  if (!getSliceArgs(env, info, &args))
    return nullptr;
  size_t len = args.end - args.start;
  if (len == 0) {
    napi_value result;
    napi_create_string_utf8(env, "", 0, &result);
    return result;
  }
  napi_value result;
  makeUtf8String(env, args.buf.data + args.start, len, &result);
  return result;
}

static napi_value hexSliceCb(napi_env env, napi_callback_info info) {
  SliceArgs args;
  if (!getSliceArgs(env, info, &args))
    return nullptr;
  size_t len = args.end - args.start;
  if (len == 0) {
    napi_value result;
    napi_create_string_utf8(env, "", 0, &result);
    return result;
  }
  std::string hex = hexEncode(args.buf.data + args.start, len);
  napi_value result;
  napi_create_string_latin1(env, hex.data(), hex.size(), &result);
  return result;
}

static napi_value base64SliceCb(napi_env env, napi_callback_info info) {
  SliceArgs args;
  if (!getSliceArgs(env, info, &args))
    return nullptr;
  size_t len = args.end - args.start;
  if (len == 0) {
    napi_value result;
    napi_create_string_utf8(env, "", 0, &result);
    return result;
  }
  std::string b64 = base64Encode(args.buf.data + args.start, len);
  napi_value result;
  napi_create_string_latin1(env, b64.data(), b64.size(), &result);
  return result;
}

static napi_value base64urlSliceCb(napi_env env, napi_callback_info info) {
  SliceArgs args;
  if (!getSliceArgs(env, info, &args))
    return nullptr;
  size_t len = args.end - args.start;
  if (len == 0) {
    napi_value result;
    napi_create_string_utf8(env, "", 0, &result);
    return result;
  }
  std::string b64 = base64UrlEncode(args.buf.data + args.start, len);
  napi_value result;
  napi_create_string_latin1(env, b64.data(), b64.size(), &result);
  return result;
}

static napi_value ucs2SliceCb(napi_env env, napi_callback_info info) {
  SliceArgs args;
  if (!getSliceArgs(env, info, &args))
    return nullptr;
  size_t len = args.end - args.start;
  if (len == 0) {
    napi_value result;
    napi_create_string_utf8(env, "", 0, &result);
    return result;
  }
  // UCS2/UTF16LE: interpret as 16-bit little-endian code units.
  size_t charLen = len / 2;
  napi_value result;
  napi_create_string_utf16(
      env,
      reinterpret_cast<const char16_t *>(args.buf.data + args.start),
      charLen,
      &result);
  return result;
}

// ---------------------------------------------------------------------------
// Callbacks: String write methods
// The "Static" variants (asciiWriteStatic, latin1WriteStatic, utf8WriteStatic)
// take (buf, string, offset, length) as positional args.
// The non-static variants (base64Write, hexWrite, ucs2Write, base64urlWrite)
// use `this` as the buffer and take (string, offset, length).
// ---------------------------------------------------------------------------

/// Helper for "static" write methods: (buf, string, offset, maxLength)
struct WriteStaticArgs {
  BufInfo buf;
  napi_value str;
  size_t offset;
  size_t maxLength;
};

static bool getWriteStaticArgs(
    napi_env env,
    napi_callback_info info,
    WriteStaticArgs *out) {
  size_t argc = 4;
  napi_value argv[4];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  if (!getBufInfo(env, argv[0], &out->buf))
    return false;

  out->str = argv[1];

  out->offset = 0;
  if (argc >= 3) {
    uint32_t v;
    napi_get_value_uint32(env, argv[2], &v);
    out->offset = v;
  }

  out->maxLength = out->buf.length - out->offset;
  if (argc >= 4) {
    uint32_t v;
    napi_get_value_uint32(env, argv[3], &v);
    out->maxLength = v;
  }

  out->maxLength = std::min(out->maxLength, out->buf.length - out->offset);
  return true;
}

/// Helper for method-style write: this.xxxWrite(string, offset, length)
struct WriteMethodArgs {
  BufInfo buf;
  napi_value str;
  size_t offset;
  size_t maxLength;
};

static bool getWriteMethodArgs(
    napi_env env,
    napi_callback_info info,
    WriteMethodArgs *out) {
  size_t argc = 3;
  napi_value argv[3];
  napi_value thisVal;
  napi_get_cb_info(env, info, &argc, argv, &thisVal, nullptr);

  if (!getBufInfo(env, thisVal, &out->buf))
    return false;

  out->str = argv[0];

  out->offset = 0;
  if (argc >= 2) {
    uint32_t v;
    napi_get_value_uint32(env, argv[1], &v);
    out->offset = v;
  }

  if (out->offset > out->buf.length) {
    napi_throw_range_error(
        env, nullptr, "\"offset\" is outside of buffer bounds");
    return false;
  }

  out->maxLength = out->buf.length - out->offset;
  if (argc >= 3) {
    uint32_t v;
    napi_get_value_uint32(env, argv[2], &v);
    out->maxLength = v;
  }

  out->maxLength = std::min(out->maxLength, out->buf.length - out->offset);
  return true;
}

static napi_value asciiWriteStaticCb(napi_env env, napi_callback_info info) {
  WriteStaticArgs args;
  if (!getWriteStaticArgs(env, info, &args))
    return nullptr;

  if (args.maxLength == 0) {
    napi_value result;
    napi_create_uint32(env, 0, &result);
    return result;
  }

  std::string str = getStringLatin1(env, args.str);
  size_t written = std::min(str.size(), args.maxLength);
  std::memcpy(args.buf.data + args.offset, str.data(), written);

  napi_value result;
  napi_create_uint32(env, static_cast<uint32_t>(written), &result);
  return result;
}

static napi_value latin1WriteStaticCb(napi_env env, napi_callback_info info) {
  WriteStaticArgs args;
  if (!getWriteStaticArgs(env, info, &args))
    return nullptr;

  if (args.maxLength == 0) {
    napi_value result;
    napi_create_uint32(env, 0, &result);
    return result;
  }

  std::string str = getStringLatin1(env, args.str);
  size_t written = std::min(str.size(), args.maxLength);
  std::memcpy(args.buf.data + args.offset, str.data(), written);

  napi_value result;
  napi_create_uint32(env, static_cast<uint32_t>(written), &result);
  return result;
}

static napi_value utf8WriteStaticCb(napi_env env, napi_callback_info info) {
  WriteStaticArgs args;
  if (!getWriteStaticArgs(env, info, &args))
    return nullptr;

  if (args.maxLength == 0) {
    napi_value result;
    napi_create_uint32(env, 0, &result);
    return result;
  }

  // Get UTF-8 bytes of the string.
  std::string str = getStringUtf8(env, args.str);

  // We need to truncate at a valid UTF-8 boundary.
  size_t written = std::min(str.size(), args.maxLength);
  // Back off to a valid UTF-8 boundary.
  while (written > 0 && (str[written - 1] & 0x80)) {
    // Check if we're in the middle of a multi-byte sequence.
    size_t back = 0;
    for (size_t j = written - 1; j < written; j--) {
      auto b = static_cast<uint8_t>(str[j]);
      if ((b & 0xC0) != 0x80) {
        // This is a lead byte. Check if the full sequence fits.
        size_t seqLen = 1;
        if ((b & 0xE0) == 0xC0)
          seqLen = 2;
        else if ((b & 0xF0) == 0xE0)
          seqLen = 3;
        else if ((b & 0xF8) == 0xF0)
          seqLen = 4;
        if (j + seqLen <= args.maxLength) {
          written = j + seqLen;
        } else {
          written = j;
        }
        break;
      }
      back++;
      if (back >= 4) {
        written = written - back;
        break;
      }
    }
    break;
  }

  std::memcpy(args.buf.data + args.offset, str.data(), written);

  napi_value result;
  napi_create_uint32(env, static_cast<uint32_t>(written), &result);
  return result;
}

static napi_value hexWriteCb(napi_env env, napi_callback_info info) {
  WriteMethodArgs args;
  if (!getWriteMethodArgs(env, info, &args))
    return nullptr;

  if (args.maxLength == 0) {
    napi_value result;
    napi_create_uint32(env, 0, &result);
    return result;
  }

  std::string str = getStringLatin1(env, args.str);
  size_t written =
      hexDecode(str.data(), str.size(), args.buf.data + args.offset, args.maxLength);

  napi_value result;
  napi_create_uint32(env, static_cast<uint32_t>(written), &result);
  return result;
}

static napi_value base64WriteCb(napi_env env, napi_callback_info info) {
  WriteMethodArgs args;
  if (!getWriteMethodArgs(env, info, &args))
    return nullptr;

  if (args.maxLength == 0) {
    napi_value result;
    napi_create_uint32(env, 0, &result);
    return result;
  }

  std::string str = getStringLatin1(env, args.str);
  std::vector<uint8_t> decoded;
  base64Decode(str.data(), str.size(), decoded);

  size_t written = std::min(decoded.size(), args.maxLength);
  std::memcpy(args.buf.data + args.offset, decoded.data(), written);

  napi_value result;
  napi_create_uint32(env, static_cast<uint32_t>(written), &result);
  return result;
}

static napi_value base64urlWriteCb(napi_env env, napi_callback_info info) {
  WriteMethodArgs args;
  if (!getWriteMethodArgs(env, info, &args))
    return nullptr;

  if (args.maxLength == 0) {
    napi_value result;
    napi_create_uint32(env, 0, &result);
    return result;
  }

  std::string str = getStringLatin1(env, args.str);
  std::vector<uint8_t> decoded;
  base64Decode(str.data(), str.size(), decoded);

  size_t written = std::min(decoded.size(), args.maxLength);
  std::memcpy(args.buf.data + args.offset, decoded.data(), written);

  napi_value result;
  napi_create_uint32(env, static_cast<uint32_t>(written), &result);
  return result;
}

static napi_value ucs2WriteCb(napi_env env, napi_callback_info info) {
  WriteMethodArgs args;
  if (!getWriteMethodArgs(env, info, &args))
    return nullptr;

  if (args.maxLength == 0) {
    napi_value result;
    napi_create_uint32(env, 0, &result);
    return result;
  }

  std::u16string str = getStringUtf16(env, args.str);
  size_t byteLen = str.size() * sizeof(char16_t);
  size_t written = std::min(byteLen, args.maxLength);
  // Ensure we don't split a code unit.
  written &= ~static_cast<size_t>(1);
  std::memcpy(args.buf.data + args.offset, str.data(), written);

  napi_value result;
  napi_create_uint32(env, static_cast<uint32_t>(written), &result);
  return result;
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------

napi_value initBufferBinding(napi_env env, napi_value exports) {
  napi_value val;
  napi_value fn;

// Helper macros.
#define SET_FN(name, cb)                                                   \
  do {                                                                     \
    napi_create_function(env, name, NAPI_AUTO_LENGTH, cb, nullptr, &fn);   \
    napi_set_named_property(env, exports, name, fn);                       \
  } while (0)

  // Core operations.
  SET_FN("byteLengthUtf8", byteLengthUtf8Cb);
  SET_FN("compare", compareCb);
  SET_FN("compareOffset", compareOffsetCb);
  SET_FN("copy", copyCb);
  SET_FN("fill", fillCb);
  SET_FN("indexOfBuffer", indexOfBufferCb);
  SET_FN("indexOfNumber", indexOfNumberCb);
  SET_FN("indexOfString", indexOfStringCb);
  SET_FN("swap16", swap16Cb);
  SET_FN("swap32", swap32Cb);
  SET_FN("swap64", swap64Cb);
  SET_FN("isUtf8", isUtf8Cb);
  SET_FN("isAscii", isAsciiCb);
  SET_FN("atob", atobCb);
  SET_FN("btoa", btoaCb);

  // Buffer prototype setup (no-op for us).
  SET_FN("setBufferPrototype", setBufferPrototypeCb);

  // ArrayBuffer utilities.
  SET_FN("copyArrayBuffer", copyArrayBufferCb);
  SET_FN("createUnsafeArrayBuffer", createUnsafeArrayBufferCb);

  // String slice methods (called as methods on Buffer via this).
  SET_FN("asciiSlice", asciiSliceCb);
  SET_FN("base64Slice", base64SliceCb);
  SET_FN("base64urlSlice", base64urlSliceCb);
  SET_FN("latin1Slice", latin1SliceCb);
  SET_FN("hexSlice", hexSliceCb);
  SET_FN("ucs2Slice", ucs2SliceCb);
  SET_FN("utf8Slice", utf8SliceCb);

  // String write methods — "static" variants (buf, string, offset, length).
  SET_FN("asciiWriteStatic", asciiWriteStaticCb);
  SET_FN("latin1WriteStatic", latin1WriteStaticCb);
  SET_FN("utf8WriteStatic", utf8WriteStaticCb);

  // String write methods — method-style (this.xxxWrite(string, offset, len)).
  SET_FN("base64Write", base64WriteCb);
  SET_FN("base64urlWrite", base64urlWriteCb);
  SET_FN("hexWrite", hexWriteCb);
  SET_FN("ucs2Write", ucs2WriteCb);

  // Constants.
  napi_create_double(env, static_cast<double>(kMaxLength), &val);
  napi_set_named_property(env, exports, "kMaxLength", val);

  napi_create_int32(env, kStringMaxLength, &val);
  napi_set_named_property(env, exports, "kStringMaxLength", val);

#undef SET_FN

  return exports;
}

} // namespace node_compat
} // namespace hermes
