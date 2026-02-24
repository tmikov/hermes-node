/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <hermes/node-compat/bindings/node_constants.h>
#include <node_api.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <cerrno>
#include <csignal>

#if !defined(_WIN32)
#include <dlfcn.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <uv.h>

#include <brotli/decode.h>
#include <brotli/encode.h>
#include <zlib.h>
#include <zstd.h>
#include <zstd_errors.h>

#include <cmath>
#include <limits>

namespace hermes {
namespace node_compat {

// Helper: set a named int32 property on an object.
static inline napi_status
setIntProp(napi_env env, napi_value obj, const char *name, int32_t val) {
  napi_value v;
  napi_status st = napi_create_int32(env, val, &v);
  if (st != napi_ok)
    return st;
  return napi_set_named_property(env, obj, name, v);
}

// Macro to define a constant from a C #define, guarded by #ifdef.
#define SET_CONST(obj, name)             \
  do {                                   \
    setIntProp(env, obj, #name, (name)); \
  } while (0)

static void defineErrnoConstants(napi_env env, napi_value target) {
#ifdef E2BIG
  SET_CONST(target, E2BIG);
#endif
#ifdef EACCES
  SET_CONST(target, EACCES);
#endif
#ifdef EADDRINUSE
  SET_CONST(target, EADDRINUSE);
#endif
#ifdef EADDRNOTAVAIL
  SET_CONST(target, EADDRNOTAVAIL);
#endif
#ifdef EAFNOSUPPORT
  SET_CONST(target, EAFNOSUPPORT);
#endif
#ifdef EAGAIN
  SET_CONST(target, EAGAIN);
#endif
#ifdef EALREADY
  SET_CONST(target, EALREADY);
#endif
#ifdef EBADF
  SET_CONST(target, EBADF);
#endif
#ifdef EBADMSG
  SET_CONST(target, EBADMSG);
#endif
#ifdef EBUSY
  SET_CONST(target, EBUSY);
#endif
#ifdef ECANCELED
  SET_CONST(target, ECANCELED);
#endif
#ifdef ECHILD
  SET_CONST(target, ECHILD);
#endif
#ifdef ECONNABORTED
  SET_CONST(target, ECONNABORTED);
#endif
#ifdef ECONNREFUSED
  SET_CONST(target, ECONNREFUSED);
#endif
#ifdef ECONNRESET
  SET_CONST(target, ECONNRESET);
#endif
#ifdef EDEADLK
  SET_CONST(target, EDEADLK);
#endif
#ifdef EDESTADDRREQ
  SET_CONST(target, EDESTADDRREQ);
#endif
#ifdef EDOM
  SET_CONST(target, EDOM);
#endif
#ifdef EDQUOT
  SET_CONST(target, EDQUOT);
#endif
#ifdef EEXIST
  SET_CONST(target, EEXIST);
#endif
#ifdef EFAULT
  SET_CONST(target, EFAULT);
#endif
#ifdef EFBIG
  SET_CONST(target, EFBIG);
#endif
#ifdef EHOSTUNREACH
  SET_CONST(target, EHOSTUNREACH);
#endif
#ifdef EIDRM
  SET_CONST(target, EIDRM);
#endif
#ifdef EILSEQ
  SET_CONST(target, EILSEQ);
#endif
#ifdef EINPROGRESS
  SET_CONST(target, EINPROGRESS);
#endif
#ifdef EINTR
  SET_CONST(target, EINTR);
#endif
#ifdef EINVAL
  SET_CONST(target, EINVAL);
#endif
#ifdef EIO
  SET_CONST(target, EIO);
#endif
#ifdef EISCONN
  SET_CONST(target, EISCONN);
#endif
#ifdef EISDIR
  SET_CONST(target, EISDIR);
#endif
#ifdef ELOOP
  SET_CONST(target, ELOOP);
#endif
#ifdef EMFILE
  SET_CONST(target, EMFILE);
#endif
#ifdef EMLINK
  SET_CONST(target, EMLINK);
#endif
#ifdef EMSGSIZE
  SET_CONST(target, EMSGSIZE);
#endif
#ifdef EMULTIHOP
  SET_CONST(target, EMULTIHOP);
#endif
#ifdef ENAMETOOLONG
  SET_CONST(target, ENAMETOOLONG);
#endif
#ifdef ENETDOWN
  SET_CONST(target, ENETDOWN);
#endif
#ifdef ENETRESET
  SET_CONST(target, ENETRESET);
#endif
#ifdef ENETUNREACH
  SET_CONST(target, ENETUNREACH);
#endif
#ifdef ENFILE
  SET_CONST(target, ENFILE);
#endif
#ifdef ENOBUFS
  SET_CONST(target, ENOBUFS);
#endif
#ifdef ENODATA
  SET_CONST(target, ENODATA);
#endif
#ifdef ENODEV
  SET_CONST(target, ENODEV);
#endif
#ifdef ENOENT
  SET_CONST(target, ENOENT);
#endif
#ifdef ENOEXEC
  SET_CONST(target, ENOEXEC);
#endif
#ifdef ENOLCK
  SET_CONST(target, ENOLCK);
#endif
#ifdef ENOLINK
  SET_CONST(target, ENOLINK);
#endif
#ifdef ENOMEM
  SET_CONST(target, ENOMEM);
#endif
#ifdef ENOMSG
  SET_CONST(target, ENOMSG);
#endif
#ifdef ENOPROTOOPT
  SET_CONST(target, ENOPROTOOPT);
#endif
#ifdef ENOSPC
  SET_CONST(target, ENOSPC);
#endif
#ifdef ENOSR
  SET_CONST(target, ENOSR);
#endif
#ifdef ENOSTR
  SET_CONST(target, ENOSTR);
#endif
#ifdef ENOSYS
  SET_CONST(target, ENOSYS);
#endif
#ifdef ENOTCONN
  SET_CONST(target, ENOTCONN);
#endif
#ifdef ENOTDIR
  SET_CONST(target, ENOTDIR);
#endif
#ifdef ENOTEMPTY
  SET_CONST(target, ENOTEMPTY);
#endif
#ifdef ENOTSOCK
  SET_CONST(target, ENOTSOCK);
#endif
#ifdef ENOTSUP
  SET_CONST(target, ENOTSUP);
#endif
#ifdef ENOTTY
  SET_CONST(target, ENOTTY);
#endif
#ifdef ENXIO
  SET_CONST(target, ENXIO);
#endif
#ifdef EOPNOTSUPP
  SET_CONST(target, EOPNOTSUPP);
#endif
#ifdef EOVERFLOW
  SET_CONST(target, EOVERFLOW);
#endif
#ifdef EPERM
  SET_CONST(target, EPERM);
#endif
#ifdef EPIPE
  SET_CONST(target, EPIPE);
#endif
#ifdef EPROTO
  SET_CONST(target, EPROTO);
#endif
#ifdef EPROTONOSUPPORT
  SET_CONST(target, EPROTONOSUPPORT);
#endif
#ifdef EPROTOTYPE
  SET_CONST(target, EPROTOTYPE);
#endif
#ifdef ERANGE
  SET_CONST(target, ERANGE);
#endif
#ifdef EROFS
  SET_CONST(target, EROFS);
#endif
#ifdef ESPIPE
  SET_CONST(target, ESPIPE);
#endif
#ifdef ESRCH
  SET_CONST(target, ESRCH);
#endif
#ifdef ESTALE
  SET_CONST(target, ESTALE);
#endif
#ifdef ETIME
  SET_CONST(target, ETIME);
#endif
#ifdef ETIMEDOUT
  SET_CONST(target, ETIMEDOUT);
#endif
#ifdef ETXTBSY
  SET_CONST(target, ETXTBSY);
#endif
#ifdef EWOULDBLOCK
  SET_CONST(target, EWOULDBLOCK);
#endif
#ifdef EXDEV
  SET_CONST(target, EXDEV);
#endif
}

static void defineSignalConstants(napi_env env, napi_value target) {
#ifdef SIGHUP
  SET_CONST(target, SIGHUP);
#endif
#ifdef SIGINT
  SET_CONST(target, SIGINT);
#endif
#ifdef SIGQUIT
  SET_CONST(target, SIGQUIT);
#endif
#ifdef SIGILL
  SET_CONST(target, SIGILL);
#endif
#ifdef SIGTRAP
  SET_CONST(target, SIGTRAP);
#endif
#ifdef SIGABRT
  SET_CONST(target, SIGABRT);
#endif
#ifdef SIGIOT
  SET_CONST(target, SIGIOT);
#endif
#ifdef SIGBUS
  SET_CONST(target, SIGBUS);
#endif
#ifdef SIGFPE
  SET_CONST(target, SIGFPE);
#endif
#ifdef SIGKILL
  SET_CONST(target, SIGKILL);
#endif
#ifdef SIGUSR1
  SET_CONST(target, SIGUSR1);
#endif
#ifdef SIGSEGV
  SET_CONST(target, SIGSEGV);
#endif
#ifdef SIGUSR2
  SET_CONST(target, SIGUSR2);
#endif
#ifdef SIGPIPE
  SET_CONST(target, SIGPIPE);
#endif
#ifdef SIGALRM
  SET_CONST(target, SIGALRM);
#endif
  SET_CONST(target, SIGTERM);
#ifdef SIGCHLD
  SET_CONST(target, SIGCHLD);
#endif
#ifdef SIGSTKFLT
  SET_CONST(target, SIGSTKFLT);
#endif
#ifdef SIGCONT
  SET_CONST(target, SIGCONT);
#endif
#ifdef SIGSTOP
  SET_CONST(target, SIGSTOP);
#endif
#ifdef SIGTSTP
  SET_CONST(target, SIGTSTP);
#endif
#ifdef SIGBREAK
  SET_CONST(target, SIGBREAK);
#endif
#ifdef SIGTTIN
  SET_CONST(target, SIGTTIN);
#endif
#ifdef SIGTTOU
  SET_CONST(target, SIGTTOU);
#endif
#ifdef SIGURG
  SET_CONST(target, SIGURG);
#endif
#ifdef SIGXCPU
  SET_CONST(target, SIGXCPU);
#endif
#ifdef SIGXFSZ
  SET_CONST(target, SIGXFSZ);
#endif
#ifdef SIGVTALRM
  SET_CONST(target, SIGVTALRM);
#endif
#ifdef SIGPROF
  SET_CONST(target, SIGPROF);
#endif
#ifdef SIGWINCH
  SET_CONST(target, SIGWINCH);
#endif
#ifdef SIGIO
  SET_CONST(target, SIGIO);
#endif
#ifdef SIGPOLL
  SET_CONST(target, SIGPOLL);
#endif
#ifdef SIGLOST
  SET_CONST(target, SIGLOST);
#endif
#ifdef SIGPWR
  SET_CONST(target, SIGPWR);
#endif
#ifdef SIGINFO
  SET_CONST(target, SIGINFO);
#endif
#ifdef SIGSYS
  SET_CONST(target, SIGSYS);
#endif
#ifdef SIGUNUSED
  SET_CONST(target, SIGUNUSED);
#endif
}

static void definePriorityConstants(napi_env env, napi_value target) {
#ifdef UV_PRIORITY_LOW
  setIntProp(env, target, "PRIORITY_LOW", UV_PRIORITY_LOW);
#endif
#ifdef UV_PRIORITY_BELOW_NORMAL
  setIntProp(env, target, "PRIORITY_BELOW_NORMAL", UV_PRIORITY_BELOW_NORMAL);
#endif
#ifdef UV_PRIORITY_NORMAL
  setIntProp(env, target, "PRIORITY_NORMAL", UV_PRIORITY_NORMAL);
#endif
#ifdef UV_PRIORITY_ABOVE_NORMAL
  setIntProp(env, target, "PRIORITY_ABOVE_NORMAL", UV_PRIORITY_ABOVE_NORMAL);
#endif
#ifdef UV_PRIORITY_HIGH
  setIntProp(env, target, "PRIORITY_HIGH", UV_PRIORITY_HIGH);
#endif
#ifdef UV_PRIORITY_HIGHEST
  setIntProp(env, target, "PRIORITY_HIGHEST", UV_PRIORITY_HIGHEST);
#endif
}

static void defineFsConstants(napi_env env, napi_value target) {
  SET_CONST(target, UV_FS_SYMLINK_DIR);
  SET_CONST(target, UV_FS_SYMLINK_JUNCTION);

  // File access modes.
  SET_CONST(target, O_RDONLY);
  SET_CONST(target, O_WRONLY);
  SET_CONST(target, O_RDWR);

  // File types from readdir.
  SET_CONST(target, UV_DIRENT_UNKNOWN);
  SET_CONST(target, UV_DIRENT_FILE);
  SET_CONST(target, UV_DIRENT_DIR);
  SET_CONST(target, UV_DIRENT_LINK);
  SET_CONST(target, UV_DIRENT_FIFO);
  SET_CONST(target, UV_DIRENT_SOCKET);
  SET_CONST(target, UV_DIRENT_CHAR);
  SET_CONST(target, UV_DIRENT_BLOCK);

  // File type flags.
  SET_CONST(target, S_IFMT);
  SET_CONST(target, S_IFREG);
  SET_CONST(target, S_IFDIR);
  SET_CONST(target, S_IFCHR);
#ifdef S_IFBLK
  SET_CONST(target, S_IFBLK);
#endif
#ifdef S_IFIFO
  SET_CONST(target, S_IFIFO);
#endif
#ifdef S_IFLNK
  SET_CONST(target, S_IFLNK);
#endif
#ifdef S_IFSOCK
  SET_CONST(target, S_IFSOCK);
#endif

  // File open flags.
#ifdef O_CREAT
  SET_CONST(target, O_CREAT);
#endif
#ifdef O_EXCL
  SET_CONST(target, O_EXCL);
#endif
  SET_CONST(target, UV_FS_O_FILEMAP);
#ifdef O_NOCTTY
  SET_CONST(target, O_NOCTTY);
#endif
#ifdef O_TRUNC
  SET_CONST(target, O_TRUNC);
#endif
#ifdef O_APPEND
  SET_CONST(target, O_APPEND);
#endif
#ifdef O_DIRECTORY
  SET_CONST(target, O_DIRECTORY);
#endif
#ifdef O_NOATIME
  SET_CONST(target, O_NOATIME);
#endif
#ifdef O_NOFOLLOW
  SET_CONST(target, O_NOFOLLOW);
#endif
#ifdef O_SYNC
  SET_CONST(target, O_SYNC);
#endif
#ifdef O_DSYNC
  SET_CONST(target, O_DSYNC);
#endif
#ifdef O_SYMLINK
  SET_CONST(target, O_SYMLINK);
#endif
#ifdef O_DIRECT
  SET_CONST(target, O_DIRECT);
#endif
#ifdef O_NONBLOCK
  SET_CONST(target, O_NONBLOCK);
#endif

  // Permission bits.
#ifdef S_IRWXU
  SET_CONST(target, S_IRWXU);
#endif
#ifdef S_IRUSR
  SET_CONST(target, S_IRUSR);
#endif
#ifdef S_IWUSR
  SET_CONST(target, S_IWUSR);
#endif
#ifdef S_IXUSR
  SET_CONST(target, S_IXUSR);
#endif
#ifdef S_IRWXG
  SET_CONST(target, S_IRWXG);
#endif
#ifdef S_IRGRP
  SET_CONST(target, S_IRGRP);
#endif
#ifdef S_IWGRP
  SET_CONST(target, S_IWGRP);
#endif
#ifdef S_IXGRP
  SET_CONST(target, S_IXGRP);
#endif
#ifdef S_IRWXO
  SET_CONST(target, S_IRWXO);
#endif
#ifdef S_IROTH
  SET_CONST(target, S_IROTH);
#endif
#ifdef S_IWOTH
  SET_CONST(target, S_IWOTH);
#endif
#ifdef S_IXOTH
  SET_CONST(target, S_IXOTH);
#endif

  // Access mode flags.
#ifdef F_OK
  SET_CONST(target, F_OK);
#endif
#ifdef R_OK
  SET_CONST(target, R_OK);
#endif
#ifdef W_OK
  SET_CONST(target, W_OK);
#endif
#ifdef X_OK
  SET_CONST(target, X_OK);
#endif

  // Copy file flags.
#ifdef UV_FS_COPYFILE_EXCL
  SET_CONST(target, UV_FS_COPYFILE_EXCL);
  setIntProp(env, target, "COPYFILE_EXCL", UV_FS_COPYFILE_EXCL);
#endif
#ifdef UV_FS_COPYFILE_FICLONE
  SET_CONST(target, UV_FS_COPYFILE_FICLONE);
  setIntProp(env, target, "COPYFILE_FICLONE", UV_FS_COPYFILE_FICLONE);
#endif
#ifdef UV_FS_COPYFILE_FICLONE_FORCE
  SET_CONST(target, UV_FS_COPYFILE_FICLONE_FORCE);
  setIntProp(
      env, target, "COPYFILE_FICLONE_FORCE", UV_FS_COPYFILE_FICLONE_FORCE);
#endif
}

static void defineDlopenConstants(napi_env env, napi_value target) {
#ifdef RTLD_LAZY
  SET_CONST(target, RTLD_LAZY);
#endif
#ifdef RTLD_NOW
  SET_CONST(target, RTLD_NOW);
#endif
#ifdef RTLD_GLOBAL
  SET_CONST(target, RTLD_GLOBAL);
#endif
#ifdef RTLD_LOCAL
  SET_CONST(target, RTLD_LOCAL);
#endif
#ifdef RTLD_DEEPBIND
  SET_CONST(target, RTLD_DEEPBIND);
#endif
}

static inline napi_status
setDoubleProp(napi_env env, napi_value obj, const char *name, double val) {
  napi_value v;
  napi_status st = napi_create_double(env, val, &v);
  if (st != napi_ok)
    return st;
  return napi_set_named_property(env, obj, name, v);
}

// Node.js compression mode enum (from node_zlib.cc).
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

// Range constants from node_zlib.cc (not in any header).
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

static void defineZlibConstants(napi_env env, napi_value target) {
  // Flush levels
  SET_CONST(target, Z_NO_FLUSH);
  SET_CONST(target, Z_PARTIAL_FLUSH);
  SET_CONST(target, Z_SYNC_FLUSH);
  SET_CONST(target, Z_FULL_FLUSH);
  SET_CONST(target, Z_FINISH);
  SET_CONST(target, Z_BLOCK);

  // Return/error codes
  SET_CONST(target, Z_OK);
  SET_CONST(target, Z_STREAM_END);
  SET_CONST(target, Z_NEED_DICT);
  SET_CONST(target, Z_ERRNO);
  SET_CONST(target, Z_STREAM_ERROR);
  SET_CONST(target, Z_DATA_ERROR);
  SET_CONST(target, Z_MEM_ERROR);
  SET_CONST(target, Z_BUF_ERROR);
  SET_CONST(target, Z_VERSION_ERROR);

  // Compression strategy
  SET_CONST(target, Z_NO_COMPRESSION);
  SET_CONST(target, Z_BEST_SPEED);
  SET_CONST(target, Z_BEST_COMPRESSION);
  SET_CONST(target, Z_DEFAULT_COMPRESSION);
  SET_CONST(target, Z_FILTERED);
  SET_CONST(target, Z_HUFFMAN_ONLY);
  SET_CONST(target, Z_RLE);
  SET_CONST(target, Z_FIXED);
  SET_CONST(target, Z_DEFAULT_STRATEGY);
  SET_CONST(target, ZLIB_VERNUM);

  // Node mode enum
  SET_CONST(target, DEFLATE);
  SET_CONST(target, INFLATE);
  SET_CONST(target, GZIP);
  SET_CONST(target, GUNZIP);
  SET_CONST(target, DEFLATERAW);
  SET_CONST(target, INFLATERAW);
  SET_CONST(target, UNZIP);
  SET_CONST(target, BROTLI_DECODE);
  SET_CONST(target, BROTLI_ENCODE);
  SET_CONST(target, ZSTD_COMPRESS);
  SET_CONST(target, ZSTD_DECOMPRESS);

  // Range constants
  SET_CONST(target, Z_MIN_WINDOWBITS);
  SET_CONST(target, Z_MAX_WINDOWBITS);
  SET_CONST(target, Z_DEFAULT_WINDOWBITS);
  SET_CONST(target, Z_MIN_CHUNK);
  setDoubleProp(
      env, target, "Z_MAX_CHUNK", std::numeric_limits<double>::infinity());
  SET_CONST(target, Z_DEFAULT_CHUNK);
  SET_CONST(target, Z_MIN_MEMLEVEL);
  SET_CONST(target, Z_MAX_MEMLEVEL);
  SET_CONST(target, Z_DEFAULT_MEMLEVEL);
  SET_CONST(target, Z_MIN_LEVEL);
  SET_CONST(target, Z_MAX_LEVEL);
  SET_CONST(target, Z_DEFAULT_LEVEL);

  // Brotli constants
  SET_CONST(target, BROTLI_OPERATION_PROCESS);
  SET_CONST(target, BROTLI_OPERATION_FLUSH);
  SET_CONST(target, BROTLI_OPERATION_FINISH);
  SET_CONST(target, BROTLI_OPERATION_EMIT_METADATA);
  SET_CONST(target, BROTLI_PARAM_MODE);
  SET_CONST(target, BROTLI_MODE_GENERIC);
  SET_CONST(target, BROTLI_MODE_TEXT);
  SET_CONST(target, BROTLI_MODE_FONT);
  SET_CONST(target, BROTLI_DEFAULT_MODE);
  SET_CONST(target, BROTLI_PARAM_QUALITY);
  SET_CONST(target, BROTLI_MIN_QUALITY);
  SET_CONST(target, BROTLI_MAX_QUALITY);
  SET_CONST(target, BROTLI_DEFAULT_QUALITY);
  SET_CONST(target, BROTLI_PARAM_LGWIN);
  SET_CONST(target, BROTLI_MIN_WINDOW_BITS);
  SET_CONST(target, BROTLI_MAX_WINDOW_BITS);
  SET_CONST(target, BROTLI_LARGE_MAX_WINDOW_BITS);
  SET_CONST(target, BROTLI_DEFAULT_WINDOW);
  SET_CONST(target, BROTLI_PARAM_LGBLOCK);
  SET_CONST(target, BROTLI_MIN_INPUT_BLOCK_BITS);
  SET_CONST(target, BROTLI_MAX_INPUT_BLOCK_BITS);
  SET_CONST(target, BROTLI_PARAM_DISABLE_LITERAL_CONTEXT_MODELING);
  SET_CONST(target, BROTLI_PARAM_SIZE_HINT);
  SET_CONST(target, BROTLI_PARAM_LARGE_WINDOW);
  SET_CONST(target, BROTLI_PARAM_NPOSTFIX);
  SET_CONST(target, BROTLI_PARAM_NDIRECT);
  SET_CONST(target, BROTLI_DECODER_RESULT_ERROR);
  SET_CONST(target, BROTLI_DECODER_RESULT_SUCCESS);
  SET_CONST(target, BROTLI_DECODER_RESULT_NEEDS_MORE_INPUT);
  SET_CONST(target, BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT);
  SET_CONST(target, BROTLI_DECODER_PARAM_DISABLE_RING_BUFFER_REALLOCATION);
  SET_CONST(target, BROTLI_DECODER_PARAM_LARGE_WINDOW);
  SET_CONST(target, BROTLI_DECODER_NO_ERROR);
  SET_CONST(target, BROTLI_DECODER_SUCCESS);
  SET_CONST(target, BROTLI_DECODER_NEEDS_MORE_INPUT);
  SET_CONST(target, BROTLI_DECODER_NEEDS_MORE_OUTPUT);
  SET_CONST(target, BROTLI_DECODER_ERROR_FORMAT_EXUBERANT_NIBBLE);
  SET_CONST(target, BROTLI_DECODER_ERROR_FORMAT_RESERVED);
  SET_CONST(target, BROTLI_DECODER_ERROR_FORMAT_EXUBERANT_META_NIBBLE);
  SET_CONST(target, BROTLI_DECODER_ERROR_FORMAT_SIMPLE_HUFFMAN_ALPHABET);
  SET_CONST(target, BROTLI_DECODER_ERROR_FORMAT_SIMPLE_HUFFMAN_SAME);
  SET_CONST(target, BROTLI_DECODER_ERROR_FORMAT_CL_SPACE);
  SET_CONST(target, BROTLI_DECODER_ERROR_FORMAT_HUFFMAN_SPACE);
  SET_CONST(target, BROTLI_DECODER_ERROR_FORMAT_CONTEXT_MAP_REPEAT);
  SET_CONST(target, BROTLI_DECODER_ERROR_FORMAT_BLOCK_LENGTH_1);
  SET_CONST(target, BROTLI_DECODER_ERROR_FORMAT_BLOCK_LENGTH_2);
  SET_CONST(target, BROTLI_DECODER_ERROR_FORMAT_TRANSFORM);
  SET_CONST(target, BROTLI_DECODER_ERROR_FORMAT_DICTIONARY);
  SET_CONST(target, BROTLI_DECODER_ERROR_FORMAT_WINDOW_BITS);
  SET_CONST(target, BROTLI_DECODER_ERROR_FORMAT_PADDING_1);
  SET_CONST(target, BROTLI_DECODER_ERROR_FORMAT_PADDING_2);
  SET_CONST(target, BROTLI_DECODER_ERROR_FORMAT_DISTANCE);
  SET_CONST(target, BROTLI_DECODER_ERROR_DICTIONARY_NOT_SET);
  SET_CONST(target, BROTLI_DECODER_ERROR_INVALID_ARGUMENTS);
  SET_CONST(target, BROTLI_DECODER_ERROR_ALLOC_CONTEXT_MODES);
  SET_CONST(target, BROTLI_DECODER_ERROR_ALLOC_TREE_GROUPS);
  SET_CONST(target, BROTLI_DECODER_ERROR_ALLOC_CONTEXT_MAP);
  SET_CONST(target, BROTLI_DECODER_ERROR_ALLOC_RING_BUFFER_1);
  SET_CONST(target, BROTLI_DECODER_ERROR_ALLOC_RING_BUFFER_2);
  SET_CONST(target, BROTLI_DECODER_ERROR_ALLOC_BLOCK_TYPE_TREES);
  SET_CONST(target, BROTLI_DECODER_ERROR_UNREACHABLE);

  // Zstd constants
  SET_CONST(target, ZSTD_e_continue);
  SET_CONST(target, ZSTD_e_flush);
  SET_CONST(target, ZSTD_e_end);
  SET_CONST(target, ZSTD_fast);
  SET_CONST(target, ZSTD_dfast);
  SET_CONST(target, ZSTD_greedy);
  SET_CONST(target, ZSTD_lazy);
  SET_CONST(target, ZSTD_lazy2);
  SET_CONST(target, ZSTD_btlazy2);
  SET_CONST(target, ZSTD_btopt);
  SET_CONST(target, ZSTD_btultra);
  SET_CONST(target, ZSTD_btultra2);
  SET_CONST(target, ZSTD_c_compressionLevel);
  SET_CONST(target, ZSTD_c_windowLog);
  SET_CONST(target, ZSTD_c_hashLog);
  SET_CONST(target, ZSTD_c_chainLog);
  SET_CONST(target, ZSTD_c_searchLog);
  SET_CONST(target, ZSTD_c_minMatch);
  SET_CONST(target, ZSTD_c_targetLength);
  SET_CONST(target, ZSTD_c_strategy);
  SET_CONST(target, ZSTD_c_enableLongDistanceMatching);
  SET_CONST(target, ZSTD_c_ldmHashLog);
  SET_CONST(target, ZSTD_c_ldmMinMatch);
  SET_CONST(target, ZSTD_c_ldmBucketSizeLog);
  SET_CONST(target, ZSTD_c_ldmHashRateLog);
  SET_CONST(target, ZSTD_c_contentSizeFlag);
  SET_CONST(target, ZSTD_c_checksumFlag);
  SET_CONST(target, ZSTD_c_dictIDFlag);
  SET_CONST(target, ZSTD_c_nbWorkers);
  SET_CONST(target, ZSTD_c_jobSize);
  SET_CONST(target, ZSTD_c_overlapLog);
  SET_CONST(target, ZSTD_d_windowLogMax);
  SET_CONST(target, ZSTD_CLEVEL_DEFAULT);
  // Zstd error codes
  SET_CONST(target, ZSTD_error_no_error);
  SET_CONST(target, ZSTD_error_GENERIC);
  SET_CONST(target, ZSTD_error_prefix_unknown);
  SET_CONST(target, ZSTD_error_version_unsupported);
  SET_CONST(target, ZSTD_error_frameParameter_unsupported);
  SET_CONST(target, ZSTD_error_frameParameter_windowTooLarge);
  SET_CONST(target, ZSTD_error_corruption_detected);
  SET_CONST(target, ZSTD_error_checksum_wrong);
  SET_CONST(target, ZSTD_error_literals_headerWrong);
  SET_CONST(target, ZSTD_error_dictionary_corrupted);
  SET_CONST(target, ZSTD_error_dictionary_wrong);
  SET_CONST(target, ZSTD_error_dictionaryCreation_failed);
  SET_CONST(target, ZSTD_error_parameter_unsupported);
  SET_CONST(target, ZSTD_error_parameter_combination_unsupported);
  SET_CONST(target, ZSTD_error_parameter_outOfBound);
  SET_CONST(target, ZSTD_error_tableLog_tooLarge);
  SET_CONST(target, ZSTD_error_maxSymbolValue_tooLarge);
  SET_CONST(target, ZSTD_error_maxSymbolValue_tooSmall);
  SET_CONST(target, ZSTD_error_stabilityCondition_notRespected);
  SET_CONST(target, ZSTD_error_stage_wrong);
  SET_CONST(target, ZSTD_error_init_missing);
  SET_CONST(target, ZSTD_error_memory_allocation);
  SET_CONST(target, ZSTD_error_workSpace_tooSmall);
  SET_CONST(target, ZSTD_error_dstSize_tooSmall);
  SET_CONST(target, ZSTD_error_srcSize_wrong);
  SET_CONST(target, ZSTD_error_dstBuffer_null);
  SET_CONST(target, ZSTD_error_noForwardProgress_destFull);
  SET_CONST(target, ZSTD_error_noForwardProgress_inputEmpty);
}

napi_value initConstantsBinding(napi_env env, napi_value exports) {
  napi_value osObj, errnoObj, signalsObj, priorityObj;
  napi_value fsObj, cryptoObj, zlibObj, traceObj;

  napi_create_object(env, &osObj);
  napi_create_object(env, &errnoObj);
  napi_create_object(env, &signalsObj);
  napi_create_object(env, &priorityObj);
  napi_create_object(env, &fsObj);
  napi_create_object(env, &cryptoObj);
  napi_create_object(env, &zlibObj);
  napi_create_object(env, &traceObj);

  defineErrnoConstants(env, errnoObj);
  defineSignalConstants(env, signalsObj);
  definePriorityConstants(env, priorityObj);
  defineFsConstants(env, fsObj);
  defineZlibConstants(env, zlibObj);
  // crypto, trace: empty stubs for now.

  // dlopen constants go under os.dlopen.
  napi_value dlopenObj;
  napi_create_object(env, &dlopenObj);
  defineDlopenConstants(env, dlopenObj);

  // Libuv UDP constants on os object.
  SET_CONST(osObj, UV_UDP_REUSEADDR);
  SET_CONST(osObj, UV_UDP_IPV6ONLY);
  SET_CONST(osObj, UV_UDP_PARTIAL);
#ifdef UV_UDP_REUSEPORT
  SET_CONST(osObj, UV_UDP_REUSEPORT);
#endif

  // Socket type constants.
#ifdef SOCK_STREAM
  SET_CONST(osObj, SOCK_STREAM);
#endif
#ifdef SOCK_DGRAM
  SET_CONST(osObj, SOCK_DGRAM);
#endif
#ifdef SOCK_RAW
  SET_CONST(osObj, SOCK_RAW);
#endif
#ifdef SOCK_SEQPACKET
  SET_CONST(osObj, SOCK_SEQPACKET);
#endif
#ifdef SOCK_RDM
  SET_CONST(osObj, SOCK_RDM);
#endif

  // Assemble os sub-objects.
  napi_set_named_property(env, osObj, "errno", errnoObj);
  napi_set_named_property(env, osObj, "signals", signalsObj);
  napi_set_named_property(env, osObj, "priority", priorityObj);
  napi_set_named_property(env, osObj, "dlopen", dlopenObj);

  // Set top-level categories on exports.
  napi_set_named_property(env, exports, "os", osObj);
  napi_set_named_property(env, exports, "fs", fsObj);
  napi_set_named_property(env, exports, "crypto", cryptoObj);
  napi_set_named_property(env, exports, "zlib", zlibObj);
  napi_set_named_property(env, exports, "trace", traceObj);

  return exports;
}

#undef SET_CONST

} // namespace node_compat
} // namespace hermes
