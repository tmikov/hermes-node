// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// Inspector server script for the inspector runtime (IO thread).
// Creates an HTTP + WebSocket server that bridges Chrome DevTools (CDP)
// to the main runtime via the inspector_bridge native binding.
//
// Loaded via: require('inspector-server') from the inspector runtime's
// evalCode. Not intended for user code.

'use strict';

var http = require('http');
var { WebSocketServer } = require('ws');
var bridge = internalBinding('inspector_bridge');

var config = bridge.getConfig();

// Only one debugger client at a time.
var activeWs = null;

// --- HTTP server ---

var server = http.createServer(function(req, res) {
  var url = req.url || '/';

  // Strip query string for path matching.
  var qIdx = url.indexOf('?');
  var pathname = qIdx >= 0 ? url.substring(0, qIdx) : url;

  if (pathname === '/json' || pathname === '/json/list') {
    var wsUrl = 'ws://' + config.host + ':' + config.actualPort + '/' +
        config.sessionId;
    var devtoolsUrl = 'http://' + config.host + ':' + config.actualPort +
        '/devtools/inspector.html?ws=' + config.host + ':' +
        config.actualPort + '/' + config.sessionId;
    var target = [{
      description: 'hermes-node instance',
      id: config.sessionId,
      title: config.scriptName || 'hermes-node',
      type: 'node',
      url: config.scriptName ? 'file://' + config.scriptName : '',
      webSocketDebuggerUrl: wsUrl,
      devtoolsFrontendUrl: devtoolsUrl
    }];
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify(target));
    return;
  }

  if (pathname === '/json/version') {
    var versionInfo = {
      Browser: 'hermes-node/0.1.0',
      'Protocol-Version': '1.1'
    };
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify(versionInfo));
    return;
  }

  if (pathname.indexOf('/devtools/') === 0) {
    // CDN redirect to Google-hosted DevTools frontend.
    var ws = '';
    var wsMatch = (url.match(/[?&]ws=([^&]+)/) || [])[1];
    if (wsMatch) {
      ws = decodeURIComponent(wsMatch);
    } else {
      ws = config.host + ':' + config.actualPort + '/' + config.sessionId;
    }
    var cdnBase = 'https://chrome-devtools-frontend.appspot.com/serve_file/' +
        '@60127beb442528082b3f6eff7392267e145262c3/js_app.html';
    var html = '<!DOCTYPE html><html><head><script>' +
        'location = ' + JSON.stringify(cdnBase) + ' + "?ws=" + ' +
        JSON.stringify(ws) + ';' +
        '</script></head><body>Opening DevTools...</body></html>';
    res.writeHead(200, { 'Content-Type': 'text/html' });
    res.end(html);
    return;
  }

  res.writeHead(404, { 'Content-Type': 'text/plain' });
  res.end('Not Found');
});

// --- WebSocket server ---

var wss = new WebSocketServer({
  server: server,
  path: '/' + config.sessionId
});

wss.on('connection', function(ws) {
  // Only one client at a time.
  if (activeWs) {
    ws.close(1013, 'Another debugger is already connected');
    return;
  }

  activeWs = ws;

  // Forward WebSocket messages (CDP commands) to main runtime.
  ws.on('message', function(data) {
    bridge.sendToMain(data.toString());
  });

  ws.on('close', function() {
    if (activeWs === ws) {
      activeWs = null;
    }
  });

  ws.on('error', function() {
    if (activeWs === ws) {
      activeWs = null;
    }
  });
});

// Register callback for outbound CDP messages from main runtime.
bridge.setMessageCallback(function(msg) {
  if (activeWs && activeWs.readyState === 1) { // WebSocket.OPEN === 1
    activeWs.send(msg);
  }
});

// --- Start listening ---

server.listen(config.port, config.host, function() {
  var addr = server.address();
  // Store actual port in config for use by /json endpoints.
  config.actualPort = addr.port;
  // Signal main thread that inspector is ready.
  bridge.notifyReady(addr.port);
});
