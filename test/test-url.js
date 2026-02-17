// Copyright (c) Tzvetan Mikov.
// RUN: %hermes-node %s | %FileCheck %s
// CHECK: PASS
// Test the URL module backed by Ada parser.

'use strict';

var assert = function(cond, msg) {
  if (!cond) throw new Error('Assertion failed: ' + (msg || ''));
};

// ---- URL constructor and basic parsing ----

var u = new URL('http://example.com:8080/path?query=1#hash');
assert(u.href === 'http://example.com:8080/path?query=1#hash', 'href');
assert(u.protocol === 'http:', 'protocol: ' + u.protocol);
assert(u.hostname === 'example.com', 'hostname: ' + u.hostname);
assert(u.port === '8080', 'port: ' + u.port);
assert(u.host === 'example.com:8080', 'host: ' + u.host);
assert(u.pathname === '/path', 'pathname: ' + u.pathname);
assert(u.search === '?query=1', 'search: ' + u.search);
assert(u.hash === '#hash', 'hash: ' + u.hash);

// ---- URL with base ----

var u2 = new URL('/relative', 'http://base.com');
assert(u2.href === 'http://base.com/relative', 'base URL: ' + u2.href);
assert(u2.hostname === 'base.com', 'base hostname');

// ---- URL with credentials ----

var u3 = new URL('http://user:pass@example.com/');
assert(u3.username === 'user', 'username: ' + u3.username);
assert(u3.password === 'pass', 'password: ' + u3.password);

// ---- URL without optional components ----

var u4 = new URL('http://example.com');
assert(u4.port === '', 'no port: ' + u4.port);
assert(u4.search === '', 'no search');
assert(u4.hash === '', 'no hash');
assert(u4.pathname === '/', 'default pathname: ' + u4.pathname);

// ---- URL setters ----

var u5 = new URL('http://example.com/foo');
u5.pathname = '/bar';
assert(u5.pathname === '/bar', 'set pathname: ' + u5.pathname);

u5.search = '?x=1';
assert(u5.search === '?x=1', 'set search');

u5.hash = '#section';
assert(u5.hash === '#section', 'set hash');

u5.port = '3000';
assert(u5.port === '3000', 'set port: ' + u5.port);

u5.hostname = 'other.com';
assert(u5.hostname === 'other.com', 'set hostname');

u5.protocol = 'https:';
assert(u5.protocol === 'https:', 'set protocol');

// ---- URL.parse (no throw) ----

var parsed = URL.parse('http://valid.com/path');
assert(parsed !== null, 'URL.parse valid');
assert(parsed.hostname === 'valid.com', 'URL.parse hostname');

var invalid = URL.parse('not a url');
assert(invalid === null, 'URL.parse invalid returns null');

// ---- URL.canParse ----

assert(URL.canParse('http://example.com') === true, 'canParse valid');
assert(URL.canParse('not a url') === false, 'canParse invalid');
assert(URL.canParse('/path', 'http://base.com') === true, 'canParse with base');

// ---- URL constructor throws on invalid ----

var threw = false;
try {
  new URL('not a url');
} catch (e) {
  threw = true;
}
assert(threw, 'URL constructor throws on invalid input');

// ---- URL origin ----

var u6 = new URL('http://example.com:8080/path');
assert(u6.origin === 'http://example.com:8080', 'origin: ' + u6.origin);

var u7 = new URL('https://secure.com/');
assert(u7.origin === 'https://secure.com', 'https origin');

// ---- URL toString / toJSON ----

var u8 = new URL('http://example.com/path');
assert(u8.toString() === u8.href, 'toString');
assert(u8.toJSON() === u8.href, 'toJSON');

// ---- URLSearchParams ----

var sp = new URLSearchParams('a=1&b=2&c=3');
assert(sp.get('a') === '1', 'sp.get a');
assert(sp.get('b') === '2', 'sp.get b');
assert(sp.get('c') === '3', 'sp.get c');
assert(sp.get('d') === null, 'sp.get missing');
assert(sp.has('a') === true, 'sp.has a');
assert(sp.has('d') === false, 'sp.has d');
assert(sp.size === 3, 'sp.size');

sp.append('d', '4');
assert(sp.get('d') === '4', 'sp.append');
assert(sp.size === 4, 'sp.size after append');

sp.delete('c');
assert(sp.get('c') === null, 'sp.delete');
assert(sp.size === 3, 'sp.size after delete');

sp.set('a', '10');
assert(sp.get('a') === '10', 'sp.set');

var all = sp.getAll('a');
assert(all.length === 1, 'sp.getAll length');
assert(all[0] === '10', 'sp.getAll value');

// toString
var spStr = sp.toString();
assert(typeof spStr === 'string', 'sp.toString');
assert(spStr.indexOf('a=10') !== -1, 'sp.toString contains a=10');

// sort
sp.sort();
var sorted = sp.toString();
assert(sorted.indexOf('a=10') < sorted.indexOf('b=2'), 'sp.sort order');

// iteration
var keys = [];
var iter = sp.keys();
var next;
while (!(next = iter.next()).done) {
  keys.push(next.value);
}
assert(keys.length === sp.size, 'sp.keys count');

// forEach
var forEachKeys = [];
sp.forEach(function(value, key) {
  forEachKeys.push(key);
});
assert(forEachKeys.length === sp.size, 'sp.forEach count');

// from object
var sp2 = new URLSearchParams({x: '1', y: '2'});
assert(sp2.get('x') === '1', 'sp from object x');
assert(sp2.get('y') === '2', 'sp from object y');

// from array of pairs
var sp3 = new URLSearchParams([['m', 'n'], ['o', 'p']]);
assert(sp3.get('m') === 'n', 'sp from pairs m');
assert(sp3.get('o') === 'p', 'sp from pairs o');

// strip leading '?'
var sp4 = new URLSearchParams('?a=1');
assert(sp4.get('a') === '1', 'sp strip leading ?');

// ---- URL searchParams integration ----

var u9 = new URL('http://example.com/path?a=1&b=2');
var params = u9.searchParams;
assert(params.get('a') === '1', 'url.searchParams.get a');
assert(params.get('b') === '2', 'url.searchParams.get b');

// Modify searchParams -> reflects in URL
params.append('c', '3');
assert(u9.search.indexOf('c=3') !== -1, 'searchParams append reflected in URL');

// ---- domainToASCII / domainToUnicode ----

var internalUrl = require('internal/url');

var ascii = internalUrl.domainToASCII('example.com');
assert(ascii === 'example.com', 'domainToASCII basic: ' + ascii);

var unicode = internalUrl.domainToUnicode('example.com');
assert(typeof unicode === 'string', 'domainToUnicode returns string');

// ---- isURL ----

assert(internalUrl.isURL(u) === true, 'isURL for URL instance');
assert(internalUrl.isURL({href: 'x', protocol: 'y'}) === true, 'isURL for URL-like');
assert(internalUrl.isURL('string') === false, 'isURL for string');
assert(internalUrl.isURL(null) === false, 'isURL for null');
assert(internalUrl.isURL({href: 'x', protocol: 'y', path: '/'}) === false,
  'isURL for legacy url (has path)');

// ---- urlToHttpOptions ----

var httpOpts = internalUrl.urlToHttpOptions(
  new URL('http://user:pass@example.com:8080/path?q=1#frag')
);
assert(httpOpts.protocol === 'http:', 'httpOpts.protocol');
assert(httpOpts.hostname === 'example.com', 'httpOpts.hostname');
assert(httpOpts.port === 8080, 'httpOpts.port: ' + httpOpts.port);
assert(httpOpts.pathname === '/path', 'httpOpts.pathname');
assert(httpOpts.search === '?q=1', 'httpOpts.search');
assert(httpOpts.path === '/path?q=1', 'httpOpts.path');
assert(httpOpts.hash === '#frag', 'httpOpts.hash');
assert(httpOpts.auth === 'user:pass', 'httpOpts.auth');

// ---- toPathIfFileURL ----

assert(internalUrl.toPathIfFileURL('/normal/path') === '/normal/path',
  'toPathIfFileURL passthrough');
assert(internalUrl.toPathIfFileURL(123) === 123,
  'toPathIfFileURL non-string passthrough');

// ---- fileURLToPath / pathToFileURL ----

var fileUrl = internalUrl.pathToFileURL('/tmp/test.txt');
assert(fileUrl instanceof URL, 'pathToFileURL returns URL');
assert(fileUrl.protocol === 'file:', 'pathToFileURL protocol');
assert(fileUrl.pathname === '/tmp/test.txt', 'pathToFileURL pathname');

var backToPath = internalUrl.fileURLToPath(fileUrl);
assert(backToPath === '/tmp/test.txt', 'fileURLToPath roundtrip: ' + backToPath);

// fileURLToPath from string
var pathFromStr = internalUrl.fileURLToPath('file:///home/user/test.js');
assert(pathFromStr === '/home/user/test.js', 'fileURLToPath from string');

// ---- URL is available as global ----

assert(typeof globalThis.URL === 'function', 'URL is global');
assert(typeof globalThis.URLSearchParams === 'function', 'URLSearchParams is global');

// ---- HTTPS URL ----

var httpsUrl = new URL('https://secure.example.com/api/v1?key=val');
assert(httpsUrl.protocol === 'https:', 'https protocol');
assert(httpsUrl.hostname === 'secure.example.com', 'https hostname');
assert(httpsUrl.pathname === '/api/v1', 'https pathname');

// ---- FTP URL ----

var ftpUrl = new URL('ftp://files.example.com/pub/docs');
assert(ftpUrl.protocol === 'ftp:', 'ftp protocol');

// ---- url.js module ----

var url = require('url');
assert(typeof url.parse === 'function', 'url.parse exists');
assert(typeof url.format === 'function', 'url.format exists');
assert(typeof url.resolve === 'function', 'url.resolve exists');

// url.parse (legacy)
var parsed2 = url.parse('http://example.com:8080/path?query=1#hash');
assert(parsed2.protocol === 'http:', 'url.parse protocol');
assert(parsed2.hostname === 'example.com', 'url.parse hostname');
assert(parsed2.port === '8080', 'url.parse port');
assert(parsed2.pathname === '/path', 'url.parse pathname');
assert(parsed2.search === '?query=1', 'url.parse search');
assert(parsed2.hash === '#hash', 'url.parse hash');

// url.format (legacy)
var formatted = url.format({
  protocol: 'http:',
  hostname: 'example.com',
  port: '3000',
  pathname: '/test'
});
assert(formatted === 'http://example.com:3000/test', 'url.format: ' + formatted);

// url.resolve (legacy)
var resolved = url.resolve('http://example.com/a/b', '/c');
assert(resolved === 'http://example.com/c', 'url.resolve: ' + resolved);

console.log('PASS');
