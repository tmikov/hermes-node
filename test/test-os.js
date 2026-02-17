// Copyright (c) Tzvetan Mikov.
// RUN: %hermes-node %s | %FileCheck %s
// CHECK: PASS
// Test the os module.

'use strict';

var assert = function(cond, msg) {
  if (!cond) throw new Error('Assertion failed: ' + (msg || ''));
};

var os = require('os');

// --- hostname ---
var hostname = os.hostname();
assert(typeof hostname === 'string', 'hostname is string');
assert(hostname.length > 0, 'hostname non-empty');

// --- type (sysname) ---
var osType = os.type();
assert(typeof osType === 'string', 'type is string');
assert(osType.length > 0, 'type non-empty');

// --- release ---
var rel = os.release();
assert(typeof rel === 'string', 'release is string');
assert(rel.length > 0, 'release non-empty');

// --- version ---
var ver = os.version();
assert(typeof ver === 'string', 'version is string');

// --- machine ---
var mach = os.machine();
assert(typeof mach === 'string', 'machine is string');
assert(mach.length > 0, 'machine non-empty');

// --- platform ---
assert(os.platform() === process.platform, 'platform matches process.platform');

// --- arch ---
assert(os.arch() === process.arch, 'arch matches process.arch');

// --- tmpdir ---
var tmp = os.tmpdir();
assert(typeof tmp === 'string', 'tmpdir is string');
assert(tmp.length > 0, 'tmpdir non-empty');

// --- homedir ---
var home = os.homedir();
assert(typeof home === 'string', 'homedir is string');
assert(home.length > 0, 'homedir non-empty');

// --- cpus ---
var cpuArr = os.cpus();
assert(Array.isArray(cpuArr), 'cpus returns array');
assert(cpuArr.length > 0, 'cpus non-empty');
assert(typeof cpuArr[0].model === 'string', 'cpu model is string');
assert(typeof cpuArr[0].speed === 'number', 'cpu speed is number');
assert(typeof cpuArr[0].times === 'object', 'cpu times is object');
assert(typeof cpuArr[0].times.user === 'number', 'cpu times.user is number');
assert(typeof cpuArr[0].times.nice === 'number', 'cpu times.nice is number');
assert(typeof cpuArr[0].times.sys === 'number', 'cpu times.sys is number');
assert(typeof cpuArr[0].times.idle === 'number', 'cpu times.idle is number');
assert(typeof cpuArr[0].times.irq === 'number', 'cpu times.irq is number');

// --- loadavg ---
var avg = os.loadavg();
assert(Array.isArray(avg), 'loadavg returns array');
assert(avg.length === 3, 'loadavg has 3 elements');
assert(typeof avg[0] === 'number', 'loadavg[0] is number');
assert(typeof avg[1] === 'number', 'loadavg[1] is number');
assert(typeof avg[2] === 'number', 'loadavg[2] is number');

// --- totalmem ---
var total = os.totalmem();
assert(typeof total === 'number', 'totalmem is number');
assert(total > 0, 'totalmem > 0');

// --- freemem ---
var free = os.freemem();
assert(typeof free === 'number', 'freemem is number');
assert(free > 0, 'freemem > 0');
assert(free <= total, 'freemem <= totalmem');

// --- uptime ---
var up = os.uptime();
assert(typeof up === 'number', 'uptime is number');
assert(up > 0, 'uptime > 0');

// --- userInfo ---
var user = os.userInfo();
assert(typeof user === 'object', 'userInfo is object');
assert(typeof user.username === 'string', 'userInfo username is string');
assert(typeof user.uid === 'number', 'userInfo uid is number');
assert(typeof user.gid === 'number', 'userInfo gid is number');
assert(typeof user.homedir === 'string', 'userInfo homedir is string');
// shell can be string or null
assert(user.shell === null || typeof user.shell === 'string',
       'userInfo shell is string or null');

// --- networkInterfaces ---
var ifaces = os.networkInterfaces();
assert(typeof ifaces === 'object', 'networkInterfaces is object');
// Should have at least lo or loopback interface.
var keys = Object.keys(ifaces);
assert(keys.length > 0, 'networkInterfaces has at least one interface');
var firstIface = ifaces[keys[0]][0];
assert(typeof firstIface.address === 'string', 'iface address is string');
assert(typeof firstIface.netmask === 'string', 'iface netmask is string');
assert(firstIface.family === 'IPv4' || firstIface.family === 'IPv6',
       'iface family is IPv4 or IPv6');
assert(typeof firstIface.mac === 'string', 'iface mac is string');
assert(typeof firstIface.internal === 'boolean', 'iface internal is boolean');

// --- endianness ---
var end = os.endianness();
assert(end === 'LE' || end === 'BE', 'endianness is LE or BE');

// --- EOL ---
assert(typeof os.EOL === 'string', 'EOL is string');
assert(os.EOL === '\n' || os.EOL === '\r\n', 'EOL is newline');

// --- devNull ---
assert(typeof os.devNull === 'string', 'devNull is string');

// --- constants ---
assert(typeof os.constants === 'object', 'constants is object');
assert(typeof os.constants.errno === 'object', 'constants.errno is object');
assert(typeof os.constants.signals === 'object', 'constants.signals is object');

// --- availableParallelism ---
var par = os.availableParallelism();
assert(typeof par === 'number', 'availableParallelism is number');
assert(par >= 1, 'availableParallelism >= 1');

// --- getPriority / setPriority ---
var prio = os.getPriority();
assert(typeof prio === 'number', 'getPriority returns number');

console.log('PASS');
