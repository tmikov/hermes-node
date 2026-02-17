// A simple HTTP server that serves a "Hello, World!" page and static files.
// Usage: hermes-node http-server.js [port] [static-dir]
//
// Try it:
//   hermes-node http-server.js 3000 .
//   curl http://localhost:3000/
//   curl http://localhost:3000/static/hello.png > /tmp/hello.png
//   open http://localhost:3000/  (in a browser)

'use strict';

var http = require('http');
var fs = require('fs');
var path = require('path');

var port = parseInt(process.argv[2], 10) || 3000;
var staticDir = path.resolve(process.argv[3] || path.dirname(__filename));

// Map file extensions to content types.
var contentTypes = {
  '.html': 'text/html',
  '.css':  'text/css',
  '.js':   'text/javascript',
  '.json': 'application/json',
  '.png':  'image/png',
  '.jpg':  'image/jpeg',
  '.jpeg': 'image/jpeg',
  '.gif':  'image/gif',
  '.svg':  'image/svg+xml',
  '.ico':  'image/x-icon',
  '.txt':  'text/plain',
};

var HELLO_HTML = [
  '<!DOCTYPE html>',
  '<html><head><title>Hello</title></head>',
  '<body>',
  '<h1>Hello, World!</h1>',
  '<p>Served by <code>hermes-node</code>.</p>',
  '<p><img src="/static/hello.png" alt="hello" width="64" height="64"></p>',
  '<p>Static files are available under <code>/static/</code></p>',
  '</body></html>',
].join('\n');

var server = http.createServer(function(req, res) {
  var url = req.url;

  // GET / — serve the hello page.
  if (url === '/' || url === '/index.html') {
    console.log('%s %s -> 200 text/html', req.method, url);
    res.writeHead(200, {'Content-Type': 'text/html'});
    res.end(HELLO_HTML);
    return;
  }

  // GET /static/<path> — serve a file from staticDir.
  if (url.indexOf('/static/') === 0) {
    var relPath = url.slice('/static/'.length);
    // Basic path traversal protection.
    if (relPath.indexOf('..') !== -1) {
      console.log('%s %s -> 400 bad request', req.method, url);
      res.writeHead(400, {'Content-Type': 'text/plain'});
      res.end('Bad request\n');
      return;
    }
    var filePath = path.join(staticDir, relPath);
    fs.readFile(filePath, function(err, data) {
      if (err) {
        var status = err.code === 'ENOENT' ? 404 : 500;
        console.log('%s %s -> %d %s', req.method, url, status, err.code);
        res.writeHead(status, {'Content-Type': 'text/plain'});
        res.end(status === 404 ? 'Not found\n' : 'Server error\n');
        return;
      }
      var ext = path.extname(filePath).toLowerCase();
      var ct = contentTypes[ext] || 'application/octet-stream';
      console.log('%s %s -> 200 %s (%d bytes)', req.method, url, ct, data.length);
      res.writeHead(200, {'Content-Type': ct});
      res.end(data);
    });
    return;
  }

  // Everything else — 404.
  console.log('%s %s -> 404', req.method, url);
  res.writeHead(404, {'Content-Type': 'text/plain'});
  res.end('Not found\n');
});

server.listen(port, function() {
  console.log('Listening on http://localhost:' + port + '/');
  console.log('Static files from ' + staticDir + ' at /static/');
});
