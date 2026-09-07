/*
 * Copyright (c) Tzvetan Mikov.
 *
 * Client TLS filter: OpenSSL SSL + memory BIOs.
 * JS feeds ciphertext with push(), writes plaintext with write().
 * Encrypted bytes come back through onencrypted. Enough for
 * tls.connect / https.get / node-fetch HTTPS. Not Node's TLSWrap.
 */

#include <hermes/node-compat/bindings/node_tls_wrap.h>
#include <hermes/node-compat/bindings/node_errors.h>
#include <node_api.h>
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include <cstring>
#include <string>

namespace hermes {
namespace node_compat {

namespace {

SSL_CTX *gClientCtx = nullptr;

SSL_CTX *clientCtx() {
  if (gClientCtx)
    return gClientCtx;
  SSL_load_error_strings();
  OpenSSL_add_ssl_algorithms();
  gClientCtx = SSL_CTX_new(TLS_client_method());
  if (!gClientCtx)
    return nullptr;
  SSL_CTX_set_default_verify_paths(gClientCtx);
  SSL_CTX_set_verify(gClientCtx, SSL_VERIFY_PEER, nullptr);
  SSL_CTX_set_min_proto_version(gClientCtx, TLS1_2_VERSION);
  return gClientCtx;
}

std::string opensslError() {
  char buf[256];
  unsigned long err = ERR_get_error();
  if (!err)
    return "TLS error";
  ERR_error_string_n(err, buf, sizeof(buf));
  return buf;
}

struct TlsSession {
  napi_env env = nullptr;
  napi_ref jsRef = nullptr;
  SSL *ssl = nullptr;
  BIO *rbio = nullptr;
  BIO *wbio = nullptr;
  bool handshakeDone = false;
  bool closed = false;
};

void freeSession(napi_env, void *data, void *) {
  auto *s = static_cast<TlsSession *>(data);
  if (s->ssl)
    SSL_free(s->ssl);
  if (s->jsRef)
    napi_delete_reference(s->env, s->jsRef);
  delete s;
}

napi_value jsThis(TlsSession *s) {
  napi_value obj;
  if (napi_get_reference_value(s->env, s->jsRef, &obj) != napi_ok)
    return nullptr;
  return obj;
}

void callNamed(TlsSession *s, const char *name, napi_value *argv, size_t argc) {
  napi_handle_scope scope;
  napi_open_handle_scope(s->env, &scope);
  napi_value thisObj = jsThis(s);
  if (!thisObj) {
    napi_close_handle_scope(s->env, scope);
    return;
  }
  napi_value cb;
  napi_get_named_property(s->env, thisObj, name, &cb);
  napi_valuetype t;
  napi_typeof(s->env, cb, &t);
  if (t == napi_function) {
    napi_value retval;
    napi_call_function(s->env, thisObj, cb, argc, argv, &retval);
    handleCallbackException(s->env);
  }
  napi_close_handle_scope(s->env, scope);
}

void emitError(TlsSession *s, const char *msg) {
  napi_handle_scope scope;
  napi_open_handle_scope(s->env, &scope);
  napi_value argv[1];
  napi_create_string_utf8(s->env, msg, NAPI_AUTO_LENGTH, &argv[0]);
  callNamed(s, "onerror", argv, 1);
  napi_close_handle_scope(s->env, scope);
}

void emitBuf(TlsSession *s, const char *name, const char *data, size_t len) {
  napi_handle_scope scope;
  napi_open_handle_scope(s->env, &scope);
  napi_value buf;
  void *copy = nullptr;
  napi_create_buffer_copy(s->env, len, data, &copy, &buf);
  napi_value argv[1] = {buf};
  callNamed(s, name, argv, 1);
  napi_close_handle_scope(s->env, scope);
}

void flushWriteBio(TlsSession *s) {
  char buf[16384];
  for (;;) {
    int n = BIO_read(s->wbio, buf, sizeof(buf));
    if (n <= 0)
      break;
    emitBuf(s, "onencrypted", buf, static_cast<size_t>(n));
  }
}

void drainApp(TlsSession *s) {
  char buf[16384];
  for (;;) {
    int n = SSL_read(s->ssl, buf, sizeof(buf));
    if (n > 0) {
      emitBuf(s, "onplaintext", buf, static_cast<size_t>(n));
      continue;
    }
    int err = SSL_get_error(s->ssl, n);
    if (err == SSL_ERROR_ZERO_RETURN) {
      callNamed(s, "onclose", nullptr, 0);
      s->closed = true;
      return;
    }
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE)
      return;
    emitError(s, opensslError().c_str());
    return;
  }
}

void pump(TlsSession *s) {
  if (!s->ssl || s->closed)
    return;
  if (!s->handshakeDone) {
    int rc = SSL_connect(s->ssl);
    flushWriteBio(s);
    if (rc == 1) {
      s->handshakeDone = true;
      callNamed(s, "onsecureconnect", nullptr, 0);
      drainApp(s);
    } else {
      int err = SSL_get_error(s->ssl, rc);
      if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE)
        emitError(s, opensslError().c_str());
    }
    return;
  }
  drainApp(s);
  flushWriteBio(s);
}

TlsSession *unwrap(napi_env env, napi_value obj) {
  void *data = nullptr;
  if (napi_unwrap(env, obj, &data) != napi_ok)
    return nullptr;
  return static_cast<TlsSession *>(data);
}

napi_value tlsNew(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_value thisObj;
  napi_get_cb_info(env, info, &argc, argv, &thisObj, nullptr);

  SSL_CTX *ctx = clientCtx();
  if (!ctx) {
    napi_throw_error(env, nullptr, "failed to create TLS context");
    return nullptr;
  }

  auto *s = new TlsSession();
  s->env = env;
  s->ssl = SSL_new(ctx);
  s->rbio = BIO_new(BIO_s_mem());
  s->wbio = BIO_new(BIO_s_mem());
  if (!s->ssl || !s->rbio || !s->wbio) {
    SSL_free(s->ssl);
    BIO_free(s->rbio);
    BIO_free(s->wbio);
    delete s;
    napi_throw_error(env, nullptr, "failed to create SSL session");
    return nullptr;
  }
  SSL_set_bio(s->ssl, s->rbio, s->wbio);
  SSL_set_connect_state(s->ssl);

  if (argc >= 1) {
    char host[256] = {0};
    size_t hostLen = 0;
    napi_get_value_string_utf8(env, argv[0], host, sizeof(host), &hostLen);
    if (hostLen > 0)
      SSL_set_tlsext_host_name(s->ssl, host);
  }

  napi_wrap(env, thisObj, s, freeSession, nullptr, nullptr);
  napi_create_reference(env, thisObj, 1, &s->jsRef);
  return thisObj;
}

napi_value tlsStart(napi_env env, napi_callback_info info) {
  napi_value thisObj;
  napi_get_cb_info(env, info, nullptr, nullptr, &thisObj, nullptr);
  TlsSession *s = unwrap(env, thisObj);
  if (s)
    pump(s);
  napi_value undefined;
  napi_get_undefined(env, &undefined);
  return undefined;
}

bool asBuffer(napi_env env, napi_value val, void **data, size_t *len) {
  napi_typedarray_type taType;
  napi_value abuf;
  size_t offset;
  if (napi_get_typedarray_info(env, val, &taType, len, data, &abuf, &offset) ==
      napi_ok)
    return *data != nullptr;
  bool isBuf = false;
  if (napi_is_buffer(env, val, &isBuf) == napi_ok && isBuf) {
    return napi_get_buffer_info(env, val, data, len) == napi_ok;
  }
  return false;
}

napi_value tlsPush(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_value thisObj;
  napi_get_cb_info(env, info, &argc, argv, &thisObj, nullptr);
  TlsSession *s = unwrap(env, thisObj);
  if (!s || !s->ssl) {
    napi_value undefined;
    napi_get_undefined(env, &undefined);
    return undefined;
  }
  void *data = nullptr;
  size_t len = 0;
  if (!asBuffer(env, argv[0], &data, &len)) {
    napi_throw_type_error(env, nullptr, "push() needs a Buffer");
    return nullptr;
  }
  BIO_write(s->rbio, data, static_cast<int>(len));
  pump(s);
  napi_value undefined;
  napi_get_undefined(env, &undefined);
  return undefined;
}

napi_value tlsWrite(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_value thisObj;
  napi_get_cb_info(env, info, &argc, argv, &thisObj, nullptr);
  TlsSession *s = unwrap(env, thisObj);
  if (!s || !s->ssl) {
    napi_value undefined;
    napi_get_undefined(env, &undefined);
    return undefined;
  }
  void *data = nullptr;
  size_t len = 0;
  if (!asBuffer(env, argv[0], &data, &len)) {
    napi_throw_type_error(env, nullptr, "write() needs a Buffer");
    return nullptr;
  }
  int n = SSL_write(s->ssl, data, static_cast<int>(len));
  flushWriteBio(s);
  if (n <= 0) {
    int err = SSL_get_error(s->ssl, n);
    if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE)
      emitError(s, opensslError().c_str());
  }
  napi_value undefined;
  napi_get_undefined(env, &undefined);
  return undefined;
}

napi_value tlsShutdown(napi_env env, napi_callback_info info) {
  napi_value thisObj;
  napi_get_cb_info(env, info, nullptr, nullptr, &thisObj, nullptr);
  TlsSession *s = unwrap(env, thisObj);
  if (s && s->ssl && !s->closed) {
    SSL_shutdown(s->ssl);
    flushWriteBio(s);
    s->closed = true;
  }
  napi_value undefined;
  napi_get_undefined(env, &undefined);
  return undefined;
}

} // namespace

napi_value initTlsWrapBinding(napi_env env, napi_value exports) {
  napi_property_descriptor proto[] = {
      {"start", nullptr, tlsStart, nullptr, nullptr, nullptr, napi_enumerable, nullptr},
      {"push", nullptr, tlsPush, nullptr, nullptr, nullptr, napi_enumerable, nullptr},
      {"write", nullptr, tlsWrite, nullptr, nullptr, nullptr, napi_enumerable, nullptr},
      {"shutdown", nullptr, tlsShutdown, nullptr, nullptr, nullptr, napi_enumerable, nullptr},
  };
  napi_value ctor;
  napi_define_class(
      env,
      "TLSWrap",
      NAPI_AUTO_LENGTH,
      tlsNew,
      nullptr,
      sizeof(proto) / sizeof(proto[0]),
      proto,
      &ctor);
  napi_property_descriptor props[] = {
      {"TLSWrap", nullptr, nullptr, nullptr, nullptr, ctor, napi_enumerable, nullptr},
  };
  napi_define_properties(env, exports, 1, props);
  return exports;
}

} // namespace node_compat
} // namespace hermes
