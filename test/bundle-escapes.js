// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.
//
// The two ways a bundled program could still reach the filesystem for
// module code without going through the container, and the fact that
// neither works any more.
//
// Both are about a `require` that is not the one the bundle loader builds.
// The CommonJS wrapper's own `require` parameter shadows globalThis.require
// inside every module, so an ordinary require() never met either of these
// -- which is exactly why they survived: the primary path was closed and
// these were not.

// 1. globalThis.require, the bootstrap loader in libjs/loader.js. It is
// still on the global object (libjs/shims/domain.js requires 'events'
// through it, lazily, so it cannot simply be deleted), and it used to read
// and compile any path it was handed. Three ways to get at it around the
// wrapper parameter, all of which a computed specifier could drive.
//
// secret.js stays on disk, right where the specifier names it, and must not
// run: --implicit-check-not is what asserts that, since a hole here prints
// its line and then carries on happily.
// RUN: rm -rf %t.esc && mkdir -p %t.esc
// RUN: echo "console.log('SECRET RAN'); module.exports = 1;" > %t.esc/secret.js
// RUN: echo "var g = (0, eval)('require');" > %t.esc/cli.js
// RUN: echo "try { g(__dirname + '/secret.js'); console.log('EVAL LOADED'); } catch (e) { console.log('EVAL', e.code); }" >> %t.esc/cli.js
// RUN: echo "try { global.require(__dirname + '/secret.js'); console.log('GLOBAL LOADED'); } catch (e) { console.log('GLOBAL', e.code); }" >> %t.esc/cli.js
// RUN: echo "try { new Function('return require')()(__dirname + '/secret.js'); console.log('FN LOADED'); } catch (e) { console.log('FN', e.code); }" >> %t.esc/cli.js
// RUN: echo "console.log('ESC DONE');" >> %t.esc/cli.js
// RUN: %hermes-node --build-bundle=%t.esc/app.hbb %t.esc/cli.js
// RUN: %hermes-node --bundle=%t.esc/app.hbb | %FileCheck --check-prefix=ESCAPE --implicit-check-not="SECRET RAN" --implicit-check-not=LOADED %s
// ESCAPE: EVAL MODULE_NOT_FOUND
// ESCAPE-NEXT: GLOBAL MODULE_NOT_FOUND
// ESCAPE-NEXT: FN MODULE_NOT_FOUND
// ESCAPE-NEXT: ESC DONE

// Unbundled, the same three still work: what the bundle closes is closed
// for the bundle, not for every run of hermes-node. Without this, "the
// escape is blocked" could be satisfied by breaking globalThis.require
// outright.
// RUN: %hermes-node %t.esc/cli.js | %FileCheck --check-prefix=UNBUNDLED %s
// UNBUNDLED: SECRET RAN
// UNBUNDLED: EVAL LOADED

// 2. Module.createRequire(), which is ordinary CommonJS that real packages
// call. The `require` it returns is Node's own, built around a Module that
// carries a filename and no bundle identity, so every specifier it named
// used to take the "no bundled importer" throw -- including specifiers the
// container holds -- and its resolve() walked the real filesystem.
//
// Nothing here is discoverable by the static scanner (req is not require),
// so the two modules are named with --include; that is the documented
// remedy and not what this case is about.
// RUN: rm -rf %t.cr %t.cr.outside && mkdir -p %t.cr/node_modules/dep %t.cr.outside
// RUN: echo '{ "main": "index.js" }' > %t.cr/node_modules/dep/package.json
// RUN: echo "module.exports = { v: 5 };" > %t.cr/node_modules/dep/index.js
// RUN: echo "module.exports = { v: 9 };" > %t.cr/local.js
// RUN: echo "console.log('OUTSIDE RAN');" > %t.cr.outside/thing.js
// RUN: echo "var req = require('module').createRequire(__filename);" > %t.cr/cli.js
// RUN: echo "console.log('CR', req('./local').v, req('dep').v);" >> %t.cr/cli.js
// RUN: echo "console.log('CR-RESOLVE', req.resolve('./local') === __dirname + '/local.js');" >> %t.cr/cli.js
// RUN: echo "console.log('CR-BUILTIN', typeof req('path').join, req.resolve('path'));" >> %t.cr/cli.js
// RUN: echo "try { req('%t.cr.outside/thing.js'); console.log('CR-LOAD LEAKED'); } catch (e) { console.log('CR-LOAD', e.code); }" >> %t.cr/cli.js
// RUN: echo "try { console.log('CR-RES LEAKED', req.resolve('%t.cr.outside/thing.js')); } catch (e) { console.log('CR-RES', e.code); }" >> %t.cr/cli.js
// RUN: echo "try { require('module').createRequire('/nowhere/at/all/x.js')('./y'); } catch (e) { console.log('CR-ALIEN', e.message.split('\n').slice(1).join(' ~ ')); }" >> %t.cr/cli.js
// RUN: %hermes-node --build-bundle=%t.cr/app.hbb --include=./local --include=dep %t.cr/cli.js
//
// The tree goes, the outside directory stays: a resolve() that still
// answered off the disk would find thing.js and report its path, so the
// file being there is what gives CR-RES its meaning.
// RUN: rm -rf %t.cr/node_modules %t.cr/local.js %t.cr/cli.js
// RUN: %hermes-node --bundle=%t.cr/app.hbb | %FileCheck --check-prefix=CREATEREQ --implicit-check-not="OUTSIDE RAN" --implicit-check-not=LEAKED %s
// CREATEREQ: CR 9 5
// CREATEREQ-NEXT: CR-RESOLVE true
// CREATEREQ-NEXT: CR-BUILTIN function path
// CREATEREQ-NEXT: CR-LOAD MODULE_NOT_FOUND
// CREATEREQ-NEXT: CR-RES MODULE_NOT_FOUND
//
// A createRequire() rooted outside the bundle names nothing the container
// can place, and says so: identityOf() derives an identity from a filename
// under the root and refuses one from anywhere else, so this must not be
// reported as coming from some module that never asked.
//
// It is also the one shape where no --include value can be computed: the
// request is relative and there is no importer identity to make it
// relative to. The message says where --include resolves from instead of
// printing a value that would fail -- see test/bundle-include.js for the
// case where a value can be computed.
// CREATEREQ-NEXT: CR-ALIEN required by <no bundled importer> ~ Not in the bundle. Add it with --include, whose value is ~ resolved from the entry's directory.

// 3. globalThis.__closeDiskModuleLoading itself. installBundleLoader() calls
// it before the bundle's entry module runs, but a -r/--require preload used
// to run even earlier -- before the bundle loader was installed at all --
// so a preload that replaced the closer with a no-op used to reopen escape
// #1 for the rest of the run, silently (hardened in dedb781 by making the
// property non-writable). The bundle-preload round closed the phase itself
// instead: -r is refused outright with --bundle (see
// tools/hermes-node/hermes-node.cpp's checkToolOptions() and
// test/bundle-errors.js NORFLAG for the general case), so there is no
// preload left to run before the closer fires, and this specific reopening
// is unreachable rather than merely defended against. Reusing
// %t.esc/secret.js and its container from case #1 to pin exactly that: the
// same -r invocation that used to reopen the escape now never starts the
// bundle at all.
// RUN: echo "globalThis.__closeDiskModuleLoading = function() {};" > %t.esc/pre.js
// RUN: %not %hermes-node --bundle=%t.esc/app.hbb -r %t.esc/pre.js 2>&1 | %FileCheck --check-prefix=PRELOAD-REFUSED --implicit-check-not="SECRET RAN" --implicit-check-not=LOADED %s
// PRELOAD-REFUSED: --bundle cannot be combined with -r or --require
