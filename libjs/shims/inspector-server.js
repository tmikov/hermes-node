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

var fs = require('fs');
var http = require('http');
var { WebSocketServer } = require('ws');
var bridge = internalBinding('inspector_bridge');

var config = bridge.getConfig();

// Only one debugger client at a time.
var activeWs = null;
// Buffer outbound CDP messages when no client is connected. This is needed
// for --inspect-brk: the runtime pauses and sends Debugger.paused before
// any DevTools client has connected. When a client connects, buffered
// messages are replayed so the client sees the paused state.
var pendingMessages = [];
// Map scriptId -> url from Debugger.scriptParsed events, used to serve
// Debugger.getScriptSource requests from disk.
var scriptUrls = Object.create(null);

// Given a regex string from Debugger.setBreakpointByUrl.urlRegex, return
// the URL of a known script that the regex matches, or (if none yet)
// the most plausible literal alternative from the regex itself (the one
// that does not start with file:// -- Hermes uses bare paths). Returns
// null if the regex cannot be parsed at all.
function pickUrlForRegex(urlRegex) {
  var re;
  try {
    re = new RegExp(urlRegex);
  } catch (e) {
    return null;
  }
  for (var sid in scriptUrls) {
    if (re.test(scriptUrls[sid])) {
      return scriptUrls[sid];
    }
  }
  // No known script matches yet (the script may not have been parsed).
  // Fall back: split on '|' top-level alternatives, undo regex escapes,
  // and prefer the alternative that does not start with file://.
  var alts = urlRegex.split('|').map(function(a) {
    return a.replace(/\\(.)/g, '$1');
  });
  for (var i = 0; i < alts.length; i++) {
    if (alts[i].indexOf('file://') !== 0) return alts[i];
  }
  return alts[0] || null;
}

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

  // Replay any buffered outbound messages (e.g. Debugger.paused from
  // --inspect-brk that arrived before the client connected).
  if (pendingMessages.length > 0) {
    for (var i = 0; i < pendingMessages.length; i++) {
      ws.send(pendingMessages[i]);
    }
    pendingMessages = [];
  }

  // Forward WebSocket messages (CDP commands) to main runtime.
  ws.on('message', function(data) {
    var str = data.toString();
    // Intercept Debugger.getScriptSource: serve from disk when possible.
    if (str.indexOf('getScriptSource') !== -1) {
      try {
        var msg = JSON.parse(str);
        if (msg.method === 'Debugger.getScriptSource') {
          var scriptId = msg.params && msg.params.scriptId;
          var url = scriptId && scriptUrls[scriptId];
          if (url) {
            try {
              var source = fs.readFileSync(url, 'utf8');
              ws.send(JSON.stringify({
                id: msg.id,
                result: { scriptSource: source }
              }));
              return;
            } catch (e) {
              // File unreadable (e.g. internal module) -- fall through.
            }
          }
        }
      } catch (e) {
        // JSON parse failed -- fall through to normal forwarding.
      }
    }
    // Rewrite Debugger.setBreakpointByUrl with urlRegex into url. Hermes
    // does not implement urlRegex; if we let the request through it
    // returns an error and DevTools resubmits forever. We compile the
    // regex, match it against URLs of known scripts, and forward with
    // url: <match> instead. If no script matches yet (the script may
    // not be parsed), we still rewrite using the alternative that does
    // not start with file:// (the form Hermes uses for scriptParsed).
    if (str.indexOf('setBreakpointByUrl') !== -1) {
      try {
        var bpMsg = JSON.parse(str);
        if (bpMsg.method === 'Debugger.setBreakpointByUrl' &&
            bpMsg.params &&
            bpMsg.params.urlRegex &&
            !bpMsg.params.url) {
          var rewritten = pickUrlForRegex(bpMsg.params.urlRegex);
          if (rewritten) {
            bpMsg.params.url = rewritten;
            delete bpMsg.params.urlRegex;
            str = JSON.stringify(bpMsg);
          }
        }
      } catch (e) {
        // Fall through.
      }
    }
    bridge.sendToMain(str);
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
  // Track Debugger.scriptParsed events to map scriptId -> url.
  if (msg.indexOf('scriptParsed') !== -1) {
    try {
      var parsed = JSON.parse(msg);
      if (parsed.method === 'Debugger.scriptParsed' && parsed.params) {
        var p = parsed.params;
        if (p.scriptId && p.url) {
          scriptUrls[p.scriptId] = p.url;
        }
      }
    } catch (e) {
      // Ignore parse errors.
    }
  }
  if (activeWs && activeWs.readyState === 1) { // WebSocket.OPEN === 1
    activeWs.send(msg);
  } else {
    // Buffer messages when no client is connected (e.g. --inspect-brk pause
    // sends Debugger.paused before DevTools has connected).
    pendingMessages.push(msg);
  }
});

// Register shutdown callback so the main thread can stop the inspector.
bridge.setShutdownCallback(function() {
  // Close WebSocket connections and servers to let the event loop exit.
  if (activeWs) {
    activeWs.close();
    activeWs = null;
  }
  wss.close();
  server.close();
});

// --- Start listening ---

server.listen(config.port, config.host, function() {
  var addr = server.address();
  // Store actual port in config for use by /json endpoints.
  config.actualPort = addr.port;
  // Signal main thread that inspector is ready.
  bridge.notifyReady(addr.port);
});
