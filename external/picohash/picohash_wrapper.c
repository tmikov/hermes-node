/*
 * C wrapper for picohash -- compiles picohash.h as C and exports functions
 * with external linkage that can be called from C++.
 */
#include "picohash/picohash.h"

void ph_init_md5(picohash_ctx_t *ctx) {
  picohash_init_md5(ctx);
}

void ph_init_sha1(picohash_ctx_t *ctx) {
  picohash_init_sha1(ctx);
}

void ph_init_sha224(picohash_ctx_t *ctx) {
  picohash_init_sha224(ctx);
}

void ph_init_sha256(picohash_ctx_t *ctx) {
  picohash_init_sha256(ctx);
}

void ph_update(picohash_ctx_t *ctx, const void *input, size_t len) {
  picohash_update(ctx, input, len);
}

void ph_final(picohash_ctx_t *ctx, void *digest) {
  picohash_final(ctx, digest);
}

void ph_reset(picohash_ctx_t *ctx) {
  picohash_reset(ctx);
}

typedef void (*picohash_initf)(picohash_ctx_t *);

static picohash_initf algo_inits[] = {
    picohash_init_md5,
    picohash_init_sha1,
    picohash_init_sha224,
    picohash_init_sha256,
};

void ph_init_hmac(
    picohash_ctx_t *ctx,
    int algo,
    const void *key,
    size_t key_len) {
  picohash_init_hmac(ctx, algo_inits[algo], key, key_len);
}
