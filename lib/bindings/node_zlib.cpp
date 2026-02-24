/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 *
 * Ported from Node.js src/node_zlib.cc (MIT licensed).
 */

#include <hermes/node-compat/bindings/node_zlib.h>
#include <hermes/node-compat/runtime/runtime_state.h>
#include <node_api.h>

#include <brotli/decode.h>
#include <brotli/encode.h>
#include <zlib.h>
#include <zstd.h>
#include <zstd_errors.h>

#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace hermes {
namespace node_compat {
namespace {

// -----------------------------------------------------------------------
// Constants (matching node_zlib.cc)
// -----------------------------------------------------------------------

#define Z_MIN_CHUNK 64
#define Z_DEFAULT_CHUNK (16 * 1024)
#define Z_MIN_MEMLEVEL 1
#define Z_MAX_MEMLEVEL 9
#define Z_DEFAULT_MEMLEVEL 8
#define Z_MIN_LEVEL -1
#define Z_MAX_LEVEL 9
#define Z_DEFAULT_LEVEL Z_DEFAULT_COMPRESSION
#define Z_MIN_WINDOWBITS 8
#define Z_MAX_WINDOWBITS 15
#define Z_DEFAULT_WINDOWBITS 15

enum node_zlib_mode {
  NONE,
  DEFLATE,
  INFLATE,
  GZIP,
  GUNZIP,
  DEFLATERAW,
  INFLATERAW,
  UNZIP,
  BROTLI_DECODE,
  BROTLI_ENCODE,
  ZSTD_COMPRESS,
  ZSTD_DECOMPRESS
};

constexpr uint8_t GZIP_HEADER_ID1 = 0x1f;
constexpr uint8_t GZIP_HEADER_ID2 = 0x8b;

// -----------------------------------------------------------------------
// Error strerror helpers
// -----------------------------------------------------------------------

#define ZLIB_ERROR_CODES(V) \
  V(Z_OK)                   \
  V(Z_STREAM_END)           \
  V(Z_NEED_DICT)            \
  V(Z_ERRNO)                \
  V(Z_STREAM_ERROR)         \
  V(Z_DATA_ERROR)           \
  V(Z_MEM_ERROR)            \
  V(Z_BUF_ERROR)            \
  V(Z_VERSION_ERROR)

inline const char *ZlibStrerror(int err) {
#define V(code)    \
  if (err == code) \
    return #code;
  ZLIB_ERROR_CODES(V)
#undef V
  return "Z_UNKNOWN_ERROR";
}

#define ZSTD_ERROR_CODES(V)                       \
  V(ZSTD_error_no_error)                          \
  V(ZSTD_error_GENERIC)                           \
  V(ZSTD_error_prefix_unknown)                    \
  V(ZSTD_error_version_unsupported)               \
  V(ZSTD_error_frameParameter_unsupported)        \
  V(ZSTD_error_frameParameter_windowTooLarge)     \
  V(ZSTD_error_corruption_detected)               \
  V(ZSTD_error_checksum_wrong)                    \
  V(ZSTD_error_literals_headerWrong)              \
  V(ZSTD_error_dictionary_corrupted)              \
  V(ZSTD_error_dictionary_wrong)                  \
  V(ZSTD_error_dictionaryCreation_failed)         \
  V(ZSTD_error_parameter_unsupported)             \
  V(ZSTD_error_parameter_combination_unsupported) \
  V(ZSTD_error_parameter_outOfBound)              \
  V(ZSTD_error_tableLog_tooLarge)                 \
  V(ZSTD_error_maxSymbolValue_tooLarge)           \
  V(ZSTD_error_maxSymbolValue_tooSmall)           \
  V(ZSTD_error_stabilityCondition_notRespected)   \
  V(ZSTD_error_stage_wrong)                       \
  V(ZSTD_error_init_missing)                      \
  V(ZSTD_error_memory_allocation)                 \
  V(ZSTD_error_workSpace_tooSmall)                \
  V(ZSTD_error_dstSize_tooSmall)                  \
  V(ZSTD_error_srcSize_wrong)                     \
  V(ZSTD_error_dstBuffer_null)                    \
  V(ZSTD_error_noForwardProgress_destFull)        \
  V(ZSTD_error_noForwardProgress_inputEmpty)

inline const char *ZstdStrerror(int err) {
#define V(code)    \
  if (err == code) \
    return #code;
  ZSTD_ERROR_CODES(V)
#undef V
  return "ZSTD_error_GENERIC";
}

inline size_t typedArrayElementSize(napi_typedarray_type type) {
  switch (type) {
    case napi_uint16_array:
    case napi_int16_array:
      return 2;
    case napi_uint32_array:
    case napi_int32_array:
    case napi_float32_array:
      return 4;
    case napi_float64_array:
    case napi_bigint64_array:
    case napi_biguint64_array:
      return 8;
    default:
      return 1;
  }
}

bool getArrayBufferViewBytes(
    napi_env env,
    napi_value value,
    const uint8_t **data,
    size_t *length) {
  void *rawData = nullptr;
  size_t rawLength = 0;

  if (napi_get_buffer_info(env, value, &rawData, &rawLength) == napi_ok) {
    *data = static_cast<const uint8_t *>(rawData);
    *length = rawLength;
    return true;
  }

  napi_typedarray_type taType;
  napi_value arrBuf;
  size_t byteOffset;
  if (napi_get_typedarray_info(
          env, value, &taType, &rawLength, &rawData, &arrBuf, &byteOffset) ==
      napi_ok) {
    if (rawData == nullptr && rawLength > 0) {
      void *arrData = nullptr;
      size_t arrLen = 0;
      if (napi_get_arraybuffer_info(env, arrBuf, &arrData, &arrLen) ==
              napi_ok &&
          arrData != nullptr && byteOffset < arrLen) {
        rawData = static_cast<uint8_t *>(arrData) + byteOffset;
      }
    }
    *data = static_cast<const uint8_t *>(rawData);
    *length = rawLength * typedArrayElementSize(taType);
    return true;
  }

  if (napi_get_dataview_info(
          env, value, &rawLength, &rawData, &arrBuf, &byteOffset) == napi_ok) {
    if (rawData == nullptr && rawLength > 0) {
      void *arrData = nullptr;
      size_t arrLen = 0;
      if (napi_get_arraybuffer_info(env, arrBuf, &arrData, &arrLen) ==
              napi_ok &&
          arrData != nullptr && byteOffset < arrLen) {
        rawData = static_cast<uint8_t *>(arrData) + byteOffset;
      }
    }
    *data = static_cast<const uint8_t *>(rawData);
    *length = rawLength;
    return true;
  }

  return false;
}

// -----------------------------------------------------------------------
// CompressionError
// -----------------------------------------------------------------------

struct CompressionError {
  CompressionError(const char *message, const char *code, int err)
      : message(message), code(code), err(err) {}
  CompressionError() = default;

  const char *message = nullptr;
  const char *code = nullptr;
  int err = 0;

  bool IsError() const {
    return code != nullptr;
  }
};

// -----------------------------------------------------------------------
// ZlibContext -- wraps z_stream
// -----------------------------------------------------------------------

class ZlibContext {
 public:
  ZlibContext() {
    std::memset(&strm_, 0, sizeof(strm_));
  }

  void Close() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!zlib_init_done_) {
        dictionary_.clear();
        mode_ = NONE;
        return;
      }
    }

    if (mode_ == DEFLATE || mode_ == GZIP || mode_ == DEFLATERAW) {
      deflateEnd(&strm_);
    } else if (
        mode_ == INFLATE || mode_ == GUNZIP || mode_ == INFLATERAW ||
        mode_ == UNZIP) {
      inflateEnd(&strm_);
    }
    mode_ = NONE;
    dictionary_.clear();
  }

  void DoThreadPoolWork() {
    bool first_init_call = InitZlib();
    if (first_init_call && err_ != Z_OK)
      return;

    const Bytef *next_expected_header_byte = nullptr;

    switch (mode_) {
      case DEFLATE:
      case GZIP:
      case DEFLATERAW:
        err_ = deflate(&strm_, flush_);
        break;
      case UNZIP:
        if (strm_.avail_in > 0)
          next_expected_header_byte = strm_.next_in;

        switch (gzip_id_bytes_read_) {
          case 0:
            if (next_expected_header_byte == nullptr)
              break;
            if (*next_expected_header_byte == GZIP_HEADER_ID1) {
              gzip_id_bytes_read_ = 1;
              next_expected_header_byte++;
              if (strm_.avail_in == 1)
                break;
            } else {
              mode_ = INFLATE;
              break;
            }
            [[fallthrough]];
          case 1:
            if (next_expected_header_byte == nullptr)
              break;
            if (*next_expected_header_byte == GZIP_HEADER_ID2) {
              gzip_id_bytes_read_ = 2;
              mode_ = GUNZIP;
            } else {
              mode_ = INFLATE;
            }
            break;
          default:
            break;
        }
        [[fallthrough]];
      case INFLATE:
      case GUNZIP:
      case INFLATERAW:
        err_ = inflate(&strm_, flush_);

        if (mode_ != INFLATERAW && err_ == Z_NEED_DICT &&
            !dictionary_.empty()) {
          err_ = inflateSetDictionary(
              &strm_, dictionary_.data(), dictionary_.size());
          if (err_ == Z_OK) {
            err_ = inflate(&strm_, flush_);
          } else if (err_ == Z_DATA_ERROR) {
            err_ = Z_NEED_DICT;
          }
        }

        while (strm_.avail_in > 0 && mode_ == GUNZIP && err_ == Z_STREAM_END &&
               strm_.next_in[0] != 0x00) {
          ResetStream();
          err_ = inflate(&strm_, flush_);
        }
        break;
      default:
        break;
    }
  }

  void
  SetBuffers(const char *in, uint32_t in_len, char *out, uint32_t out_len) {
    strm_.avail_in = in_len;
    strm_.next_in = const_cast<Bytef *>(reinterpret_cast<const Bytef *>(in));
    strm_.avail_out = out_len;
    strm_.next_out = reinterpret_cast<Bytef *>(out);
  }

  void SetFlush(int flush) {
    flush_ = flush;
  }

  void GetAfterWriteOffsets(uint32_t *avail_in, uint32_t *avail_out) const {
    *avail_in = strm_.avail_in;
    *avail_out = strm_.avail_out;
  }

  CompressionError GetErrorInfo() const {
    switch (err_) {
      case Z_OK:
      case Z_BUF_ERROR:
        if (strm_.avail_out != 0 && flush_ == Z_FINISH)
          return ErrorForMessage("unexpected end of file");
        [[fallthrough]];
      case Z_STREAM_END:
        break;
      case Z_NEED_DICT:
        if (dictionary_.empty())
          return ErrorForMessage("Missing dictionary");
        else
          return ErrorForMessage("Bad dictionary");
      default:
        return ErrorForMessage("Zlib error");
    }
    return CompressionError{};
  }

  void SetMode(node_zlib_mode mode) {
    mode_ = mode;
  }

  CompressionError ResetStream() {
    bool first_init_call = InitZlib();
    if (first_init_call && err_ != Z_OK)
      return ErrorForMessage("Failed to init stream before reset");

    err_ = Z_OK;
    switch (mode_) {
      case DEFLATE:
      case DEFLATERAW:
      case GZIP:
        err_ = deflateReset(&strm_);
        break;
      case INFLATE:
      case INFLATERAW:
      case GUNZIP:
        err_ = inflateReset(&strm_);
        break;
      default:
        break;
    }

    if (err_ != Z_OK)
      return ErrorForMessage("Failed to reset stream");

    return SetDictionary();
  }

  void Init(
      int level,
      int window_bits,
      int mem_level,
      int strategy,
      std::vector<unsigned char> &&dictionary) {
    level_ = level;
    window_bits_ = window_bits;
    mem_level_ = mem_level;
    strategy_ = strategy;
    flush_ = Z_NO_FLUSH;
    err_ = Z_OK;

    if (mode_ == GZIP || mode_ == GUNZIP)
      window_bits_ += 16;
    if (mode_ == UNZIP)
      window_bits_ += 32;
    if (mode_ == DEFLATERAW || mode_ == INFLATERAW)
      window_bits_ *= -1;

    dictionary_ = std::move(dictionary);
  }

  CompressionError SetParams(int level, int strategy) {
    bool first_init_call = InitZlib();
    if (first_init_call && err_ != Z_OK)
      return ErrorForMessage("Failed to init stream before set parameters");

    err_ = Z_OK;
    switch (mode_) {
      case DEFLATE:
      case DEFLATERAW:
        err_ = deflateParams(&strm_, level, strategy);
        break;
      default:
        break;
    }

    if (err_ != Z_OK && err_ != Z_BUF_ERROR)
      return ErrorForMessage("Failed to set parameters");

    return CompressionError{};
  }

 private:
  CompressionError ErrorForMessage(const char *message) const {
    if (strm_.msg != nullptr)
      message = strm_.msg;
    return CompressionError{message, ZlibStrerror(err_), err_};
  }

  CompressionError SetDictionary() {
    if (dictionary_.empty())
      return CompressionError{};

    err_ = Z_OK;
    switch (mode_) {
      case DEFLATE:
      case DEFLATERAW:
        err_ = deflateSetDictionary(
            &strm_, dictionary_.data(), dictionary_.size());
        break;
      case INFLATERAW:
        err_ = inflateSetDictionary(
            &strm_, dictionary_.data(), dictionary_.size());
        break;
      default:
        break;
    }

    if (err_ != Z_OK)
      return ErrorForMessage("Failed to set dictionary");

    return CompressionError{};
  }

  bool InitZlib() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (zlib_init_done_)
      return false;

    switch (mode_) {
      case DEFLATE:
      case GZIP:
      case DEFLATERAW:
        err_ = deflateInit2(
            &strm_, level_, Z_DEFLATED, window_bits_, mem_level_, strategy_);
        break;
      case INFLATE:
      case GUNZIP:
      case INFLATERAW:
      case UNZIP:
        err_ = inflateInit2(&strm_, window_bits_);
        break;
      default:
        break;
    }

    if (err_ != Z_OK) {
      dictionary_.clear();
      mode_ = NONE;
      return true;
    }

    SetDictionary();
    zlib_init_done_ = true;
    return true;
  }

  std::mutex mutex_;
  bool zlib_init_done_ = false;
  int err_ = 0;
  int flush_ = 0;
  int level_ = 0;
  int mem_level_ = 0;
  node_zlib_mode mode_ = NONE;
  int strategy_ = 0;
  int window_bits_ = 0;
  unsigned int gzip_id_bytes_read_ = 0;
  std::vector<unsigned char> dictionary_;
  z_stream strm_;
};

// -----------------------------------------------------------------------
// BrotliContext base
// -----------------------------------------------------------------------

class BrotliContext {
 public:
  BrotliContext() = default;

  void
  SetBuffers(const char *in, uint32_t in_len, char *out, uint32_t out_len) {
    next_in_ = reinterpret_cast<const uint8_t *>(in);
    next_out_ = reinterpret_cast<uint8_t *>(out);
    avail_in_ = in_len;
    avail_out_ = out_len;
  }

  void SetFlush(int flush) {
    flush_ = static_cast<BrotliEncoderOperation>(flush);
  }

  void GetAfterWriteOffsets(uint32_t *avail_in, uint32_t *avail_out) const {
    *avail_in = static_cast<uint32_t>(avail_in_);
    *avail_out = static_cast<uint32_t>(avail_out_);
  }

  void SetMode(node_zlib_mode mode) {
    mode_ = mode;
  }

 protected:
  node_zlib_mode mode_ = NONE;
  const uint8_t *next_in_ = nullptr;
  uint8_t *next_out_ = nullptr;
  size_t avail_in_ = 0;
  size_t avail_out_ = 0;
  BrotliEncoderOperation flush_ = BROTLI_OPERATION_PROCESS;
};

// -----------------------------------------------------------------------
// BrotliEncoderContext
// -----------------------------------------------------------------------

class BrotliEncoderContext : public BrotliContext {
 public:
  void Close() {
    if (state_) {
      BrotliEncoderDestroyInstance(state_);
      state_ = nullptr;
    }
    mode_ = NONE;
  }

  void DoThreadPoolWork() {
    const uint8_t *next_in = next_in_;
    last_result_ = BrotliEncoderCompressStream(
        state_, flush_, &avail_in_, &next_in, &avail_out_, &next_out_, nullptr);
    next_in_ += next_in - next_in_;
  }

  CompressionError Init() {
    if (state_)
      BrotliEncoderDestroyInstance(state_);
    state_ = BrotliEncoderCreateInstance(nullptr, nullptr, nullptr);
    if (!state_) {
      return CompressionError(
          "Could not initialize Brotli instance",
          "ERR_ZLIB_INITIALIZATION_FAILED",
          -1);
    }
    return CompressionError{};
  }

  CompressionError ResetStream() {
    return Init();
  }

  CompressionError SetParams(int key, uint32_t value) {
    if (!BrotliEncoderSetParameter(
            state_, static_cast<BrotliEncoderParameter>(key), value)) {
      return CompressionError(
          "Setting parameter failed", "ERR_BROTLI_PARAM_SET_FAILED", -1);
    }
    return CompressionError{};
  }

  CompressionError GetErrorInfo() const {
    if (!last_result_) {
      return CompressionError(
          "Compression failed", "ERR_BROTLI_COMPRESSION_FAILED", -1);
    }
    return CompressionError{};
  }

  ~BrotliEncoderContext() {
    if (state_)
      BrotliEncoderDestroyInstance(state_);
  }

 private:
  bool last_result_ = false;
  BrotliEncoderState *state_ = nullptr;
};

// -----------------------------------------------------------------------
// BrotliDecoderContext
// -----------------------------------------------------------------------

class BrotliDecoderContext : public BrotliContext {
 public:
  void Close() {
    if (state_) {
      BrotliDecoderDestroyInstance(state_);
      state_ = nullptr;
    }
    mode_ = NONE;
  }

  void DoThreadPoolWork() {
    const uint8_t *next_in = next_in_;
    last_result_ = BrotliDecoderDecompressStream(
        state_, &avail_in_, &next_in, &avail_out_, &next_out_, nullptr);
    next_in_ += next_in - next_in_;
    if (last_result_ == BROTLI_DECODER_RESULT_ERROR) {
      error_ = BrotliDecoderGetErrorCode(state_);
      error_string_ = std::string("ERR_") + BrotliDecoderErrorString(error_);
    }
  }

  CompressionError Init() {
    if (state_)
      BrotliDecoderDestroyInstance(state_);
    state_ = BrotliDecoderCreateInstance(nullptr, nullptr, nullptr);
    if (!state_) {
      return CompressionError(
          "Could not initialize Brotli instance",
          "ERR_ZLIB_INITIALIZATION_FAILED",
          -1);
    }
    return CompressionError{};
  }

  CompressionError ResetStream() {
    return Init();
  }

  CompressionError SetParams(int key, uint32_t value) {
    if (!BrotliDecoderSetParameter(
            state_, static_cast<BrotliDecoderParameter>(key), value)) {
      return CompressionError(
          "Setting parameter failed", "ERR_BROTLI_PARAM_SET_FAILED", -1);
    }
    return CompressionError{};
  }

  CompressionError GetErrorInfo() const {
    if (error_ != BROTLI_DECODER_NO_ERROR) {
      return CompressionError(
          "Decompression failed",
          error_string_.c_str(),
          static_cast<int>(error_));
    } else if (
        flush_ == BROTLI_OPERATION_FINISH &&
        last_result_ == BROTLI_DECODER_RESULT_NEEDS_MORE_INPUT) {
      return CompressionError(
          "unexpected end of file", "Z_BUF_ERROR", Z_BUF_ERROR);
    }
    return CompressionError{};
  }

  ~BrotliDecoderContext() {
    if (state_)
      BrotliDecoderDestroyInstance(state_);
  }

 private:
  BrotliDecoderResult last_result_ = BROTLI_DECODER_RESULT_SUCCESS;
  BrotliDecoderErrorCode error_ = BROTLI_DECODER_NO_ERROR;
  std::string error_string_;
  BrotliDecoderState *state_ = nullptr;
};

// -----------------------------------------------------------------------
// ZstdContext base
// -----------------------------------------------------------------------

class ZstdContext {
 public:
  ZstdContext() = default;

  void Close() {}

  void
  SetBuffers(const char *in, uint32_t in_len, char *out, uint32_t out_len) {
    input_.src = reinterpret_cast<const uint8_t *>(in);
    input_.size = in_len;
    input_.pos = 0;
    output_.dst = reinterpret_cast<uint8_t *>(out);
    output_.size = out_len;
    output_.pos = 0;
  }

  void SetFlush(int flush) {
    flush_ = static_cast<ZSTD_EndDirective>(flush);
  }

  void GetAfterWriteOffsets(uint32_t *avail_in, uint32_t *avail_out) const {
    *avail_in = static_cast<uint32_t>(input_.size - input_.pos);
    *avail_out = static_cast<uint32_t>(output_.size - output_.pos);
  }

  CompressionError GetErrorInfo() const {
    if (error_ != ZSTD_error_no_error) {
      return CompressionError(
          error_string_.c_str(),
          error_code_string_.c_str(),
          static_cast<int>(error_));
    }
    return {};
  }

 protected:
  ZSTD_EndDirective flush_ = ZSTD_e_continue;
  ZSTD_inBuffer input_ = {nullptr, 0, 0};
  ZSTD_outBuffer output_ = {nullptr, 0, 0};
  ZSTD_ErrorCode error_ = ZSTD_error_no_error;
  std::string error_string_;
  std::string error_code_string_;
};

// -----------------------------------------------------------------------
// ZstdCompressContext
// -----------------------------------------------------------------------

class ZstdCompressContext : public ZstdContext {
 public:
  ~ZstdCompressContext() {
    if (cctx_)
      ZSTD_freeCCtx(cctx_);
  }

  void Close() {
    if (cctx_) {
      ZSTD_freeCCtx(cctx_);
      cctx_ = nullptr;
    }
    ZstdContext::Close();
  }

  void DoThreadPoolWork() {
    size_t const remaining =
        ZSTD_compressStream2(cctx_, &output_, &input_, flush_);
    if (ZSTD_isError(remaining)) {
      error_ = ZSTD_getErrorCode(remaining);
      error_code_string_ = ZstdStrerror(error_);
      error_string_ = ZSTD_getErrorString(error_);
    }
  }

  CompressionError Init(
      uint64_t pledged_src_size,
      std::string_view dictionary = {}) {
    pledged_src_size_ = pledged_src_size;
    if (cctx_)
      ZSTD_freeCCtx(cctx_);
    cctx_ = ZSTD_createCCtx();
    if (!cctx_) {
      return CompressionError(
          "Could not initialize zstd instance",
          "ERR_ZLIB_INITIALIZATION_FAILED",
          -1);
    }

    if (!dictionary.empty()) {
      size_t ret =
          ZSTD_CCtx_loadDictionary(cctx_, dictionary.data(), dictionary.size());
      if (ZSTD_isError(ret)) {
        return CompressionError(
            "Failed to load zstd dictionary",
            "ERR_ZLIB_DICTIONARY_LOAD_FAILED",
            -1);
      }
    }

    size_t result = ZSTD_CCtx_setPledgedSrcSize(cctx_, pledged_src_size);
    if (ZSTD_isError(result)) {
      return CompressionError(
          "Could not set pledged src size",
          "ERR_ZLIB_INITIALIZATION_FAILED",
          -1);
    }
    return {};
  }

  CompressionError ResetStream() {
    return Init(pledged_src_size_);
  }

  CompressionError SetParameter(int key, int value) {
    size_t result =
        ZSTD_CCtx_setParameter(cctx_, static_cast<ZSTD_cParameter>(key), value);
    if (ZSTD_isError(result)) {
      return CompressionError(
          "Setting parameter failed", "ERR_ZSTD_PARAM_SET_FAILED", -1);
    }
    return {};
  }

 private:
  ZSTD_CCtx *cctx_ = nullptr;
  uint64_t pledged_src_size_ = ZSTD_CONTENTSIZE_UNKNOWN;
};

// -----------------------------------------------------------------------
// ZstdDecompressContext
// -----------------------------------------------------------------------

class ZstdDecompressContext : public ZstdContext {
 public:
  ~ZstdDecompressContext() {
    if (dctx_)
      ZSTD_freeDCtx(dctx_);
  }

  void Close() {
    if (dctx_) {
      ZSTD_freeDCtx(dctx_);
      dctx_ = nullptr;
    }
    ZstdContext::Close();
  }

  void DoThreadPoolWork() {
    size_t const ret = ZSTD_decompressStream(dctx_, &output_, &input_);
    if (ZSTD_isError(ret)) {
      error_ = ZSTD_getErrorCode(ret);
      error_code_string_ = ZstdStrerror(error_);
      error_string_ = ZSTD_getErrorString(error_);
    }
  }

  CompressionError Init(
      uint64_t /*pledged_src_size*/,
      std::string_view dictionary = {}) {
    if (dctx_)
      ZSTD_freeDCtx(dctx_);
    dctx_ = ZSTD_createDCtx();
    if (!dctx_) {
      return CompressionError(
          "Could not initialize zstd instance",
          "ERR_ZLIB_INITIALIZATION_FAILED",
          -1);
    }

    if (!dictionary.empty()) {
      size_t ret =
          ZSTD_DCtx_loadDictionary(dctx_, dictionary.data(), dictionary.size());
      if (ZSTD_isError(ret)) {
        return CompressionError(
            "Failed to load zstd dictionary",
            "ERR_ZLIB_DICTIONARY_LOAD_FAILED",
            -1);
      }
    }
    return {};
  }

  CompressionError ResetStream() {
    return Init(ZSTD_CONTENTSIZE_UNKNOWN);
  }

  CompressionError SetParameter(int key, int value) {
    size_t result =
        ZSTD_DCtx_setParameter(dctx_, static_cast<ZSTD_dParameter>(key), value);
    if (ZSTD_isError(result)) {
      return CompressionError(
          "Setting parameter failed", "ERR_ZSTD_PARAM_SET_FAILED", -1);
    }
    return {};
  }

 private:
  ZSTD_DCtx *dctx_ = nullptr;
};

// -----------------------------------------------------------------------
// CompressionStream<Context> -- NAPI wrapper (templated)
// -----------------------------------------------------------------------

template <typename Context>
class CompressionStream {
 public:
  CompressionStream() = default;

  ~CompressionStream() {
    ctx_.Close();
  }

  Context *context() {
    return &ctx_;
  }

  // ---- Static NAPI callbacks ----

  static void Destructor(napi_env, void *data, void *) {
    delete static_cast<CompressionStream *>(data);
  }

  static napi_value New(napi_env env, napi_callback_info info) {
    napi_value thisVal;
    napi_get_cb_info(env, info, nullptr, nullptr, &thisVal, nullptr);

    auto *stream = new CompressionStream();
    napi_wrap(env, thisVal, stream, Destructor, nullptr, nullptr);
    return thisVal;
  }

  static CompressionStream *Unwrap(napi_env env, napi_value thisVal) {
    CompressionStream *stream = nullptr;
    napi_unwrap(env, thisVal, reinterpret_cast<void **>(&stream));
    return stream;
  }

  // write(flush, in, in_off, in_len, out, out_off, out_len)
  template <bool async>
  static napi_value Write(napi_env env, napi_callback_info info) {
    size_t argc = 7;
    napi_value argv[7], thisVal;
    napi_get_cb_info(env, info, &argc, argv, &thisVal, nullptr);

    auto *stream = Unwrap(env, thisVal);
    if (!stream || !stream->init_done_ || stream->closed_) {
      napi_throw_error(env, nullptr, "zlib binding closed");
      return nullptr;
    }

    uint32_t flush;
    napi_get_value_uint32(env, argv[0], &flush);

    const char *in = nullptr;
    uint32_t in_len = 0;

    napi_valuetype inType;
    napi_typeof(env, argv[1], &inType);
    if (inType != napi_null) {
      void *inData = nullptr;
      size_t inBufLen = 0;
      napi_get_buffer_info(env, argv[1], &inData, &inBufLen);

      uint32_t in_off;
      napi_get_value_uint32(env, argv[2], &in_off);
      napi_get_value_uint32(env, argv[3], &in_len);

      in = static_cast<const char *>(inData) + in_off;
    }

    void *outData = nullptr;
    size_t outBufLen = 0;
    napi_get_buffer_info(env, argv[4], &outData, &outBufLen);

    uint32_t out_off, out_len;
    napi_get_value_uint32(env, argv[5], &out_off);
    napi_get_value_uint32(env, argv[6], &out_len);

    char *out = static_cast<char *>(outData) + out_off;

    stream->ctx_.SetBuffers(in, in_len, out, out_len);
    stream->ctx_.SetFlush(flush);

    if constexpr (!async) {
      // Sync version
      stream->ctx_.DoThreadPoolWork();

      const CompressionError err = stream->ctx_.GetErrorInfo();
      if (err.IsError()) {
        stream->EmitError(env, thisVal, err);
      } else {
        stream->UpdateWriteResult();
      }
      return nullptr;
    }

    // Async version: queue a napi_async_work
    stream->write_in_progress_ = true;

    // Store thisVal ref so it isn't GC'd during async work.
    napi_ref thisRef;
    napi_create_reference(env, thisVal, 1, &thisRef);

    struct AsyncData {
      CompressionStream *stream;
      napi_ref thisRef;
      napi_async_work work;
    };

    auto *ad = new AsyncData{stream, thisRef, nullptr};

    napi_value resourceName;
    napi_create_string_utf8(env, "zlib", NAPI_AUTO_LENGTH, &resourceName);
    napi_create_async_work(
        env,
        nullptr,
        resourceName,
        // Execute (thread pool)
        [](napi_env, void *data) {
          auto *ad = static_cast<AsyncData *>(data);
          ad->stream->ctx_.DoThreadPoolWork();
        },
        // Complete (main thread)
        [](napi_env env, napi_status status, void *data) {
          auto *ad = static_cast<AsyncData *>(data);
          auto *stream = ad->stream;
          stream->write_in_progress_ = false;

          napi_handle_scope scope;
          napi_open_handle_scope(env, &scope);

          napi_value thisVal;
          napi_get_reference_value(env, ad->thisRef, &thisVal);
          napi_delete_reference(env, ad->thisRef);

          napi_delete_async_work(env, ad->work);

          if (status == napi_cancelled) {
            stream->ctx_.Close();
            stream->closed_ = true;
            delete ad;
            napi_close_handle_scope(env, scope);
            return;
          }

          const CompressionError err = stream->ctx_.GetErrorInfo();
          if (err.IsError()) {
            stream->EmitError(env, thisVal, err);
          } else {
            stream->UpdateWriteResult();

            // Call processCallback
            if (stream->callbackRef_) {
              napi_value cb;
              napi_get_reference_value(env, stream->callbackRef_, &cb);
              napi_call_function(env, thisVal, cb, 0, nullptr, nullptr);

              bool pending = false;
              napi_is_exception_pending(env, &pending);
              if (pending) {
                napi_value exc;
                napi_get_and_clear_last_exception(env, &exc);
              }
            }
          }

          if (stream->pending_close_) {
            stream->ctx_.Close();
            stream->closed_ = true;
            stream->pending_close_ = false;
          }

          delete ad;
          napi_close_handle_scope(env, scope);
        },
        ad,
        &ad->work);
    napi_queue_async_work(env, ad->work);

    return nullptr;
  }

  static napi_value DoClose(napi_env env, napi_callback_info info) {
    napi_value thisVal;
    napi_get_cb_info(env, info, nullptr, nullptr, &thisVal, nullptr);

    auto *stream = Unwrap(env, thisVal);
    if (!stream)
      return nullptr;

    if (stream->write_in_progress_) {
      stream->pending_close_ = true;
      return nullptr;
    }

    stream->ctx_.Close();
    stream->closed_ = true;
    return nullptr;
  }

  static napi_value DoReset(napi_env env, napi_callback_info info) {
    napi_value thisVal;
    napi_get_cb_info(env, info, nullptr, nullptr, &thisVal, nullptr);

    auto *stream = Unwrap(env, thisVal);
    if (!stream)
      return nullptr;

    const CompressionError err = stream->ctx_.ResetStream();
    if (err.IsError())
      stream->EmitError(env, thisVal, err);
    return nullptr;
  }

  // ---- Initialization helpers ----

  void InitStream(napi_env env, uint32_t *write_result, napi_value callback) {
    write_result_ = write_result;
    napi_create_reference(env, callback, 1, &callbackRef_);
    init_done_ = true;
  }

  void UpdateWriteResult() {
    if (!write_result_)
      return;
    uint32_t avail_in, avail_out;
    ctx_.GetAfterWriteOffsets(&avail_in, &avail_out);
    write_result_[0] = avail_out;
    write_result_[1] = avail_in;
  }

  void
  EmitError(napi_env env, napi_value thisVal, const CompressionError &err) {
    // Call handle.onerror(message, errno, code)
    napi_value onerrorFn;
    napi_get_named_property(env, thisVal, "onerror", &onerrorFn);

    napi_valuetype fnType;
    napi_typeof(env, onerrorFn, &fnType);
    if (fnType != napi_function)
      return;

    napi_value msgVal, errnoVal, codeVal;
    napi_create_string_utf8(
        env, err.message ? err.message : "", NAPI_AUTO_LENGTH, &msgVal);
    napi_create_int32(env, err.err, &errnoVal);
    napi_create_string_utf8(
        env, err.code ? err.code : "", NAPI_AUTO_LENGTH, &codeVal);

    napi_value args[3] = {msgVal, errnoVal, codeVal};
    napi_call_function(env, thisVal, onerrorFn, 3, args, nullptr);

    bool pending = false;
    napi_is_exception_pending(env, &pending);
    if (pending) {
      napi_value exc;
      napi_get_and_clear_last_exception(env, &exc);
    }

    write_in_progress_ = false;
    if (pending_close_) {
      ctx_.Close();
      closed_ = true;
      pending_close_ = false;
    }
  }

 private:
  Context ctx_;
  napi_ref callbackRef_ = nullptr;
  uint32_t *write_result_ = nullptr;
  bool init_done_ = false;
  bool write_in_progress_ = false;
  bool pending_close_ = false;
  bool closed_ = false;
};

// -----------------------------------------------------------------------
// Type-specific Init/Params functions
// -----------------------------------------------------------------------

// --- ZlibStream ---

using ZlibStream = CompressionStream<ZlibContext>;

static napi_value zlibNew(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1], thisVal;
  napi_get_cb_info(env, info, &argc, argv, &thisVal, nullptr);

  auto *stream = new ZlibStream();
  napi_wrap(env, thisVal, stream, ZlibStream::Destructor, nullptr, nullptr);

  int32_t mode;
  napi_get_value_int32(env, argv[0], &mode);
  stream->context()->SetMode(static_cast<node_zlib_mode>(mode));
  return thisVal;
}

// init(windowBits, level, memLevel, strategy, writeState, callback, dictionary)
static napi_value zlibInit(napi_env env, napi_callback_info info) {
  size_t argc = 7;
  napi_value argv[7], thisVal;
  napi_get_cb_info(env, info, &argc, argv, &thisVal, nullptr);

  auto *stream = ZlibStream::Unwrap(env, thisVal);
  if (!stream) {
    napi_throw_error(env, nullptr, "Invalid zlib stream");
    return nullptr;
  }

  uint32_t window_bits;
  napi_get_value_uint32(env, argv[0], &window_bits);
  int32_t level;
  napi_get_value_int32(env, argv[1], &level);
  uint32_t mem_level;
  napi_get_value_uint32(env, argv[2], &mem_level);
  uint32_t strategy;
  napi_get_value_uint32(env, argv[3], &strategy);

  // argv[4] = Uint32Array(2) for write results
  void *writeStateData = nullptr;
  size_t writeStateLen = 0;
  napi_typedarray_type taType;
  napi_value arrBuf;
  size_t byteOffset;
  napi_get_typedarray_info(
      env,
      argv[4],
      &taType,
      &writeStateLen,
      &writeStateData,
      &arrBuf,
      &byteOffset);
  uint32_t *write_result = static_cast<uint32_t *>(writeStateData);

  // argv[5] = processCallback
  // argv[6] = dictionary (Buffer or undefined)

  std::vector<unsigned char> dictionary;
  napi_valuetype dictType;
  napi_typeof(env, argv[6], &dictType);
  if (dictType != napi_undefined && dictType != napi_null) {
    const uint8_t *dictData = nullptr;
    size_t dictLen = 0;
    if (getArrayBufferViewBytes(env, argv[6], &dictData, &dictLen) &&
        dictData != nullptr && dictLen > 0) {
      dictionary.assign(dictData, dictData + dictLen);
    }
  }

  stream->InitStream(env, write_result, argv[5]);
  stream->context()->Init(
      level, window_bits, mem_level, strategy, std::move(dictionary));
  return nullptr;
}

// params(level, strategy)
static napi_value zlibParams(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2], thisVal;
  napi_get_cb_info(env, info, &argc, argv, &thisVal, nullptr);

  auto *stream = ZlibStream::Unwrap(env, thisVal);
  if (!stream)
    return nullptr;

  int32_t level, strategy;
  napi_get_value_int32(env, argv[0], &level);
  napi_get_value_int32(env, argv[1], &strategy);

  const CompressionError err = stream->context()->SetParams(level, strategy);
  if (err.IsError())
    stream->EmitError(env, thisVal, err);
  return nullptr;
}

// --- BrotliEncoder ---

using BrotliEncoderStream = CompressionStream<BrotliEncoderContext>;

static napi_value brotliEncoderNew(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1], thisVal;
  napi_get_cb_info(env, info, &argc, argv, &thisVal, nullptr);

  auto *stream = new BrotliEncoderStream();
  napi_wrap(
      env, thisVal, stream, BrotliEncoderStream::Destructor, nullptr, nullptr);

  int32_t mode;
  napi_get_value_int32(env, argv[0], &mode);
  stream->context()->SetMode(static_cast<node_zlib_mode>(mode));
  return thisVal;
}

// init(paramsUint32Array, writeState, callback)
static napi_value brotliEncoderInit(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3], thisVal;
  napi_get_cb_info(env, info, &argc, argv, &thisVal, nullptr);

  auto *stream = BrotliEncoderStream::Unwrap(env, thisVal);
  if (!stream) {
    napi_throw_error(env, nullptr, "Invalid brotli stream");
    return nullptr;
  }

  // argv[1] = Uint32Array(2) for write results
  void *writeStateData = nullptr;
  size_t writeStateLen = 0;
  napi_typedarray_type taType;
  napi_value arrBuf;
  size_t byteOffset;
  napi_get_typedarray_info(
      env,
      argv[1],
      &taType,
      &writeStateLen,
      &writeStateData,
      &arrBuf,
      &byteOffset);
  uint32_t *write_result = static_cast<uint32_t *>(writeStateData);

  stream->InitStream(env, write_result, argv[2]);

  CompressionError err = stream->context()->Init();
  if (err.IsError()) {
    stream->EmitError(env, thisVal, err);
    napi_throw_error(env, nullptr, "Initialization failed");
    return nullptr;
  }

  // Apply params from argv[0] (Uint32Array)
  void *paramsData = nullptr;
  size_t paramsLen = 0;
  napi_get_typedarray_info(
      env, argv[0], &taType, &paramsLen, &paramsData, &arrBuf, &byteOffset);
  const uint32_t *params = static_cast<const uint32_t *>(paramsData);

  for (size_t i = 0; i < paramsLen; i++) {
    if (params[i] == static_cast<uint32_t>(-1))
      continue;
    err = stream->context()->SetParams(static_cast<int>(i), params[i]);
    if (err.IsError()) {
      stream->EmitError(env, thisVal, err);
      napi_throw_error(env, nullptr, "Initialization failed");
      return nullptr;
    }
  }

  return nullptr;
}

// --- BrotliDecoder ---

using BrotliDecoderStream = CompressionStream<BrotliDecoderContext>;

static napi_value brotliDecoderNew(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1], thisVal;
  napi_get_cb_info(env, info, &argc, argv, &thisVal, nullptr);

  auto *stream = new BrotliDecoderStream();
  napi_wrap(
      env, thisVal, stream, BrotliDecoderStream::Destructor, nullptr, nullptr);

  int32_t mode;
  napi_get_value_int32(env, argv[0], &mode);
  stream->context()->SetMode(static_cast<node_zlib_mode>(mode));
  return thisVal;
}

// init(paramsUint32Array, writeState, callback)
static napi_value brotliDecoderInit(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3], thisVal;
  napi_get_cb_info(env, info, &argc, argv, &thisVal, nullptr);

  auto *stream = BrotliDecoderStream::Unwrap(env, thisVal);
  if (!stream) {
    napi_throw_error(env, nullptr, "Invalid brotli stream");
    return nullptr;
  }

  void *writeStateData = nullptr;
  size_t writeStateLen = 0;
  napi_typedarray_type taType;
  napi_value arrBuf;
  size_t byteOffset;
  napi_get_typedarray_info(
      env,
      argv[1],
      &taType,
      &writeStateLen,
      &writeStateData,
      &arrBuf,
      &byteOffset);
  uint32_t *write_result = static_cast<uint32_t *>(writeStateData);

  stream->InitStream(env, write_result, argv[2]);

  CompressionError err = stream->context()->Init();
  if (err.IsError()) {
    stream->EmitError(env, thisVal, err);
    napi_throw_error(env, nullptr, "Initialization failed");
    return nullptr;
  }

  void *paramsData = nullptr;
  size_t paramsLen = 0;
  napi_get_typedarray_info(
      env, argv[0], &taType, &paramsLen, &paramsData, &arrBuf, &byteOffset);
  const uint32_t *params = static_cast<const uint32_t *>(paramsData);

  for (size_t i = 0; i < paramsLen; i++) {
    if (params[i] == static_cast<uint32_t>(-1))
      continue;
    err = stream->context()->SetParams(static_cast<int>(i), params[i]);
    if (err.IsError()) {
      stream->EmitError(env, thisVal, err);
      napi_throw_error(env, nullptr, "Initialization failed");
      return nullptr;
    }
  }

  return nullptr;
}

// --- ZstdCompress ---

using ZstdCompressStream = CompressionStream<ZstdCompressContext>;

static napi_value zstdCompressNew(napi_env env, napi_callback_info info) {
  napi_value thisVal;
  napi_get_cb_info(env, info, nullptr, nullptr, &thisVal, nullptr);

  auto *stream = new ZstdCompressStream();
  napi_wrap(
      env, thisVal, stream, ZstdCompressStream::Destructor, nullptr, nullptr);
  return thisVal;
}

// init(params, pledgedSrcSize, writeState, callback[, dictionary])
static napi_value zstdCompressInit(napi_env env, napi_callback_info info) {
  size_t argc = 5;
  napi_value argv[5], thisVal;
  napi_get_cb_info(env, info, &argc, argv, &thisVal, nullptr);

  auto *stream = ZstdCompressStream::Unwrap(env, thisVal);
  if (!stream) {
    napi_throw_error(env, nullptr, "Invalid zstd stream");
    return nullptr;
  }

  // argv[2] = writeState Uint32Array(2)
  void *writeStateData = nullptr;
  size_t writeStateLen = 0;
  napi_typedarray_type taType;
  napi_value arrBuf;
  size_t byteOffset;
  napi_get_typedarray_info(
      env,
      argv[2],
      &taType,
      &writeStateLen,
      &writeStateData,
      &arrBuf,
      &byteOffset);
  uint32_t *write_result = static_cast<uint32_t *>(writeStateData);

  stream->InitStream(env, write_result, argv[3]);

  // Get pledgedSrcSize
  uint64_t pledged_src_size = ZSTD_CONTENTSIZE_UNKNOWN;
  napi_valuetype srcSizeType;
  napi_typeof(env, argv[1], &srcSizeType);
  if (srcSizeType == napi_number) {
    int64_t signed_pledged;
    napi_get_value_int64(env, argv[1], &signed_pledged);
    if (signed_pledged >= 0)
      pledged_src_size = static_cast<uint64_t>(signed_pledged);
  }

  // Get dictionary (optional argv[4])
  std::string_view dictionary;
  std::string dictBuf;
  if (argc >= 5) {
    napi_valuetype dictType;
    napi_typeof(env, argv[4], &dictType);
    if (dictType != napi_undefined) {
      const uint8_t *dictData = nullptr;
      size_t dictLen = 0;
      if (getArrayBufferViewBytes(env, argv[4], &dictData, &dictLen) &&
          dictData != nullptr && dictLen > 0) {
        dictBuf.assign(reinterpret_cast<const char *>(dictData), dictLen);
        dictionary = dictBuf;
      }
    }
  }

  CompressionError err = stream->context()->Init(pledged_src_size, dictionary);
  if (err.IsError()) {
    stream->EmitError(env, thisVal, err);
    napi_throw_error(env, nullptr, err.message);
    return nullptr;
  }

  // Apply params from argv[0]
  void *paramsData = nullptr;
  size_t paramsLen = 0;
  napi_get_typedarray_info(
      env, argv[0], &taType, &paramsLen, &paramsData, &arrBuf, &byteOffset);
  const uint32_t *params = static_cast<const uint32_t *>(paramsData);

  for (size_t i = 0; i < paramsLen; i++) {
    if (params[i] == static_cast<uint32_t>(-1))
      continue;
    err = stream->context()->SetParameter(
        static_cast<int>(i), static_cast<int>(params[i]));
    if (err.IsError()) {
      stream->EmitError(env, thisVal, err);
      napi_throw_error(env, nullptr, err.message);
      return nullptr;
    }
  }

  return nullptr;
}

// --- ZstdDecompress ---

using ZstdDecompressStream = CompressionStream<ZstdDecompressContext>;

static napi_value zstdDecompressNew(napi_env env, napi_callback_info info) {
  napi_value thisVal;
  napi_get_cb_info(env, info, nullptr, nullptr, &thisVal, nullptr);

  auto *stream = new ZstdDecompressStream();
  napi_wrap(
      env, thisVal, stream, ZstdDecompressStream::Destructor, nullptr, nullptr);
  return thisVal;
}

// init(params, pledgedSrcSize, writeState, callback[, dictionary])
static napi_value zstdDecompressInit(napi_env env, napi_callback_info info) {
  size_t argc = 5;
  napi_value argv[5], thisVal;
  napi_get_cb_info(env, info, &argc, argv, &thisVal, nullptr);

  auto *stream = ZstdDecompressStream::Unwrap(env, thisVal);
  if (!stream) {
    napi_throw_error(env, nullptr, "Invalid zstd stream");
    return nullptr;
  }

  void *writeStateData = nullptr;
  size_t writeStateLen = 0;
  napi_typedarray_type taType;
  napi_value arrBuf;
  size_t byteOffset;
  napi_get_typedarray_info(
      env,
      argv[2],
      &taType,
      &writeStateLen,
      &writeStateData,
      &arrBuf,
      &byteOffset);
  uint32_t *write_result = static_cast<uint32_t *>(writeStateData);

  stream->InitStream(env, write_result, argv[3]);

  uint64_t pledged_src_size = ZSTD_CONTENTSIZE_UNKNOWN;
  napi_valuetype srcSizeType;
  napi_typeof(env, argv[1], &srcSizeType);
  if (srcSizeType == napi_number) {
    int64_t signed_pledged;
    napi_get_value_int64(env, argv[1], &signed_pledged);
    if (signed_pledged >= 0)
      pledged_src_size = static_cast<uint64_t>(signed_pledged);
  }

  std::string_view dictionary;
  std::string dictBuf;
  if (argc >= 5) {
    napi_valuetype dictType;
    napi_typeof(env, argv[4], &dictType);
    if (dictType != napi_undefined) {
      const uint8_t *dictData = nullptr;
      size_t dictLen = 0;
      if (getArrayBufferViewBytes(env, argv[4], &dictData, &dictLen) &&
          dictData != nullptr && dictLen > 0) {
        dictBuf.assign(reinterpret_cast<const char *>(dictData), dictLen);
        dictionary = dictBuf;
      }
    }
  }

  CompressionError err = stream->context()->Init(pledged_src_size, dictionary);
  if (err.IsError()) {
    stream->EmitError(env, thisVal, err);
    napi_throw_error(env, nullptr, err.message);
    return nullptr;
  }

  void *paramsData = nullptr;
  size_t paramsLen = 0;
  napi_get_typedarray_info(
      env, argv[0], &taType, &paramsLen, &paramsData, &arrBuf, &byteOffset);
  const uint32_t *params = static_cast<const uint32_t *>(paramsData);

  for (size_t i = 0; i < paramsLen; i++) {
    if (params[i] == static_cast<uint32_t>(-1))
      continue;
    err = stream->context()->SetParameter(
        static_cast<int>(i), static_cast<int>(params[i]));
    if (err.IsError()) {
      stream->EmitError(env, thisVal, err);
      napi_throw_error(env, nullptr, err.message);
      return nullptr;
    }
  }

  return nullptr;
}

// No-op params for brotli/zstd.
static napi_value noopParams(napi_env, napi_callback_info) {
  return nullptr;
}

// -----------------------------------------------------------------------
// CRC32 standalone function
// -----------------------------------------------------------------------

static napi_value crc32Func(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

  uint32_t value = 0;
  if (argc >= 2)
    napi_get_value_uint32(env, argv[1], &value);

  // argv[0] can be a string or a Buffer/TypedArray
  napi_valuetype type;
  napi_typeof(env, argv[0], &type);

  uint32_t result;
  if (type == napi_string) {
    // Get string as UTF-8
    size_t len = 0;
    napi_get_value_string_utf8(env, argv[0], nullptr, 0, &len);
    std::string buf(len, '\0');
    napi_get_value_string_utf8(env, argv[0], &buf[0], len + 1, &len);
    result = static_cast<uint32_t>(
        crc32(value, reinterpret_cast<const Bytef *>(buf.data()), len));
  } else {
    const uint8_t *data = nullptr;
    size_t len = 0;
    if (!getArrayBufferViewBytes(env, argv[0], &data, &len)) {
      napi_throw_error(env, nullptr, "Invalid input buffer");
      return nullptr;
    }
    result = static_cast<uint32_t>(
        crc32(value, reinterpret_cast<const Bytef *>(data), len));
  }

  napi_value ret;
  napi_create_uint32(env, result, &ret);
  return ret;
}

// -----------------------------------------------------------------------
// Registration helper: define a compression class
// -----------------------------------------------------------------------

template <typename Stream>
static void defineCompressionClass(
    napi_env env,
    napi_value exports,
    const char *name,
    napi_callback newFn,
    napi_callback initFn,
    napi_callback paramsFn) {
  napi_property_descriptor protoProps[] = {
      {"write",
       nullptr,
       Stream::template Write<true>,
       nullptr,
       nullptr,
       nullptr,
       napi_enumerable,
       nullptr},
      {"writeSync",
       nullptr,
       Stream::template Write<false>,
       nullptr,
       nullptr,
       nullptr,
       napi_enumerable,
       nullptr},
      {"close",
       nullptr,
       Stream::DoClose,
       nullptr,
       nullptr,
       nullptr,
       napi_enumerable,
       nullptr},
      {"init",
       nullptr,
       initFn,
       nullptr,
       nullptr,
       nullptr,
       napi_enumerable,
       nullptr},
      {"params",
       nullptr,
       paramsFn,
       nullptr,
       nullptr,
       nullptr,
       napi_enumerable,
       nullptr},
      {"reset",
       nullptr,
       Stream::DoReset,
       nullptr,
       nullptr,
       nullptr,
       napi_enumerable,
       nullptr},
  };

  napi_value ctor;
  napi_define_class(
      env,
      name,
      NAPI_AUTO_LENGTH,
      newFn,
      nullptr,
      sizeof(protoProps) / sizeof(protoProps[0]),
      protoProps,
      &ctor);

  napi_set_named_property(env, exports, name, ctor);
}

} // anonymous namespace

// -----------------------------------------------------------------------
// initZlibBinding
// -----------------------------------------------------------------------

napi_value initZlibBinding(napi_env env, napi_value exports) {
  defineCompressionClass<ZlibStream>(
      env, exports, "Zlib", zlibNew, zlibInit, zlibParams);

  defineCompressionClass<BrotliEncoderStream>(
      env,
      exports,
      "BrotliEncoder",
      brotliEncoderNew,
      brotliEncoderInit,
      noopParams);

  defineCompressionClass<BrotliDecoderStream>(
      env,
      exports,
      "BrotliDecoder",
      brotliDecoderNew,
      brotliDecoderInit,
      noopParams);

  defineCompressionClass<ZstdCompressStream>(
      env,
      exports,
      "ZstdCompress",
      zstdCompressNew,
      zstdCompressInit,
      noopParams);

  defineCompressionClass<ZstdDecompressStream>(
      env,
      exports,
      "ZstdDecompress",
      zstdDecompressNew,
      zstdDecompressInit,
      noopParams);

  // crc32 function
  napi_value crc32Fn;
  napi_create_function(
      env, "crc32", NAPI_AUTO_LENGTH, crc32Func, nullptr, &crc32Fn);
  napi_set_named_property(env, exports, "crc32", crc32Fn);

  // ZLIB_VERSION string
  napi_value versionStr;
  napi_create_string_utf8(env, ZLIB_VERSION, NAPI_AUTO_LENGTH, &versionStr);
  napi_set_named_property(env, exports, "ZLIB_VERSION", versionStr);

  return exports;
}

} // namespace node_compat
} // namespace hermes
