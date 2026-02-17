// Copyright (c) Tzvetan Mikov.
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// RUN: %hermes-node %s | %FileCheck %s
// CHECK: PASS

'use strict';

var assert = globalThis.require('assert');

// Test 1: Verify binding exports exist.
var binding = globalThis.internalBinding('http_parser');
assert(typeof binding.HTTPParser === 'function', 'HTTPParser is a function');
assert(Array.isArray(binding.methods), 'methods is an array');
assert(Array.isArray(binding.allMethods), 'allMethods is an array');
assert(typeof binding.ConnectionsList === 'function', 'ConnectionsList is a function');

// Test 2: Verify constants on HTTPParser.
var HTTPParser = binding.HTTPParser;
assert(HTTPParser.REQUEST === 1, 'REQUEST === 1');
assert(HTTPParser.RESPONSE === 2, 'RESPONSE === 2');
assert(typeof HTTPParser.kOnMessageBegin === 'number');
assert(typeof HTTPParser.kOnHeaders === 'number');
assert(typeof HTTPParser.kOnHeadersComplete === 'number');
assert(typeof HTTPParser.kOnBody === 'number');
assert(typeof HTTPParser.kOnMessageComplete === 'number');
assert(typeof HTTPParser.kOnExecute === 'number');
assert(typeof HTTPParser.kOnTimeout === 'number');
assert(typeof HTTPParser.kLenientNone === 'number');
assert(typeof HTTPParser.kLenientAll === 'number');

// Test 3: Verify methods arrays.
assert(binding.methods.includes('GET'), 'methods includes GET');
assert(binding.methods.includes('POST'), 'methods includes POST');
assert(binding.methods.includes('DELETE'), 'methods includes DELETE');
assert(binding.allMethods.includes('GET'), 'allMethods includes GET');
assert(binding.allMethods.includes('PRI'), 'allMethods includes PRI');
assert(binding.allMethods.includes('DESCRIBE'), 'allMethods includes DESCRIBE');
assert(binding.allMethods.length > binding.methods.length, 'allMethods > methods');

// Test 4: Create parser and parse a simple HTTP request.
var parser = new HTTPParser();
parser.initialize(HTTPParser.REQUEST, {});

var messageBeginCalled = false;
var headersCompleteCalled = false;
var bodyData = null;
var messageCompleteCalled = false;
var parsedMethod = null;
var parsedUrl = null;
var parsedHeaders = null;
var parsedVersionMajor = null;
var parsedVersionMinor = null;

parser[HTTPParser.kOnMessageBegin] = function() {
  messageBeginCalled = true;
};

parser[HTTPParser.kOnHeadersComplete] = function(
    versionMajor, versionMinor, headers, method, url,
    statusCode, statusMessage, upgrade, shouldKeepAlive) {
  headersCompleteCalled = true;
  parsedMethod = method;
  parsedUrl = url;
  parsedHeaders = headers;
  parsedVersionMajor = versionMajor;
  parsedVersionMinor = versionMinor;
  return 0;
};

parser[HTTPParser.kOnBody] = function(chunk) {
  bodyData = chunk;
};

parser[HTTPParser.kOnMessageComplete] = function() {
  messageCompleteCalled = true;
};

var request = 'GET /test?q=1 HTTP/1.1\r\n' +
              'Host: localhost:8080\r\n' +
              'User-Agent: hermes-test\r\n' +
              'Accept: */*\r\n' +
              '\r\n';

var buf = Buffer.from(request);
var ret = parser.execute(buf);
assert(typeof ret === 'number', 'execute returns a number on success');
assert(ret === buf.length, 'all bytes parsed');
assert(messageBeginCalled, 'kOnMessageBegin called');
assert(headersCompleteCalled, 'kOnHeadersComplete called');
assert(messageCompleteCalled, 'kOnMessageComplete called');
assert(parsedMethod === 1, 'method is GET (1)'); // HTTP_GET = 1
assert(parsedUrl === '/test?q=1', 'url is /test?q=1');
assert(parsedVersionMajor === 1, 'version major is 1');
assert(parsedVersionMinor === 1, 'version minor is 1');
assert(Array.isArray(parsedHeaders), 'headers is an array');
// Headers are flat: [name, value, name, value, ...]
assert(parsedHeaders.length === 6, 'three header pairs = 6 elements');
assert(parsedHeaders[0].toLowerCase() === 'host', 'first header is Host');
assert(parsedHeaders[1] === 'localhost:8080', 'Host value');
assert(parsedHeaders[2].toLowerCase() === 'user-agent', 'second header is User-Agent');
assert(parsedHeaders[3] === 'hermes-test', 'User-Agent value');
assert(bodyData === null, 'no body for GET request');

// Test 5: Parse HTTP response.
var parser2 = new HTTPParser();
parser2.initialize(HTTPParser.RESPONSE, {});

var respHeadersComplete = false;
var respStatusCode = null;
var respStatusMessage = null;
var respBody = '';
var respComplete = false;

parser2[HTTPParser.kOnMessageBegin] = function() {};
parser2[HTTPParser.kOnHeadersComplete] = function(
    versionMajor, versionMinor, headers, method, url,
    statusCode, statusMessage, upgrade, shouldKeepAlive) {
  respHeadersComplete = true;
  respStatusCode = statusCode;
  respStatusMessage = statusMessage;
  return 0;
};
parser2[HTTPParser.kOnBody] = function(chunk) {
  respBody += Buffer.from(chunk).toString();
};
parser2[HTTPParser.kOnMessageComplete] = function() {
  respComplete = true;
};

var response = 'HTTP/1.1 200 OK\r\n' +
               'Content-Type: text/plain\r\n' +
               'Content-Length: 5\r\n' +
               '\r\n' +
               'Hello';

var respBuf = Buffer.from(response);
var ret2 = parser2.execute(respBuf);
assert(typeof ret2 === 'number', 'response parse returns number');
assert(ret2 === respBuf.length, 'all response bytes parsed');
assert(respHeadersComplete, 'response headers complete');
assert(respStatusCode === 200, 'status code is 200');
assert(respStatusMessage === 'OK', 'status message is OK');
assert(respBody === 'Hello', 'response body is Hello');
assert(respComplete, 'response message complete');

// Test 6: Parse a POST request with body.
var parser3 = new HTTPParser();
parser3.initialize(HTTPParser.REQUEST, {});

var postBody = '';
var postComplete = false;
var postMethod = null;

parser3[HTTPParser.kOnMessageBegin] = function() {};
parser3[HTTPParser.kOnHeadersComplete] = function(
    versionMajor, versionMinor, headers, method, url) {
  postMethod = method;
  return 0;
};
parser3[HTTPParser.kOnBody] = function(chunk) {
  postBody += Buffer.from(chunk).toString();
};
parser3[HTTPParser.kOnMessageComplete] = function() {
  postComplete = true;
};

var postRequest = 'POST /data HTTP/1.1\r\n' +
                  'Host: localhost\r\n' +
                  'Content-Length: 13\r\n' +
                  '\r\n' +
                  'Hello, World!';

var postBuf = Buffer.from(postRequest);
var ret3 = parser3.execute(postBuf);
assert(typeof ret3 === 'number', 'POST parse returns number');
assert(postComplete, 'POST message complete');
assert(postMethod === 3, 'method is POST (3)'); // HTTP_POST = 3
assert(postBody === 'Hello, World!', 'POST body correct');

// Test 7: Reinitialize parser (keep-alive).
parser3.initialize(HTTPParser.REQUEST, {});

var reinitComplete = false;
var reinitUrl = null;
parser3[HTTPParser.kOnMessageBegin] = function() {};
parser3[HTTPParser.kOnHeadersComplete] = function(
    versionMajor, versionMinor, headers, method, url) {
  reinitUrl = url;
  return 0;
};
parser3[HTTPParser.kOnBody] = function() {};
parser3[HTTPParser.kOnMessageComplete] = function() {
  reinitComplete = true;
};

var req2 = 'GET /second HTTP/1.1\r\nHost: localhost\r\n\r\n';
parser3.execute(Buffer.from(req2));
assert(reinitComplete, 'reinitialize allows second parse');
assert(reinitUrl === '/second', 'second request URL correct');

// Test 8: Parser error on invalid input.
var parser4 = new HTTPParser();
parser4.initialize(HTTPParser.REQUEST, {});
parser4[HTTPParser.kOnMessageBegin] = function() {};
parser4[HTTPParser.kOnHeadersComplete] = function() { return 0; };
parser4[HTTPParser.kOnBody] = function() {};
parser4[HTTPParser.kOnMessageComplete] = function() {};

var invalid = 'INVALID GARBAGE\r\n\r\n';
var ret4 = parser4.execute(Buffer.from(invalid));
// On error, execute returns an Error-like object with code, reason, bytesParsed
assert(typeof ret4 === 'object', 'error returns an object');
assert(typeof ret4.code === 'string', 'error has code');
assert(typeof ret4.reason === 'string', 'error has reason');
assert(typeof ret4.bytesParsed === 'number', 'error has bytesParsed');

// Test 9: Pause / Resume
var parser5 = new HTTPParser();
parser5.initialize(HTTPParser.REQUEST, {});
parser5[HTTPParser.kOnMessageBegin] = function() {};
parser5[HTTPParser.kOnHeadersComplete] = function() { return 0; };
parser5[HTTPParser.kOnBody] = function() {};
parser5[HTTPParser.kOnMessageComplete] = function() {};
// Just verify pause/resume don't throw.
parser5.pause();
parser5.resume();

// Test 10: ConnectionsList
var connList = new binding.ConnectionsList();
assert(typeof connList.all === 'function', 'connList has all()');
assert(typeof connList.idle === 'function', 'connList has idle()');
assert(typeof connList.active === 'function', 'connList has active()');
assert(typeof connList.expired === 'function', 'connList has expired()');
var allConns = connList.all();
assert(Array.isArray(allConns), 'all() returns array');
assert(allConns.length === 0, 'empty initially');

// Test 11: Try loading the HTTP module.
var http = require('http');
assert(typeof http.createServer === 'function', 'http.createServer exists');
assert(typeof http.request === 'function', 'http.request exists');
assert(typeof http.get === 'function', 'http.get exists');
assert(typeof http.Agent === 'function', 'http.Agent exists');
assert(typeof http.Server === 'function', 'http.Server exists');
assert(typeof http.ClientRequest === 'function', 'http.ClientRequest exists');
assert(typeof http.IncomingMessage === 'function', 'http.IncomingMessage exists');
assert(typeof http.STATUS_CODES === 'object', 'http.STATUS_CODES exists');
assert(http.STATUS_CODES[200] === 'OK', 'STATUS_CODES[200] = OK');

// Test 12: HTTP server + client end-to-end.
var server = http.createServer(function(req, res) {
  assert(req.method === 'GET', 'server sees GET method');
  assert(req.url === '/test', 'server sees /test URL');
  res.writeHead(200, { 'Content-Type': 'text/plain' });
  res.end('Hello from hermes-node');
});

server.listen(0, function() {
  var port = server.address().port;
  http.get('http://127.0.0.1:' + port + '/test', function(res) {
    var data = '';
    res.on('data', function(chunk) { data += chunk; });
    res.on('end', function() {
      assert(res.statusCode === 200, 'client sees 200');
      assert(data === 'Hello from hermes-node', 'response body matches');
      server.close(function() {
        console.log('PASS');
      });
    });
  });
});
