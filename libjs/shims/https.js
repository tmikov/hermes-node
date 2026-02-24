// Stub https module -- TLS is not yet implemented.
// Allows packages that require('https') at top level to load without
// crashing, as long as they only use http at runtime.
'use strict';

function notImplemented() {
  throw new Error('https is not supported (TLS not implemented)');
}

module.exports = {
  Agent: notImplemented,
  Server: notImplemented,
  createServer: notImplemented,
  get: notImplemented,
  globalAgent: undefined,
  request: notImplemented,
};
