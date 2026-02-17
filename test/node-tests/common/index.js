// Copyright (c) Tzvetan Mikov.
// Minimal shim of Node.js test/common/index.js for hermes-node.
// Provides just enough of the test harness for Node fs tests to run.
'use strict';

var process = globalThis.process;
var assert = require('assert');
var fs = require('fs');
var path = require('path');
var util = require('util');

var isWindows = process.platform === 'win32';
var isLinux = process.platform === 'linux';
var isMacOS = process.platform === 'darwin';

var noop = function() {};

// --- mustCall / mustSucceed tracking ---
var mustCallChecks = [];

function runCallChecks(exitCode) {
  if (exitCode !== 0) return;

  var failed = mustCallChecks.filter(function(context) {
    if ('minimum' in context) {
      context.messageSegment = 'at least ' + context.minimum;
      return context.actual < context.minimum;
    }
    context.messageSegment = 'exactly ' + context.exact;
    return context.actual !== context.exact;
  });

  failed.forEach(function(context) {
    console.log('Mismatched %s function calls. Expected %s, actual %d.',
                context.name,
                context.messageSegment,
                context.actual);
    if (context.stack) {
      var lines = context.stack.split('\n');
      console.log(lines.slice(2).join('\n'));
    }
  });

  if (failed.length) process.exit(1);
}

function _mustCallInner(fn, criteria, field) {
  if (criteria === undefined) criteria = 1;
  if (typeof fn === 'number') {
    criteria = fn;
    fn = noop;
  } else if (fn === undefined) {
    fn = noop;
  }

  if (typeof criteria !== 'number')
    throw new TypeError('Invalid ' + field + ' value: ' + criteria);

  var context = {
    actual: 0,
    stack: (new Error()).stack || '',
    name: fn.name || '<anonymous>',
  };
  context[field] = criteria;

  // Add the exit listener only once
  if (mustCallChecks.length === 0) process.on('exit', runCallChecks);

  mustCallChecks.push(context);

  var _return = function() {
    context.actual++;
    return fn.apply(this, arguments);
  };
  Object.defineProperties(_return, {
    name: { value: fn.name, writable: false, enumerable: false, configurable: true },
    length: { value: fn.length, writable: false, enumerable: false, configurable: true },
  });
  return _return;
}

function mustCall(fn, exact) {
  return _mustCallInner(fn, exact, 'exact');
}

function mustSucceed(fn, exact) {
  return mustCall(function(err) {
    assert.ifError(err);
    if (typeof fn === 'function')
      return fn.apply(this, Array.prototype.slice.call(arguments, 1));
  }, exact);
}

function mustCallAtLeast(fn, minimum) {
  return _mustCallInner(fn, minimum, 'minimum');
}

function mustNotCall(msg) {
  return function mustNotCall() {
    var argsInfo = arguments.length > 0 ?
      '\ncalled with arguments: ' + Array.prototype.map.call(arguments, function(arg) {
        return util.inspect(arg);
      }).join(', ') : '';
    assert.fail((msg || 'function should not have been called') + argsInfo);
  };
}

// --- mustNotMutateObjectDeep ---
function mustNotMutateObjectDeep(original) {
  if (original === null || typeof original !== 'object') {
    return original;
  }
  // Use Proxy to detect mutations
  return new Proxy(original, {
    defineProperty: function(target, property) {
      assert.fail('Expected no side effects, got ' + String(property) + ' defined');
    },
    deleteProperty: function(target, property) {
      assert.fail('Expected no side effects, got ' + String(property) + ' deleted');
    },
    get: function(target, prop, receiver) {
      return mustNotMutateObjectDeep(Reflect.get(target, prop, receiver));
    },
    set: function(target, property, value) {
      assert.fail('Expected no side effects, got ' + String(value) + ' assigned to ' + String(property));
    },
  });
}

// --- skip / printSkipMessage ---
function printSkipMessage(msg) {
  console.log('1..0 # Skipped: ' + msg);
}

function skip(msg) {
  printSkipMessage(msg);
  process.exit(0);
}

// --- expectWarning ---
var catchWarning;

function _expectWarning(name, expected, code) {
  if (typeof expected === 'string') {
    expected = [[expected, code]];
  } else if (!Array.isArray(expected)) {
    expected = Object.entries(expected).map(function(entry) { return [entry[1], entry[0]]; });
  } else if (expected.length !== 0 && !Array.isArray(expected[0])) {
    expected = [[expected[0], expected[1]]];
  }
  return mustCall(function(warning) {
    var expectedProperties = expected.shift();
    if (!expectedProperties) {
      assert.fail('Unexpected extra warning received: ' + warning);
    }
    var message = expectedProperties[0];
    var code = expectedProperties[1];
    assert.strictEqual(warning.name, name);
    if (typeof message === 'string') {
      assert.strictEqual(warning.message, message);
    } else {
      assert.match(warning.message, message);
    }
    assert.strictEqual(warning.code, code);
  }, expected.length);
}

function expectWarning(nameOrMap, expected, code) {
  if (catchWarning === undefined) {
    catchWarning = {};
    process.on('warning', function(warning) {
      if (!catchWarning[warning.name]) {
        throw new TypeError('"' + warning.name + '" was triggered without being expected.\n' +
          util.inspect(warning));
      }
      catchWarning[warning.name](warning);
    });
  }
  if (typeof nameOrMap === 'string') {
    catchWarning[nameOrMap] = _expectWarning(nameOrMap, expected, code);
  } else {
    Object.keys(nameOrMap).forEach(function(name) {
      catchWarning[name] = _expectWarning(name, nameOrMap[name]);
    });
  }
}

// --- expectsError ---
function expectsError(validator, exact) {
  return mustCall(function() {
    if (arguments.length !== 1) {
      assert.fail('Expected one argument, got ' + util.inspect(Array.from(arguments)));
    }
    var error = arguments[0];
    assert.throws(function() { throw error; }, validator);
    return true;
  }, exact);
}

// --- invalidArgTypeHelper ---
function invalidArgTypeHelper(input) {
  if (input == null) {
    return ' Received ' + input;
  }
  if (typeof input === 'function') {
    return ' Received function ' + input.name;
  }
  if (typeof input === 'object') {
    if (input.constructor && input.constructor.name) {
      return ' Received an instance of ' + input.constructor.name;
    }
    return ' Received ' + util.inspect(input, { depth: -1 });
  }

  var inspected = util.inspect(input, { colors: false });
  if (inspected.length > 28) { inspected = inspected.slice(0, 25) + '...'; }

  return ' Received type ' + typeof input + ' (' + inspected + ')';
}

// --- allowGlobals (no-op in our env) ---
function allowGlobals() {}

// --- runWithInvalidFD ---
function runWithInvalidFD(func) {
  var fd = 1 << 30;
  try {
    while (fs.fstatSync(fd--) && fd > 0) { /* empty */ }
  } catch (e) {
    return func(fd);
  }
  printSkipMessage('Could not generate an invalid fd');
}

// --- platformTimeout ---
function platformTimeout(ms) {
  return ms;
}

// --- canCreateSymLink ---
function canCreateSymLink() {
  return !isWindows;
}

// --- pwdCommand ---
var pwdCommand = isWindows ?
  ['cmd.exe', ['/d', '/c', 'cd']] :
  ['pwd', []];

// --- escapePOSIXShell ---
function escapePOSIXShell(cmdParts) {
  var args = [];
  for (var i = 1; i < arguments.length; i++) {
    args.push(arguments[i]);
  }
  // On POSIX shells, pass values via env to avoid shell escaping issues.
  var env = {};
  var keys = Object.keys(process.env);
  for (var k = 0; k < keys.length; k++) {
    env[keys[k]] = process.env[keys[k]];
  }
  var cmd = cmdParts[0];
  for (var i = 0; i < args.length; i++) {
    var envVarName = 'ESCAPED_' + i;
    env[envVarName] = args[i];
    cmd += '${' + envVarName + '}' + cmdParts[i + 1];
  }
  return [cmd, { env: env }];
}

// --- getArrayBufferViews ---
function getArrayBufferViews(buf) {
  var start = buf.byteOffset;
  var end = buf.byteOffset + buf.byteLength;
  var ab = buf.buffer;
  return [
    new Int8Array(ab, start, end - start),
    new Uint8Array(ab, start, end - start),
    new Uint8ClampedArray(ab, start, end - start),
    new Int16Array(ab, start, (end - start) / 2),
    new Uint16Array(ab, start, (end - start) / 2),
    new Int32Array(ab, start, (end - start) / 4),
    new Uint32Array(ab, start, (end - start) / 4),
    new Float32Array(ab, start, (end - start) / 4),
    new Float64Array(ab, start, (end - start) / 8),
    new DataView(ab, start, end - start),
  ];
}

// --- localhostIPv4 ---
var localhostIPv4 = '127.0.0.1';

// --- PIPE (Unix domain socket path for tests) ---
var tmpdir = require('./tmpdir');
var PIPE = (function() {
  var localRelative = path.relative(process.cwd(), tmpdir.path + '/');
  var pipeName = 'node-test.' + process.pid + '.sock';
  return localRelative + pipeName;
})();

// --- nodeLibPath: project root for --node-lib-path ---
// Tests live in <root>/test/node-tests/parallel/, so 3 levels up.
var nodeLibPath = path.resolve(__dirname, '../../..');

// --- spawnArgs: construct args for spawning hermes-node children ---
// Returns an array like ['--node-lib-path', '<root>', scriptPath, ...extra].
function spawnArgs(scriptPath) {
  var extra = [];
  for (var i = 1; i < arguments.length; i++) {
    extra.push(arguments[i]);
  }
  return ['--node-lib-path', nodeLibPath, scriptPath].concat(extra);
}

// --- spawnCmd: construct shell command for exec() with hermes-node children ---
function spawnCmd(scriptPath) {
  var extra = [];
  for (var i = 1; i < arguments.length; i++) {
    extra.push(arguments[i]);
  }
  var parts = [process.execPath, '--node-lib-path', nodeLibPath, scriptPath];
  return parts.concat(extra).join(' ');
}

var common = {
  allowGlobals: allowGlobals,
  canCreateSymLink: canCreateSymLink,
  escapePOSIXShell: escapePOSIXShell,
  expectsError: expectsError,
  expectWarning: expectWarning,
  getArrayBufferViews: getArrayBufferViews,
  invalidArgTypeHelper: invalidArgTypeHelper,
  isLinux: isLinux,
  isMacOS: isMacOS,
  isWindows: isWindows,
  localhostIPv4: localhostIPv4,
  mustCall: mustCall,
  mustCallAtLeast: mustCallAtLeast,
  mustNotCall: mustNotCall,
  mustNotMutateObjectDeep: mustNotMutateObjectDeep,
  mustSucceed: mustSucceed,
  nodeLibPath: nodeLibPath,
  PIPE: PIPE,
  platformTimeout: platformTimeout,
  printSkipMessage: printSkipMessage,
  pwdCommand: pwdCommand,
  runWithInvalidFD: runWithInvalidFD,
  skip: skip,
  spawnArgs: spawnArgs,
  spawnCmd: spawnCmd,

  get isAIX() { return false; },
  get isIBMi() { return false; },
  get isFreeBSD() { return process.platform === 'freebsd'; },
  get isSunOS() { return process.platform === 'sunos'; },
  get isOpenBSD() { return process.platform === 'openbsd'; },
  get isDebug() { return false; },
  get isASan() { return false; },
  get hasCrypto() { return false; },
  get hasIntl() { return false; },
};

module.exports = common;
