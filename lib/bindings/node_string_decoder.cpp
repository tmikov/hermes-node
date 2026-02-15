/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <hermes/node-compat/bindings/node_string_decoder.h>
#include <node_api.h>
#include <simdutf.h>

#include <algorithm>
#include <cstring>
#include <string>

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
// StringDecoder state layout (matches Node's StringDecoder struct)
// ---------------------------------------------------------------------------
//
// The decoder state is stored as raw bytes in a JS Buffer (Uint8Array).
// Layout (7 bytes total):
//   [0..3]  incomplete character buffer (up to 4 bytes)
//   [4]     missing bytes count
//   [5]     buffered bytes count
//   [6]     encoding field (Encoding enum value)

enum Fields {
  kIncompleteCharactersStart = 0,
  kIncompleteCharactersEnd = 4,
  kMissingBytes = 4,
  kBufferedBytes = 5,
  kEncodingField = 6,
  kNumFields = 7,
};

static const size_t kDecoderSize = kNumFields;

// ---------------------------------------------------------------------------
// Helpers — create JS string from raw bytes in a given encoding
// ---------------------------------------------------------------------------

/// Convert bytes to a hex string.
static napi_status makeHexString(
    napi_env env,
    const char *data,
    size_t length,
    napi_value *result) {
  static const char hex[] = "0123456789abcdef";
  std::string out(length * 2, '\0');
  for (size_t i = 0; i < length; ++i) {
    auto c = static_cast<unsigned char>(data[i]);
    out[i * 2] = hex[c >> 4];
    out[i * 2 + 1] = hex[c & 0x0f];
  }
  return napi_create_string_latin1(env, out.data(), out.size(), result);
}

/// Base64 encode bytes using simdutf.
static napi_status makeBase64String(
    napi_env env,
    const char *data,
    size_t length,
    napi_value *result,
    bool urlSafe) {
  auto opts = urlSafe ? simdutf::base64_url : simdutf::base64_default;
  size_t outLen = simdutf::base64_length_from_binary(length, opts);
  std::string out(outLen, '\0');
  simdutf::binary_to_base64(data, length, &out[0], opts);
  return napi_create_string_latin1(env, out.data(), out.size(), result);
}

/// Create a JS string from raw bytes in the given encoding.
static napi_status makeString(
    napi_env env,
    const char *data,
    size_t length,
    Encoding encoding,
    napi_value *result) {
  if (length == 0) {
    return napi_create_string_utf8(env, "", 0, result);
  }

  switch (encoding) {
    case UTF8:
      return napi_create_string_utf8(env, data, length, result);

    case ASCII:
    case LATIN1:
      return napi_create_string_latin1(env, data, length, result);

    case UCS2:
      // UCS2/UTF16LE: data is little-endian 16-bit code units.
      return napi_create_string_utf16(
          env, reinterpret_cast<const char16_t *>(data), length / 2, result);

    case HEX:
      return makeHexString(env, data, length, result);

    case BASE64:
      return makeBase64String(env, data, length, result, false);

    case BASE64URL:
      return makeBase64String(env, data, length, result, true);

    default:
      return napi_create_string_utf8(env, data, length, result);
  }
}

// ---------------------------------------------------------------------------
// StringDecoder state access helpers
// ---------------------------------------------------------------------------

static inline char *incompleteCharBuf(uint8_t *state) {
  return reinterpret_cast<char *>(state + kIncompleteCharactersStart);
}

static inline unsigned missingBytes(const uint8_t *state) {
  return state[kMissingBytes];
}

static inline unsigned bufferedBytes(const uint8_t *state) {
  return state[kBufferedBytes];
}

static inline Encoding decoderEncoding(const uint8_t *state) {
  return static_cast<Encoding>(state[kEncodingField]);
}

// ---------------------------------------------------------------------------
// Core decode logic (ported from Node's StringDecoder::DecodeData)
// ---------------------------------------------------------------------------

/// Decode data through the string decoder state machine.
/// Returns napi_ok on success, sets *result to the decoded JS string.
/// The nread value may be adjusted (increased if buffered chars are completed,
/// decreased if incomplete chars at end are buffered).
static napi_status decodeData(
    napi_env env,
    uint8_t *state,
    const char *data,
    size_t *nread_ptr,
    napi_value *result) {
  size_t nread = *nread_ptr;
  Encoding enc = decoderEncoding(state);

  if (enc == UTF8 || enc == UCS2 || enc == BASE64 || enc == BASE64URL) {
    napi_value prepend = nullptr;

    // See if we want bytes to finish a character from the previous chunk.
    if (missingBytes(state) > 0) {
      if (enc == UTF8) {
        // For UTF-8: check continuation bytes.
        for (size_t i = 0; i < nread && i < missingBytes(state); ++i) {
          if ((data[i] & 0xC0) != 0x80) {
            // Not a continuation byte — stop here.
            state[kMissingBytes] = 0;
            std::memcpy(
                incompleteCharBuf(state) + bufferedBytes(state), data, i);
            state[kBufferedBytes] += static_cast<uint8_t>(i);
            data += i;
            nread -= i;
            break;
          }
        }
      }

      size_t found_bytes =
          std::min(nread, static_cast<size_t>(missingBytes(state)));
      std::memcpy(
          incompleteCharBuf(state) + bufferedBytes(state), data, found_bytes);
      data += found_bytes;
      nread -= found_bytes;

      state[kMissingBytes] -= static_cast<uint8_t>(found_bytes);
      state[kBufferedBytes] += static_cast<uint8_t>(found_bytes);

      if (missingBytes(state) == 0) {
        // Complete character — create prepend string.
        napi_status st = makeString(
            env, incompleteCharBuf(state), bufferedBytes(state), enc, &prepend);
        if (st != napi_ok)
          return st;

        *nread_ptr += bufferedBytes(state);
        state[kBufferedBytes] = 0;
      }
    }

    napi_value body;

    if (nread == 0) {
      // All data consumed finishing the previous character.
      if (prepend) {
        *result = prepend;
      } else {
        napi_status st = napi_create_string_utf8(env, "", 0, result);
        if (st != napi_ok)
          return st;
      }
      return napi_ok;
    }

    // Check for incomplete character at end of input.
    if (enc == UTF8 && (data[nread - 1] & 0x80)) {
      // Non-ASCII byte at end — find the start of the incomplete character.
      for (size_t i = nread - 1;; --i) {
        state[kBufferedBytes]++;
        if ((data[i] & 0xC0) == 0x80) {
          // Trailing byte.
          if (state[kBufferedBytes] >= 4 || i == 0) {
            // Invalid UTF-8 or no lead byte in buffer.
            state[kBufferedBytes] = 0;
            break;
          }
        } else {
          // Lead byte found.
          if ((data[i] & 0xE0) == 0xC0) {
            state[kMissingBytes] = 2;
          } else if ((data[i] & 0xF0) == 0xE0) {
            state[kMissingBytes] = 3;
          } else if ((data[i] & 0xF8) == 0xF0) {
            state[kMissingBytes] = 4;
          } else {
            state[kBufferedBytes] = 0;
            break;
          }

          if (bufferedBytes(state) >= missingBytes(state)) {
            state[kMissingBytes] = 0;
            state[kBufferedBytes] = 0;
          }

          state[kMissingBytes] -= state[kBufferedBytes];
          break;
        }
      }
    } else if (enc == UCS2) {
      if ((nread % 2) == 1) {
        // Half a codepoint.
        state[kBufferedBytes] = 1;
        state[kMissingBytes] = 1;
      } else if ((data[nread - 1] & 0xFC) == 0xD8) {
        // Half a surrogate pair.
        state[kBufferedBytes] = 2;
        state[kMissingBytes] = 2;
      }
    } else if (enc == BASE64 || enc == BASE64URL) {
      state[kBufferedBytes] = static_cast<uint8_t>(nread % 3);
      if (state[kBufferedBytes] > 0)
        state[kMissingBytes] = 3 - state[kBufferedBytes];
    }

    if (bufferedBytes(state) > 0) {
      // Copy incomplete bytes from end of input.
      nread -= bufferedBytes(state);
      *nread_ptr -= bufferedBytes(state);
      std::memcpy(incompleteCharBuf(state), data + nread, bufferedBytes(state));
    }

    if (nread > 0) {
      napi_status st = makeString(env, data, nread, enc, &body);
      if (st != napi_ok)
        return st;
    } else {
      napi_status st = napi_create_string_utf8(env, "", 0, &body);
      if (st != napi_ok)
        return st;
    }

    if (!prepend) {
      *result = body;
      return napi_ok;
    }

    // Concatenate prepend + body via string concatenation.
    napi_value global, stringCtor, concatFn;
    napi_status st = napi_get_global(env, &global);
    if (st != napi_ok)
      return st;
    st = napi_get_named_property(env, global, "String", &stringCtor);
    if (st != napi_ok)
      return st;
    napi_value proto;
    st = napi_get_named_property(env, stringCtor, "prototype", &proto);
    if (st != napi_ok)
      return st;
    st = napi_get_named_property(env, proto, "concat", &concatFn);
    if (st != napi_ok)
      return st;
    napi_value args[] = {body};
    st = napi_call_function(env, prepend, concatFn, 1, args, result);
    return st;

  } else {
    // ASCII, HEX, LATIN1 — no multi-byte handling needed.
    return makeString(env, data, nread, enc, result);
  }
}

/// Flush any buffered incomplete character data.
static napi_status flushData(napi_env env, uint8_t *state, napi_value *result) {
  Encoding enc = decoderEncoding(state);

  if (enc == UCS2 && (bufferedBytes(state) % 2) == 1) {
    // Ignore a single trailing byte.
    state[kMissingBytes]--;
    state[kBufferedBytes]--;
  }

  if (bufferedBytes(state) == 0) {
    return napi_create_string_utf8(env, "", 0, result);
  }

  napi_status st = makeString(
      env, incompleteCharBuf(state), bufferedBytes(state), enc, result);

  state[kMissingBytes] = 0;
  state[kBufferedBytes] = 0;

  return st;
}

// ---------------------------------------------------------------------------
// NAPI callbacks
// ---------------------------------------------------------------------------

/// decode(decoderBuffer, inputBuffer) -> string
static napi_value decodeCallback(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  if (argc < 2) {
    napi_throw_error(env, nullptr, "decode requires 2 arguments");
    return nullptr;
  }

  // Get decoder state from first arg (Buffer/Uint8Array).
  uint8_t *decoderData = nullptr;
  size_t decoderLen = 0;
  napi_status st = napi_get_buffer_info(
      env, argv[0], reinterpret_cast<void **>(&decoderData), &decoderLen);
  if (st != napi_ok || decoderLen < kDecoderSize) {
    // Try as TypedArray.
    napi_typedarray_type type;
    size_t byteOffset;
    napi_value arrBuf;
    st = napi_get_typedarray_info(
        env,
        argv[0],
        &type,
        &decoderLen,
        reinterpret_cast<void **>(&decoderData),
        &arrBuf,
        &byteOffset);
    if (st != napi_ok || decoderLen < kDecoderSize) {
      napi_throw_error(env, nullptr, "Invalid decoder buffer");
      return nullptr;
    }
  }

  // Get input data from second arg (ArrayBufferView).
  uint8_t *inputData = nullptr;
  size_t inputLen = 0;
  // Try as TypedArray first.
  {
    napi_typedarray_type type;
    size_t byteOffset;
    napi_value arrBuf;
    st = napi_get_typedarray_info(
        env,
        argv[1],
        &type,
        &inputLen,
        reinterpret_cast<void **>(&inputData),
        &arrBuf,
        &byteOffset);
    if (st != napi_ok) {
      // Try as DataView.
      size_t byteLength;
      st = napi_get_dataview_info(
          env,
          argv[1],
          &byteLength,
          reinterpret_cast<void **>(&inputData),
          nullptr,
          nullptr);
      if (st != napi_ok) {
        napi_throw_error(env, nullptr, "Invalid input buffer");
        return nullptr;
      }
      inputLen = byteLength;
    }
  }

  size_t nread = inputLen;
  napi_value result;
  st = decodeData(
      env,
      decoderData,
      reinterpret_cast<const char *>(inputData),
      &nread,
      &result);
  if (st != napi_ok)
    return nullptr;

  return result;
}

/// flush(decoderBuffer) -> string
static napi_value flushCallback(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  if (argc < 1) {
    napi_throw_error(env, nullptr, "flush requires 1 argument");
    return nullptr;
  }

  // Get decoder state.
  uint8_t *decoderData = nullptr;
  size_t decoderLen = 0;
  napi_status st = napi_get_buffer_info(
      env, argv[0], reinterpret_cast<void **>(&decoderData), &decoderLen);
  if (st != napi_ok || decoderLen < kDecoderSize) {
    napi_typedarray_type type;
    size_t byteOffset;
    napi_value arrBuf;
    st = napi_get_typedarray_info(
        env,
        argv[0],
        &type,
        &decoderLen,
        reinterpret_cast<void **>(&decoderData),
        &arrBuf,
        &byteOffset);
    if (st != napi_ok || decoderLen < kDecoderSize) {
      napi_throw_error(env, nullptr, "Invalid decoder buffer");
      return nullptr;
    }
  }

  napi_value result;
  st = flushData(env, decoderData, &result);
  if (st != napi_ok)
    return nullptr;

  return result;
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------

napi_value initStringDecoderBinding(napi_env env, napi_value exports) {
  napi_value val;

  // Export constants.
#define SET_INT32(name, v)                                 \
  do {                                                     \
    napi_create_int32(env, static_cast<int32_t>(v), &val); \
    napi_set_named_property(env, exports, name, val);      \
  } while (0)

  SET_INT32("kIncompleteCharactersStart", kIncompleteCharactersStart);
  SET_INT32("kIncompleteCharactersEnd", kIncompleteCharactersEnd);
  SET_INT32("kMissingBytes", kMissingBytes);
  SET_INT32("kBufferedBytes", kBufferedBytes);
  SET_INT32("kEncodingField", kEncodingField);
  SET_INT32("kNumFields", kNumFields);
  SET_INT32("kSize", kDecoderSize);
#undef SET_INT32

  // Export encodings array.
  napi_value encodings;
  napi_create_array_with_length(env, 8, &encodings);

#define SET_ENCODING(idx, name)                               \
  do {                                                        \
    napi_value s;                                             \
    napi_create_string_utf8(env, name, NAPI_AUTO_LENGTH, &s); \
    napi_set_element(env, encodings, idx, s);                 \
  } while (0)

  // Indices match Node's enum encoding values.
  // LATIN1 = BINARY = 4, so "latin1" goes at index 4.
  SET_ENCODING(ASCII, "ascii");
  SET_ENCODING(UTF8, "utf8");
  SET_ENCODING(BASE64, "base64");
  SET_ENCODING(UCS2, "utf16le");
  SET_ENCODING(LATIN1, "latin1");
  SET_ENCODING(HEX, "hex");
  SET_ENCODING(BUFFER, "buffer");
  SET_ENCODING(BASE64URL, "base64url");

#undef SET_ENCODING

  napi_set_named_property(env, exports, "encodings", encodings);

  // Export decode and flush functions.
  napi_value fn;
  napi_create_function(
      env, "decode", NAPI_AUTO_LENGTH, decodeCallback, nullptr, &fn);
  napi_set_named_property(env, exports, "decode", fn);

  napi_create_function(
      env, "flush", NAPI_AUTO_LENGTH, flushCallback, nullptr, &fn);
  napi_set_named_property(env, exports, "flush", fn);

  return exports;
}

} // namespace node_compat
} // namespace hermes
