// Copyright (c) Tzvetan Mikov.
// Test that a real npm package (minimist) loads and works correctly.
'use strict';

function assert(cond, msg) {
  if (!cond) throw new Error('Assertion failed: ' + msg);
}
function assertEqual(a, b, msg) {
  if (a !== b) throw new Error('Assertion failed: ' + msg + ' (' + JSON.stringify(a) + ' !== ' + JSON.stringify(b) + ')');
}
function assertDeepEqual(a, b, msg) {
  if (JSON.stringify(a) !== JSON.stringify(b))
    throw new Error('Assertion failed: ' + msg + ' (' + JSON.stringify(a) + ' !== ' + JSON.stringify(b) + ')');
}

// 1. require('minimist') loads the real npm package.
var minimist = require('minimist');
assertEqual(typeof minimist, 'function', 'minimist is a function');

// 2. Basic argument parsing works.
var argv = minimist(['--name', 'hermes', '--count', '42', 'hello', 'world']);
assertEqual(argv.name, 'hermes', 'parsed --name flag');
assertEqual(argv.count, 42, 'parsed --count as number');
assertDeepEqual(argv._, ['hello', 'world'], 'positional args in _');

// 3. Boolean and string options work.
var argv2 = minimist(['-x', '3', '-y', '4', '-n5', '-abc', '--beep=boop'], {
  boolean: ['x'],
  string: ['n']
});
assertEqual(argv2.x, true, '-x is boolean true');
assertEqual(argv2.y, 4, '-y is number 4');
assertEqual(argv2.n, '5', '-n is string "5"');
assertEqual(argv2.a, true, '-a from -abc');
assertEqual(argv2.b, true, '-b from -abc');
assertEqual(argv2.c, true, '-c from -abc');
assertEqual(argv2.beep, 'boop', '--beep=boop');

// 4. Default values work.
var argv3 = minimist([], { default: { x: 10, y: 'hello' } });
assertEqual(argv3.x, 10, 'default x is 10');
assertEqual(argv3.y, 'hello', 'default y is hello');

// 5. Alias option works.
var argv4 = minimist(['-v'], { alias: { v: 'verbose' } });
assertEqual(argv4.v, true, 'alias -v is true');
assertEqual(argv4.verbose, true, 'alias verbose is true');

// 6. -- stops parsing.
var argv5 = minimist(['--foo', 'bar', '--', '--baz', 'qux']);
assertEqual(argv5.foo, 'bar', '--foo is bar');
assert(argv5.baz === undefined, '--baz not parsed after --');
assertDeepEqual(argv5._, ['--baz', 'qux'], 'args after -- in _');

// 7. Module is cached (same reference on second require).
var minimist2 = require('minimist');
assert(minimist === minimist2, 'minimist module is cached');

// 8. require.resolve returns path to the package.
var resolved = require.resolve('minimist');
assert(resolved.endsWith('node_modules/minimist/index.js'), 'resolve path ends with node_modules/minimist/index.js');

// 9. Package has correct metadata in package.json.
var path = require('path');
var pkgJsonPath = path.join(path.dirname(resolved), 'package.json');
var pkgJson = require(pkgJsonPath);
assertEqual(pkgJson.name, 'minimist', 'package.json name is minimist');
assertEqual(pkgJson.version, '1.2.8', 'package.json version is 1.2.8');

console.log('PASS');
