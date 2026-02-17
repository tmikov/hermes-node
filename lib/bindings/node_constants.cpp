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
  // crypto, zlib, trace: empty stubs for now.

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
