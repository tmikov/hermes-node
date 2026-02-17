// Ported from Node.js test/parallel/test-net-connect-destroy.js
// RUN: TEST_THREAD_ID=$$ %hermes-node %s

'use strict';
var common = require('../common');
var net = require('net');

var socket = new net.Socket();
socket.on('close', common.mustCall());
socket.destroy();
