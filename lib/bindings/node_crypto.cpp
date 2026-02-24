/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <hermes/node-compat/bindings/node_crypto.h>
#include <hermes/node-compat/runtime/runtime_state.h>
#include <node_api.h>
#include <picohash_wrapper.h>
#include <uv.h>

#include <cstring>

namespace hermes {
namespace node_compat {

// ---------------------------------------------------------------------------
// Algorithm helpers
// ---------------------------------------------------------------------------

enum Algorithm {
  kMD5 = PH_MD5,
  kSHA1 = PH_SHA1,
  kSHA224 = PH_SHA224,
  kSHA256 = PH_SHA256
};

static bool parseAlgorithm(const char *name, Algorithm &out) {
  if (std::strcmp(name, "md5") == 0) {
    out = kMD5;
    return true;
  }
  if (std::strcmp(name, "sha1") == 0) {
    out = kSHA1;
    return true;
  }
  if (std::strcmp(name, "sha224") == 0) {
    out = kSHA224;
    return true;
  }
  if (std::strcmp(name, "sha256") == 0) {
    out = kSHA256;
    return true;
  }
  return false;
}

static void initCtx(picohash_ctx_t *ctx, Algorithm algo) {
  switch (algo) {
    case kMD5:
      ph_init_md5(ctx);
      break;
    case kSHA1:
      ph_init_sha1(ctx);
      break;
    case kSHA224:
      ph_init_sha224(ctx);
      break;
    case kSHA256:
      ph_init_sha256(ctx);
      break;
  }
}

// ---------------------------------------------------------------------------
// HashContext: shared native data for Hash and Hmac
// ---------------------------------------------------------------------------

struct HashContext {
  picohash_ctx_t ctx;
  Algorithm algo;
  bool finalized = false;
};

static void hashContextFinalizer(napi_env, void *data, void *) {
  delete static_cast<HashContext *>(data);
}

// ---------------------------------------------------------------------------
// Hash class
// ---------------------------------------------------------------------------

static napi_value hashNew(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1], thisVal;
  napi_get_cb_info(env, info, &argc, argv, &thisVal, nullptr);

  if (argc < 1) {
    napi_throw_error(env, nullptr, "algorithm argument required");
    return nullptr;
  }

  char algoBuf[32];
  size_t algoLen = 0;
  napi_get_value_string_utf8(env, argv[0], algoBuf, sizeof(algoBuf), &algoLen);

  Algorithm algo;
  if (!parseAlgorithm(algoBuf, algo)) {
    napi_throw_error(env, nullptr, "Unknown message digest");
    return nullptr;
  }

  auto *hctx = new HashContext();
  hctx->algo = algo;
  initCtx(&hctx->ctx, algo);

  napi_wrap(env, thisVal, hctx, hashContextFinalizer, nullptr, nullptr);
  return thisVal;
}

static napi_value hashUpdate(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1], thisVal;
  napi_get_cb_info(env, info, &argc, argv, &thisVal, nullptr);

  HashContext *hctx = nullptr;
  napi_unwrap(env, thisVal, reinterpret_cast<void **>(&hctx));
  if (!hctx || hctx->finalized) {
    napi_throw_error(env, nullptr, "Digest already called");
    return nullptr;
  }

  if (argc < 1)
    return thisVal;

  // Get buffer data from Uint8Array/Buffer.
  void *data = nullptr;
  size_t len = 0;
  napi_typedarray_type taType;
  napi_value abuf;
  size_t offset;
  napi_status st = napi_get_typedarray_info(
      env, argv[0], &taType, &len, &data, &abuf, &offset);
  if (st != napi_ok || !data) {
    napi_throw_type_error(env, nullptr, "data must be a Buffer or Uint8Array");
    return nullptr;
  }

  ph_update(&hctx->ctx, data, len);

  // Return true for success (JS wrapper chains .update calls).
  napi_value result;
  napi_get_boolean(env, true, &result);
  return result;
}

static napi_value hashDigest(napi_env env, napi_callback_info info) {
  napi_value thisVal;
  napi_get_cb_info(env, info, nullptr, nullptr, &thisVal, nullptr);

  HashContext *hctx = nullptr;
  napi_unwrap(env, thisVal, reinterpret_cast<void **>(&hctx));
  if (!hctx || hctx->finalized) {
    napi_throw_error(env, nullptr, "Digest already called");
    return nullptr;
  }

  unsigned char digest[PICOHASH_MAX_DIGEST_LENGTH];
  ph_final(&hctx->ctx, digest);
  hctx->finalized = true;

  napi_value result;
  napi_create_buffer_copy(
      env, hctx->ctx.digest_length, digest, nullptr, &result);
  return result;
}

static napi_value hashCopy(napi_env env, napi_callback_info info) {
  napi_value thisVal;
  napi_get_cb_info(env, info, nullptr, nullptr, &thisVal, nullptr);

  HashContext *hctx = nullptr;
  napi_unwrap(env, thisVal, reinterpret_cast<void **>(&hctx));
  if (!hctx) {
    napi_throw_error(env, nullptr, "Invalid Hash object");
    return nullptr;
  }

  // Create a new Hash instance via the constructor so it has prototype methods.
  napi_value hashCtorVal;
  napi_get_reference_value(
      env, getRuntimeState(env)->hashCtorRef, &hashCtorVal);

  const char *algoName = nullptr;
  switch (hctx->algo) {
    case kMD5:
      algoName = "md5";
      break;
    case kSHA1:
      algoName = "sha1";
      break;
    case kSHA224:
      algoName = "sha224";
      break;
    case kSHA256:
      algoName = "sha256";
      break;
  }
  napi_value algoStr;
  napi_create_string_utf8(env, algoName, NAPI_AUTO_LENGTH, &algoStr);

  napi_value newInstance;
  napi_new_instance(env, hashCtorVal, 1, &algoStr, &newInstance);

  // Overwrite the fresh context with a copy of the source state.
  HashContext *newCtx = nullptr;
  napi_unwrap(env, newInstance, reinterpret_cast<void **>(&newCtx));
  std::memcpy(&newCtx->ctx, &hctx->ctx, sizeof(picohash_ctx_t));
  newCtx->finalized = hctx->finalized;

  return newInstance;
}

// ---------------------------------------------------------------------------
// Hmac class
// ---------------------------------------------------------------------------

static napi_value hmacNew(napi_env env, napi_callback_info info) {
  napi_value thisVal;
  napi_get_cb_info(env, info, nullptr, nullptr, &thisVal, nullptr);
  // No-op constructor. Actual init happens in hmacInit.
  return thisVal;
}

static napi_value hmacInit(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2], thisVal;
  napi_get_cb_info(env, info, &argc, argv, &thisVal, nullptr);

  if (argc < 2) {
    napi_throw_error(env, nullptr, "algorithm and key arguments required");
    return nullptr;
  }

  char algoBuf[32];
  size_t algoLen = 0;
  napi_get_value_string_utf8(env, argv[0], algoBuf, sizeof(algoBuf), &algoLen);

  Algorithm algo;
  if (!parseAlgorithm(algoBuf, algo)) {
    napi_throw_error(env, nullptr, "Unknown message digest");
    return nullptr;
  }

  // Get key data.
  void *keyData = nullptr;
  size_t keyLen = 0;
  napi_typedarray_type taType;
  napi_value abuf;
  size_t offset;
  napi_status st = napi_get_typedarray_info(
      env, argv[1], &taType, &keyLen, &keyData, &abuf, &offset);
  if (st != napi_ok) {
    napi_throw_type_error(env, nullptr, "key must be a Buffer or Uint8Array");
    return nullptr;
  }

  auto *hctx = new HashContext();
  hctx->algo = algo;

  ph_init_hmac(
      &hctx->ctx,
      static_cast<int>(algo),
      keyData ? keyData : static_cast<const void *>(""),
      keyLen);

  napi_wrap(env, thisVal, hctx, hashContextFinalizer, nullptr, nullptr);
  return thisVal;
}

static napi_value hmacUpdate(napi_env env, napi_callback_info info) {
  return hashUpdate(env, info);
}

static napi_value hmacDigest(napi_env env, napi_callback_info info) {
  return hashDigest(env, info);
}

// ---------------------------------------------------------------------------
// Standalone functions
// ---------------------------------------------------------------------------

static napi_value getHashes(napi_env env, napi_callback_info) {
  napi_value result;
  napi_create_array_with_length(env, 4, &result);

  const char *names[] = {"md5", "sha1", "sha224", "sha256"};
  for (uint32_t i = 0; i < 4; ++i) {
    napi_value str;
    napi_create_string_utf8(env, names[i], NAPI_AUTO_LENGTH, &str);
    napi_set_element(env, result, i, str);
  }
  return result;
}

static napi_value getFipsCrypto(napi_env env, napi_callback_info) {
  napi_value result;
  napi_create_int32(env, 0, &result);
  return result;
}

static napi_value setFipsCrypto(napi_env env, napi_callback_info) {
  napi_value undef;
  napi_get_undefined(env, &undef);
  return undef;
}

static napi_value timingSafeEqual(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  if (argc < 2) {
    napi_throw_error(env, nullptr, "two arguments required");
    return nullptr;
  }

  void *dataA = nullptr, *dataB = nullptr;
  size_t lenA = 0, lenB = 0;
  napi_typedarray_type taType;
  napi_value abuf;
  size_t offset;

  napi_get_typedarray_info(
      env, argv[0], &taType, &lenA, &dataA, &abuf, &offset);
  napi_get_typedarray_info(
      env, argv[1], &taType, &lenB, &dataB, &abuf, &offset);

  if (lenA != lenB) {
    napi_throw_range_error(
        env,
        "ERR_CRYPTO_TIMING_SAFE_EQUAL_LENGTH",
        "Input buffers must have the same byte length");
    return nullptr;
  }

  // Constant-time comparison.
  volatile unsigned char result = 0;
  const volatile unsigned char *a =
      static_cast<const volatile unsigned char *>(dataA);
  const volatile unsigned char *b =
      static_cast<const volatile unsigned char *>(dataB);
  for (size_t i = 0; i < lenA; ++i) {
    result |= a[i] ^ b[i];
  }

  napi_value ret;
  napi_get_boolean(env, result == 0, &ret);
  return ret;
}

static napi_value randomFillSync(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  if (argc < 3) {
    napi_throw_error(
        env, nullptr, "buffer, offset, and size arguments required");
    return nullptr;
  }

  // Get typed array data pointer.
  void *data = nullptr;
  size_t byteLength = 0;
  napi_typedarray_type taType;
  napi_value abuf;
  size_t taOffset;
  napi_status st = napi_get_typedarray_info(
      env, argv[0], &taType, &byteLength, &data, &abuf, &taOffset);
  if (st != napi_ok || !data) {
    napi_throw_type_error(
        env, nullptr, "buffer must be a TypedArray or Buffer");
    return nullptr;
  }

  uint32_t offset = 0;
  napi_get_value_uint32(env, argv[1], &offset);

  uint32_t size = 0;
  napi_get_value_uint32(env, argv[2], &size);

  uv_loop_t *loop = getRuntimeState(env)->loop;
  uv_random_t req;
  int rc =
      uv_random(loop, &req, static_cast<char *>(data) + offset, size, 0, NULL);
  if (rc < 0) {
    napi_throw_error(env, nullptr, uv_strerror(rc));
    return nullptr;
  }

  napi_value undef;
  napi_get_undefined(env, &undef);
  return undef;
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------

napi_value initCryptoBinding(napi_env env, napi_value exports) {
  // Define Hash class.
  napi_property_descriptor hashProtoProps[] = {
      {"update",
       nullptr,
       hashUpdate,
       nullptr,
       nullptr,
       nullptr,
       napi_enumerable,
       nullptr},
      {"digest",
       nullptr,
       hashDigest,
       nullptr,
       nullptr,
       nullptr,
       napi_enumerable,
       nullptr},
      {"copy",
       nullptr,
       hashCopy,
       nullptr,
       nullptr,
       nullptr,
       napi_enumerable,
       nullptr},
  };

  napi_value hashCtor;
  napi_define_class(
      env,
      "Hash",
      NAPI_AUTO_LENGTH,
      hashNew,
      nullptr,
      sizeof(hashProtoProps) / sizeof(hashProtoProps[0]),
      hashProtoProps,
      &hashCtor);

  napi_create_reference(env, hashCtor, 1, &getRuntimeState(env)->hashCtorRef);

  // Define Hmac class.
  napi_property_descriptor hmacProtoProps[] = {
      {"init",
       nullptr,
       hmacInit,
       nullptr,
       nullptr,
       nullptr,
       napi_enumerable,
       nullptr},
      {"update",
       nullptr,
       hmacUpdate,
       nullptr,
       nullptr,
       nullptr,
       napi_enumerable,
       nullptr},
      {"digest",
       nullptr,
       hmacDigest,
       nullptr,
       nullptr,
       nullptr,
       napi_enumerable,
       nullptr},
  };

  napi_value hmacCtor;
  napi_define_class(
      env,
      "Hmac",
      NAPI_AUTO_LENGTH,
      hmacNew,
      nullptr,
      sizeof(hmacProtoProps) / sizeof(hmacProtoProps[0]),
      hmacProtoProps,
      &hmacCtor);

  // Constants needed by Node's internal/crypto/random.js.
  napi_value kSync, kAsync;
  napi_create_int32(env, 0, &kSync);
  napi_create_int32(env, 1, &kAsync);

  // clang-format off
  napi_property_descriptor props[] = {
    {"Hash",            nullptr, nullptr,         nullptr, nullptr, hashCtor, napi_enumerable, nullptr},
    {"Hmac",            nullptr, nullptr,         nullptr, nullptr, hmacCtor, napi_enumerable, nullptr},
    {"getHashes",       nullptr, getHashes,       nullptr, nullptr, nullptr,  napi_enumerable, nullptr},
    {"getFipsCrypto",   nullptr, getFipsCrypto,   nullptr, nullptr, nullptr,  napi_enumerable, nullptr},
    {"setFipsCrypto",   nullptr, setFipsCrypto,   nullptr, nullptr, nullptr,  napi_enumerable, nullptr},
    {"timingSafeEqual", nullptr, timingSafeEqual, nullptr, nullptr, nullptr,  napi_enumerable, nullptr},
    {"randomFillSync",  nullptr, randomFillSync,  nullptr, nullptr, nullptr,  napi_enumerable, nullptr},
    {"kCryptoJobSync",  nullptr, nullptr,         nullptr, nullptr, kSync,    napi_enumerable, nullptr},
    {"kCryptoJobAsync", nullptr, nullptr,         nullptr, nullptr, kAsync,   napi_enumerable, nullptr},
  };
  // clang-format on

  napi_define_properties(env, exports, sizeof(props) / sizeof(props[0]), props);
  return exports;
}

} // namespace node_compat
} // namespace hermes
