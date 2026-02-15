/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <ada.h>
#include <hermes/node-compat/bindings/node_encoding.h>
#include <node_api.h>
#include <simdutf.h>

#include <cstring>
#include <string>

namespace hermes {
namespace node_compat {

// ---------------------------------------------------------------------------
// Helper: get buffer data from a value that may be TypedArray, ArrayBuffer,
// or SharedArrayBuffer.
// ---------------------------------------------------------------------------

struct BufInfo {
  const uint8_t *data;
  size_t length;
};

static bool getBufferData(napi_env env, napi_value val, BufInfo *out) {
  // Try TypedArray first (Uint8Array, etc.)
  {
    napi_typedarray_type type;
    size_t length;
    void *data;
    napi_value arrBuf;
    size_t byteOffset;
    napi_status st = napi_get_typedarray_info(
        env, val, &type, &length, &data, &arrBuf, &byteOffset);
    if (st == napi_ok) {
      // For TypedArray, length is element count. We need byte length.
      // Get it from the arraybuffer info + offset.
      void *abData;
      size_t abLen;
      napi_get_arraybuffer_info(env, arrBuf, &abData, &abLen);
      out->data = static_cast<const uint8_t *>(data);
      // The byte length of the view
      size_t elemSize = 1;
      switch (type) {
        case napi_uint16_array:
        case napi_int16_array:
          elemSize = 2;
          break;
        case napi_uint32_array:
        case napi_int32_array:
        case napi_float32_array:
          elemSize = 4;
          break;
        case napi_float64_array:
        case napi_bigint64_array:
        case napi_biguint64_array:
          elemSize = 8;
          break;
        default:
          elemSize = 1;
          break;
      }
      out->length = length * elemSize;
      return true;
    }
  }

  // Try DataView
  {
    void *data;
    size_t length;
    napi_value arrBuf;
    size_t byteOffset;
    napi_status st =
        napi_get_dataview_info(env, val, &length, &data, &arrBuf, &byteOffset);
    if (st == napi_ok) {
      out->data = static_cast<const uint8_t *>(data);
      out->length = length;
      return true;
    }
  }

  // Try ArrayBuffer
  {
    void *data;
    size_t length;
    napi_status st = napi_get_arraybuffer_info(env, val, &data, &length);
    if (st == napi_ok) {
      out->data = static_cast<const uint8_t *>(data);
      out->length = length;
      return true;
    }
  }

  return false;
}

// ---------------------------------------------------------------------------
// UTF-8 validation
// ---------------------------------------------------------------------------

/// Validate UTF-8 data. Returns true if valid.
static bool isValidUtf8(const uint8_t *data, size_t length) {
  return simdutf::validate_utf8(reinterpret_cast<const char *>(data), length);
}

// ---------------------------------------------------------------------------
// Latin-1 to UTF-8 conversion
// ---------------------------------------------------------------------------

static std::string latin1ToUtf8(const uint8_t *data, size_t length) {
  size_t utf8Len = simdutf::utf8_length_from_latin1(
      reinterpret_cast<const char *>(data), length);
  std::string result(utf8Len, '\0');
  (void)simdutf::convert_latin1_to_utf8(
      reinterpret_cast<const char *>(data), length, &result[0]);
  return result;
}

// ---------------------------------------------------------------------------
// Encode a JS string to UTF-8 bytes, writing char count and byte count
// to the encodeIntoResults Uint32Array.
// ---------------------------------------------------------------------------

// Count UTF-16 code units consumed from a JS string when encoding to UTF-8.
// This is needed for encodeInto to report how many characters were read.
static size_t countUtf16UnitsFromUtf8(const uint8_t *data, size_t length) {
  return simdutf::utf16_length_from_utf8(
      reinterpret_cast<const char *>(data), length);
}

// ---------------------------------------------------------------------------
// encodeUtf8String(input: string): Uint8Array
// ---------------------------------------------------------------------------

static napi_value encodeUtf8String(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  // Get string UTF-8 byte length.
  size_t strLen;
  napi_get_value_string_utf8(env, argv[0], nullptr, 0, &strLen);

  // Write into a temporary buffer (napi_get_value_string_utf8 appends '\0').
  std::string tmp(strLen + 1, '\0');
  size_t actualLen;
  napi_get_value_string_utf8(env, argv[0], &tmp[0], strLen + 1, &actualLen);

  // Allocate ArrayBuffer and copy.
  void *abData;
  napi_value arrayBuffer;
  napi_create_arraybuffer(env, actualLen, &abData, &arrayBuffer);
  memcpy(abData, tmp.data(), actualLen);

  // Create Uint8Array view.
  napi_value result;
  napi_create_typedarray(
      env, napi_uint8_array, actualLen, arrayBuffer, 0, &result);
  return result;
}

// ---------------------------------------------------------------------------
// decodeUTF8(buffer, ignoreBOM: bool, hasFatal: bool): string
// ---------------------------------------------------------------------------

static napi_value decodeUTF8(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  BufInfo buf;
  if (!getBufferData(env, argv[0], &buf)) {
    napi_throw_type_error(
        env,
        "ERR_INVALID_ARG_TYPE",
        "The \"list\" argument must be an instance of SharedArrayBuffer, "
        "ArrayBuffer or ArrayBufferView.");
    return nullptr;
  }

  bool ignoreBOM = false;
  bool hasFatal = false;
  if (argc > 1) {
    napi_get_value_bool(env, argv[1], &ignoreBOM);
  }
  if (argc > 2) {
    napi_get_value_bool(env, argv[2], &hasFatal);
  }

  const uint8_t *data = buf.data;
  size_t length = buf.length;

  // Fatal mode: validate UTF-8
  if (hasFatal) {
    if (!isValidUtf8(data, length)) {
      napi_throw_type_error(
          env,
          "ERR_ENCODING_INVALID_ENCODED_DATA",
          "The encoded data was not valid for encoding utf-8");
      return nullptr;
    }
  }

  // Skip BOM if present and not ignoring
  if (!ignoreBOM && length >= 3) {
    if (data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF) {
      data += 3;
      length -= 3;
    }
  }

  if (length == 0) {
    napi_value result;
    napi_create_string_utf8(env, "", 0, &result);
    return result;
  }

  napi_value result;
  napi_create_string_utf8(
      env, reinterpret_cast<const char *>(data), length, &result);
  return result;
}

// ---------------------------------------------------------------------------
// decodeLatin1(buffer, ignoreBOM: bool, hasFatal: bool): string
// ---------------------------------------------------------------------------

static napi_value decodeLatin1(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  BufInfo buf;
  if (!getBufferData(env, argv[0], &buf)) {
    napi_throw_type_error(
        env,
        "ERR_INVALID_ARG_TYPE",
        "The \"input\" argument must be an instance of ArrayBuffer, "
        "SharedArrayBuffer, or ArrayBufferView.");
    return nullptr;
  }

  bool ignoreBOM = false;
  bool hasFatal = false;
  if (argc > 1) {
    napi_get_value_bool(env, argv[1], &ignoreBOM);
  }
  if (argc > 2) {
    napi_get_value_bool(env, argv[2], &hasFatal);
  }

  const uint8_t *data = buf.data;
  size_t length = buf.length;

  // Note: Node checks ignoreBOM (not !ignoreBOM) for Latin-1 BOM skip.
  // Latin-1 BOM is 0xFF.
  if (ignoreBOM && length > 0 && data[0] == 0xFF) {
    data++;
    length--;
  }

  if (length == 0) {
    napi_value result;
    napi_create_string_utf8(env, "", 0, &result);
    return result;
  }

  // Convert Latin-1 to UTF-8
  std::string utf8 = latin1ToUtf8(data, length);

  napi_value result;
  napi_create_string_utf8(env, utf8.data(), utf8.size(), &result);
  return result;
}

// ---------------------------------------------------------------------------
// toASCII(domain: string): string
// IDNA ToASCII using Ada's IDNA implementation.
// ---------------------------------------------------------------------------

static napi_value toASCII(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  // Get domain string as UTF-8.
  size_t len = 0;
  napi_get_value_string_utf8(env, argv[0], nullptr, 0, &len);
  std::string domain(len, '\0');
  napi_get_value_string_utf8(env, argv[0], &domain[0], len + 1, &len);

  std::string ascii = ada::idna::to_ascii(domain);

  napi_value result;
  napi_create_string_utf8(env, ascii.data(), ascii.size(), &result);
  return result;
}

// ---------------------------------------------------------------------------
// toUnicode(domain: string): string
// IDNA ToUnicode using Ada's IDNA implementation.
// ---------------------------------------------------------------------------

static napi_value toUnicode(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  // Get domain string as UTF-8.
  size_t len = 0;
  napi_get_value_string_utf8(env, argv[0], nullptr, 0, &len);
  std::string domain(len, '\0');
  napi_get_value_string_utf8(env, argv[0], &domain[0], len + 1, &len);

  std::string unicode = ada::idna::to_unicode(domain);

  napi_value result;
  napi_create_string_utf8(env, unicode.data(), unicode.size(), &result);
  return result;
}

// ---------------------------------------------------------------------------
// initEncodingBinding
// ---------------------------------------------------------------------------

napi_value initEncodingBinding(napi_env env, napi_value exports) {
  // Create encodeIntoResults Uint32Array(2)
  void *resultsData;
  napi_value resultsArrBuf;
  napi_create_arraybuffer(
      env, 2 * sizeof(uint32_t), &resultsData, &resultsArrBuf);
  memset(resultsData, 0, 2 * sizeof(uint32_t));

  napi_value encodeIntoResults;
  napi_create_typedarray(
      env, napi_uint32_array, 2, resultsArrBuf, 0, &encodeIntoResults);

  // Set encodeIntoResults as property on exports
  napi_set_named_property(env, exports, "encodeIntoResults", encodeIntoResults);

  // Create a persistent reference to encodeIntoResults to pass as data
  // to encodeInto callback
  napi_ref resultsRef;
  napi_create_reference(env, encodeIntoResults, 1, &resultsRef);

  // Create encodeInto function with encodeIntoResults as callback data.
  // We pass the resultsRef so encodeInto can update it.
  // Actually, we need the napi_value, not the ref. Since the ref keeps it
  // alive, we can deref it in the callback. But simpler: just pass the
  // napi_value directly since it's also set on exports (which keeps it alive).
  // NAPI callback data is void*, so we store the ref and deref in callback.

  // Alternative simpler approach: store resultsRef as data, deref in callback.
  // But even simpler: we can pass the exports object as data, and look up
  // encodeIntoResults from it. However, the simplest approach is to use a
  // small struct or just update the property directly.

  // Simplest: create encodeInto as a closure that captures the Uint32Array.
  // We'll use the napi_ref as callback data.
  napi_value encodeIntoFnVal;
  napi_create_function(
      env,
      "encodeInto",
      NAPI_AUTO_LENGTH,
      [](napi_env env, napi_callback_info info) -> napi_value {
        size_t argc = 3;
        napi_value argv[3];
        void *cbData;
        napi_get_cb_info(env, info, &argc, argv, nullptr, &cbData);

        // Deref the results array
        napi_ref resultsRef = static_cast<napi_ref>(cbData);
        napi_value resultsArr;
        napi_get_reference_value(env, resultsRef, &resultsArr);

        // Get source string as UTF-8
        size_t srcUtf8Len;
        napi_get_value_string_utf8(env, argv[0], nullptr, 0, &srcUtf8Len);

        // Get dest buffer info
        size_t destLen;
        void *destData;
        napi_get_typedarray_info(
            env, argv[1], nullptr, &destLen, &destData, nullptr, nullptr);

        // Get the full UTF-8 string
        std::string fullUtf8(srcUtf8Len + 1, '\0');
        size_t actualLen;
        napi_get_value_string_utf8(
            env, argv[0], &fullUtf8[0], srcUtf8Len + 1, &actualLen);

        // Determine how many complete UTF-8 sequences fit in dest
        size_t written = 0;
        size_t i = 0;
        while (i < actualLen && written < destLen) {
          uint8_t b = static_cast<uint8_t>(fullUtf8[i]);
          size_t seqLen;
          if (b < 0x80) {
            seqLen = 1;
          } else if ((b & 0xE0) == 0xC0) {
            seqLen = 2;
          } else if ((b & 0xF0) == 0xE0) {
            seqLen = 3;
          } else {
            seqLen = 4;
          }
          if (written + seqLen > destLen)
            break;
          i += seqLen;
          written += seqLen;
        }

        // Copy truncated UTF-8 into dest
        memcpy(destData, fullUtf8.data(), written);

        // Count UTF-16 code units consumed
        size_t charsRead = countUtf16UnitsFromUtf8(
            reinterpret_cast<const uint8_t *>(fullUtf8.data()), written);

        // Update encodeIntoResults
        napi_value readVal, writtenVal;
        napi_create_uint32(env, static_cast<uint32_t>(charsRead), &readVal);
        napi_create_uint32(env, static_cast<uint32_t>(written), &writtenVal);
        napi_set_element(env, resultsArr, 0, readVal);
        napi_set_element(env, resultsArr, 1, writtenVal);

        return nullptr;
      },
      static_cast<void *>(resultsRef),
      &encodeIntoFnVal);
  napi_set_named_property(env, exports, "encodeInto", encodeIntoFnVal);

  // Set remaining functions
  napi_value fn;

  napi_create_function(
      env,
      "encodeUtf8String",
      NAPI_AUTO_LENGTH,
      encodeUtf8String,
      nullptr,
      &fn);
  napi_set_named_property(env, exports, "encodeUtf8String", fn);

  napi_create_function(
      env, "decodeUTF8", NAPI_AUTO_LENGTH, decodeUTF8, nullptr, &fn);
  napi_set_named_property(env, exports, "decodeUTF8", fn);

  napi_create_function(
      env, "decodeLatin1", NAPI_AUTO_LENGTH, decodeLatin1, nullptr, &fn);
  napi_set_named_property(env, exports, "decodeLatin1", fn);

  napi_create_function(env, "toASCII", NAPI_AUTO_LENGTH, toASCII, nullptr, &fn);
  napi_set_named_property(env, exports, "toASCII", fn);

  napi_create_function(
      env, "toUnicode", NAPI_AUTO_LENGTH, toUnicode, nullptr, &fn);
  napi_set_named_property(env, exports, "toUnicode", fn);

  return exports;
}

} // namespace node_compat
} // namespace hermes
