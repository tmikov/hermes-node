/*
 * C++-safe wrapper for picohash.
 *
 * picohash.h uses void-pointer-to-function-pointer casts that are valid in C
 * but illegal in C++.  This header declares the types (copied verbatim from
 * picohash.h) and wrapper functions with external C linkage so C++ code can
 * use picohash without including the original header directly.
 */
#ifndef PICOHASH_WRAPPER_H
#define PICOHASH_WRAPPER_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Constants (from picohash.h) ---- */

#define PICOHASH_MD5_BLOCK_LENGTH 64
#define PICOHASH_MD5_DIGEST_LENGTH 16
#define PICOHASH_SHA1_BLOCK_LENGTH 64
#define PICOHASH_SHA1_DIGEST_LENGTH 20
#define PICOHASH_SHA256_BLOCK_LENGTH 64
#define PICOHASH_SHA256_DIGEST_LENGTH 32
#define PICOHASH_SHA224_BLOCK_LENGTH PICOHASH_SHA256_BLOCK_LENGTH
#define PICOHASH_SHA224_DIGEST_LENGTH 28
#define PICOHASH_MAX_BLOCK_LENGTH 64
#define PICOHASH_MAX_DIGEST_LENGTH 32

/* ---- Type definitions (from picohash.h) ---- */

typedef struct _picohash_md5_ctx_t {
  uint_fast32_t lo, hi;
  uint_fast32_t a, b, c, d;
  unsigned char buffer[64];
  uint_fast32_t block[PICOHASH_MD5_DIGEST_LENGTH];
  const void *(*_body)(
      struct _picohash_md5_ctx_t *ctx,
      const void *data,
      size_t size);
} _picohash_md5_ctx_t;

typedef struct {
  uint32_t buffer[PICOHASH_SHA1_BLOCK_LENGTH / 4];
  uint32_t state[PICOHASH_SHA1_DIGEST_LENGTH / 4];
  uint64_t byteCount;
  uint8_t bufferOffset;
} _picohash_sha1_ctx_t;

typedef struct {
  uint64_t length;
  uint32_t state[PICOHASH_SHA256_DIGEST_LENGTH / 4];
  uint32_t curlen;
  unsigned char buf[PICOHASH_SHA256_BLOCK_LENGTH];
} _picohash_sha256_ctx_t;

typedef struct picohash_ctx_t {
  union {
    _picohash_md5_ctx_t _md5;
    _picohash_sha1_ctx_t _sha1;
    _picohash_sha256_ctx_t _sha256;
  };
  size_t block_length;
  size_t digest_length;
  void (*_reset)(void *ctx);
  void (*_update)(void *ctx, const void *input, size_t len);
  void (*_final)(void *ctx, void *digest);
  struct {
    unsigned char key[PICOHASH_MAX_BLOCK_LENGTH];
    void (*hash_reset)(void *ctx);
    void (*hash_final)(void *ctx, void *digest);
  } _hmac;
} picohash_ctx_t;

/* ---- Wrapper function declarations ---- */

void ph_init_md5(picohash_ctx_t *ctx);
void ph_init_sha1(picohash_ctx_t *ctx);
void ph_init_sha224(picohash_ctx_t *ctx);
void ph_init_sha256(picohash_ctx_t *ctx);
void ph_update(picohash_ctx_t *ctx, const void *input, size_t len);
void ph_final(picohash_ctx_t *ctx, void *digest);
void ph_reset(picohash_ctx_t *ctx);
void ph_init_hmac(
    picohash_ctx_t *ctx,
    int algo,
    const void *key,
    size_t key_len);

/* Algorithm constants for ph_init_hmac */
#define PH_MD5 0
#define PH_SHA1 1
#define PH_SHA224 2
#define PH_SHA256 3

#ifdef __cplusplus
}
#endif

#endif /* PICOHASH_WRAPPER_H */
