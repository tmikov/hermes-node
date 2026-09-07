// HTTPS client. Uses tls.connect. No server.
'use strict';

const http = require('http');
const tls = require('tls');
const { URL } = require('url');

function Agent(options) {
  http.Agent.call(this, options);
  this.defaultPort = 443;
  this.protocol = 'https:';
  this.maxCachedSessions = 0;
  this._sessionCache = { map: {}, list: [] };
}
Object.setPrototypeOf(Agent.prototype, http.Agent.prototype);
Object.setPrototypeOf(Agent, http.Agent);

Agent.prototype.createConnection = function createConnection(options) {
  return tls.connect(options);
};

const globalAgent = new Agent({ keepAlive: true, timeout: 5000 });

function request(input, options, cb) {
  if (typeof input === 'string') {
    const parsed = new URL(input);
    if (typeof options === 'function') {
      cb = options;
      options = {};
    }
    options = Object.assign({
      protocol: parsed.protocol,
      hostname: parsed.hostname,
      host: parsed.hostname,
      port: parsed.port || 443,
      path: parsed.pathname + parsed.search,
    }, options || {});
  } else if (typeof input === 'function') {
    cb = input;
    options = {};
  } else {
    cb = options;
    options = input || {};
  }
  options = options || {};
  options._defaultAgent = module.exports.globalAgent;
  options.protocol = options.protocol || 'https:';
  options.port = options.port || 443;
  options.defaultPort = options.defaultPort || 443;
  return http.request(options, cb);
}

function get(input, options, cb) {
  const req = request(input, options, cb);
  req.end();
  return req;
}

function notImplemented() {
  throw new Error('https.Server is not implemented');
}

module.exports = {
  Agent: Agent,
  globalAgent: globalAgent,
  Server: notImplemented,
  createServer: notImplemented,
  get: get,
  request: request,
};
