// Stub tls module -- TLS is not yet implemented.
// Allows packages that require('tls') at top level to load without
// crashing, as long as they don't actually use TLS at runtime.
'use strict';

function notImplemented() {
  throw new Error('tls is not supported (TLS not implemented)');
}

module.exports = {
  connect: notImplemented,
  createSecureContext: notImplemented,
  createSecurePair: notImplemented,
  createServer: notImplemented,
  getCiphers: function() { return []; },
  DEFAULT_ECDH_CURVE: 'auto',
  DEFAULT_MAX_VERSION: 'TLSv1.3',
  DEFAULT_MIN_VERSION: 'TLSv1.2',
  SecureContext: notImplemented,
  Server: notImplemented,
  TLSSocket: notImplemented,
};
