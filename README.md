# hermes-node

A JavaScript and TypeScript runtime with Node.js compatibility, built on
[Hermes](https://github.com/facebook/hermes). Ships as a single binary.

Run `.js` or `.ts` files directly. Many Node packages work unchanged.
TypeScript runs without a separate build step. A Chrome DevTools debugger
comes built in.

> **Note.** This repository is AI slop. Not a single line was typed by a
> human; most have never been read by one either. It is a collaboration
> between several versions of Claude Opus and ChatGPT Codex, with a human
> supplying direction and the occasional thumbs-up. The human disclaims
> responsibility for the quality, while conceding that without the AI,
> none of this would have existed in the first place.

## Status

Early and experimental. Lots works, lots doesn't. Useful for trying things
and filing bugs, not for production.

The bundled Node.js JavaScript libraries come from Node.js v24.13.0, and
`process.version` reports the same. Override with `--node-version` if a
package gates behavior on it.

### Highlights

- TypeScript runs directly. Types are stripped by Hermes at parse time. No
  `tsc`, no loader, no flag. See [TypeScript](#typescript) below.
- The inspector serves its own DevTools frontend. `--inspect` starts a Chrome
  DevTools Protocol server and serves the UI from the same port; open the
  printed URL in any Chromium-based browser to attach. See
  [Debugging](#debugging) below.
- Recognizable Node API surface: Node's own `lib/*.js` for the JS side, with
  the native bindings ported on top of Hermes Node-API.

### Two caveats

**1. The first run pays for compilation; later runs don't.**

Hermes is an AOT compiler, not a JIT. Your script and every non-builtin
module it requires get compiled to bytecode before they run, and the
optimizer that makes that bytecode fast was meant to run once at build time,
not on every startup. (Built-in Node modules escape this. They're compiled
when the binary is built and embedded in it.)

So the bytecode goes to disk, in `~/.cache/hermes-node/compile-cache` by
default, and later runs reuse it. Since it gets reused, optimizing it is
worth the wait, and optimization is on by default whenever the cache is.
Turn the cache off and optimization goes off too, so a one-shot run isn't
punished for it.

The `examples/flow-bundler` workload requires about 1500 distinct files:

| | wall clock |
|---|---|
| `--no-compile-cache` | 6.0 s |
| first run, populating the cache | 7.4 s |
| every run after that | 2.05 s |

First run costs about 23% extra. Every run after is roughly 3x faster. It
starts paying at run two, so the only losing case is code you run exactly
once.

Read that table carefully, though: the rows differ in two settings, not one,
because the two move together by default. Pin `--optimize` and the parts
separate. Optimizing alone adds about 3.0 s to the first run and saves about
0.67 s on each one after (`--optimize=off` gives 4.5 s then 2.8 s, against
7.4 s then 2.05 s with it on).

It's still not where it should be. Each module compiles as its own unit, so
the optimizer never sees across a file boundary and can't inline between
files. Thousands of tiny `node_modules` files is about the worst input you
can hand it; the same code rolled into one bundle optimizes much better.
Improving that is on the roadmap.

The cache key is the file path plus a checksum of the source, so editing one
file recompiles that file and leaves the rest of the tree alone. See
[Compile cache](#compile-cache) for the controls.

**2. Hermes isn't V8.**

`hermes-node` won't match V8's TurboFan on hot untyped JavaScript, cache or
no cache. Hermes is a bytecode interpreter (with a small arm64 JIT) built
for fast startup, predictable memory, and a small footprint. Long-running
untyped workloads are V8's home turf, not Hermes's.

That said, `hermes-node` is built directly on Hermes, so future Hermes
capabilities flow through to it. When
[Static Hermes](https://github.com/facebook/hermes/blob/static_h/doc/TypedLanguage.md)
is released, hermes-node will pick it up: code written in the statically
typed JavaScript dialect will run with substantially higher performance.

## Install

Download the latest release for your platform from the
[Releases page](../../releases/latest).

Available artifacts:

- `hermes-node-<version>-linux-x64.tar.gz` (Linux x86_64)
- `hermes-node-<version>-macos-universal.tar.gz` (macOS x86_64 + arm64,
  universal)
- `SHA256SUMS`

No Windows build yet.

### Linux

```sh
tar xzf hermes-node-*-linux-x64.tar.gz
cd hermes-node-*-linux-x64
./hermes-node --version
```

### macOS

The macOS binary is a universal binary (runs natively on both Intel and
Apple Silicon). It is ad-hoc codesigned but not notarized, so Gatekeeper
will block it on first launch with "cannot be opened because the developer
cannot be verified."

```sh
tar xzf hermes-node-*-macos-universal.tar.gz
cd hermes-node-*-macos-universal
xattr -d com.apple.quarantine ./hermes-node    # one-time
./hermes-node --version
```

Alternatively, find `hermes-node` in Finder, right-click → Open → Open in
the dialog.

## Quick start

```sh
# Run a script
./hermes-node script.js

# Evaluate code
./hermes-node -e 'console.log("hello from hermes")'

# REPL
./hermes-node
```

A minimal example:

```js
// hello.js
const fs = require('fs');
const path = require('path');

const files = fs.readdirSync('.');
for (const name of files) {
  const st = fs.statSync(path.resolve(name));
  console.log(`${st.isDirectory() ? 'D' : 'F'} ${name}`);
}
```

```sh
./hermes-node hello.js
```

## TypeScript

`.ts` files run directly. There is no separate transpile step and no flag
to enable it.

```ts
// greet.ts
function greet(name: string): string {
  return `hello, ${name}`;
}

console.log(greet('hermes'));
```

```sh
./hermes-node greet.ts
```

`require()` also resolves `.ts`:

```js
// index.js
const { greet } = require('./greet');  // finds greet.ts
console.log(greet('world'));
```

This is type stripping, not type checking. Hermes's parser erases the types
before compilation; `tsc` never runs and `.d.ts` files are not consulted.
For real type checking, run `tsc --noEmit` in your build pipeline as usual;
`hermes-node` just runs the result.

TypeScript features that need *emit* are not supported, because nothing is
emitted: the parser just deletes type annotations. That rules out enums with
reverse mappings, `namespace` with runtime members, parameter property
assignment, and `experimentalDecorators` emit.

## Debugging

`hermes-node` ships with Chrome DevTools Protocol support and serves the
DevTools frontend itself, so you don't need to install anything to debug.

Start a script with the inspector attached:

```sh
./hermes-node --inspect script.js
```

You'll see:

```
Debugger listening on ws://127.0.0.1:9229/<session-id>
For help, see: https://nodejs.org/en/docs/inspector
Open DevTools: http://127.0.0.1:9229/devtools/inspector.html?ws=127.0.0.1:9229/<session-id>
```

Open the "Open DevTools:" URL in any Chromium-based browser. You get the
full DevTools experience: breakpoints, stepping, console, heap, source maps.

Other inspector flags:

- `--inspect-brk` pauses on the first line of your script so you can set
  breakpoints before anything runs.
- `--inspect-open` automatically launches your system browser to the
  DevTools URL once the inspector is ready.
- `--inspect=8080` or `--inspect=0.0.0.0:9229` chooses a custom port or
  bind address. Same syntax for `--inspect-brk`.

Typical debug-from-scratch invocation:

```sh
./hermes-node --inspect-brk --inspect-open script.js
```

That pauses your script on line 1, opens your browser to the DevTools URL,
and leaves you ready to set breakpoints.

## Compile cache

On by default. Compiled bytecode for your script and anything under
`node_modules` goes in `$XDG_CACHE_HOME/hermes-node/compile-cache`, or
`~/.cache/hermes-node/compile-cache` if that variable isn't set. Built-in
modules are unaffected, since they're already bytecode inside the binary.

| | |
|---|---|
| `--compile-cache=<dir>` | Use a different directory |
| `--no-compile-cache` | Turn it off |
| `--optimize=on\|off\|default` | Override the optimizer. `default` is on with the cache, off without |
| `HERMES_NODE_COMPILE_CACHE=<dir>` | Same as `--compile-cache` |
| `HERMES_NODE_DISABLE_COMPILE_CACHE=1` | Same as `--no-compile-cache` |
| `HERMES_NODE_DEBUG_NATIVE=COMPILE_CACHE` | Log every hit and miss to stderr |

There are no `NODE_*` equivalents. `NODE_COMPILE_CACHE`,
`module.enableCompileCache()` and `getCompileCacheDir()` do nothing here, on
purpose: this cache holds Hermes bytecode, not V8 code cache data, and
pretending otherwise would only mislead code that checks.

`--inspect` and `--inspect-brk` turn the cache off. Cached entries are
compiled without the full debug info the debugger needs to set breakpoints
anywhere, so `--optimize=on` is rejected alongside the inspector too.

A stale entry can't produce wrong results. Each one stores the length and
checksum of the source it came from, and a mismatch counts as a miss.
Deleting the cache directory is always safe.

## Command-line options

```
Usage: hermes-node [options] [script.js] [-- script-args...]

Options:
  -e, --eval <code>              Evaluate code
  --inspect[=[host:]port]        Enable inspector (default 127.0.0.1:9229)
  --inspect-brk[=[host:]port]    Enable inspector, break before user code
  --compile-cache=<dir>          Bytecode cache directory
  --no-compile-cache             Disable the bytecode cache
  --optimize=<default|on|off>    Optimize compiled code. default is on
                                 with the cache, off without it
  --inspect-open                 Open the DevTools URL in the system browser
  --node-version <version>       Override process.version (e.g. v24.13.0)
  -r, --require <module>         Preload a module before the script (repeatable)
  -v, --version                  Print the hermes-node version and exit
  -h, --help                     Show this help
```

Arguments after `--` are passed through to the script and visible in
`process.argv`:

```sh
./hermes-node script.js -- --my-flag foo bar
```

## What works

A non-exhaustive list of core modules that load and have working basic
functionality:

- `assert`, `buffer`, `console`, `events`, `path`, `process`, `stream`,
  `string_decoder`, `timers`, `util`, `querystring`
- `fs` (sync and async) and `fs.promises`
- `os`, `url` (including `URL` / `URLSearchParams` globals)
- `dns` (`lookup` and `resolve*`)
- `net` (TCP and Unix domain sockets)
- `http` (server and client)
- `child_process` (`spawn`, `spawnSync`, and friends)
- `tty`, REPL
- `process.stdin` / `stdout` / `stderr` as proper streams

Coverage gets thinner past that. Notable things that don't work yet, or only
partially:

- `worker_threads`, `cluster` (single-threaded only)
- `crypto`, `tls`, `https` (the latter two are stub modules that throw on
  any use, since there is no TLS implementation)
- `Atomics`, `AbortSignal` / `AbortController` globals
- V8-API native addons (those written against `v8.h` / NAN). Node-API
  addons are supported, see [Limitations](#limitations).

This list will move; check the issue tracker for current state.

## Limitations

- **First-run compilation.** Code that isn't in the compile cache yet has to
  be compiled, with optimizations, before it runs. See
  [Compile cache](#compile-cache).
- **No JIT exposed.** Hermes has an arm64 JIT but `hermes-node` doesn't
  wire it up today.
- **Subset of Node.** Whether a package works depends on which core modules
  it touches. A package using only supported modules (see
  [What works](#what-works)) will likely run. One that pulls in `crypto`,
  `tls`, `worker_threads`, etc., will not.
- **Native addons.** Node-API (N-API) addons work: `process.dlopen` is
  wired up and the NAPI symbols are exported from the binary, so a `.node`
  file built against Node-API loads and runs. Older addons that use V8's
  C++ API directly (`v8.h`, NAN) don't work and never will, since there is
  no V8.
- **Single-threaded.** No `worker_threads`, no `cluster`.

## Roadmap

In rough priority order:

- AOT pre-compilation flow for `node_modules` trees, so even the first run
  doesn't have to compile
- More bindings: `crypto`, `tls`
- Filling in gaps in already-supported modules

## How it works

`hermes-node` reuses Node.js's own JavaScript libraries (`lib/*.js` from a
recent Node release) and ports Node's native bindings on top of Hermes
Node-API. The built-in JS is compiled to Hermes bytecode at build time and
embedded in the binary, so the runtime is self-contained: no separate `.js`
files to ship, no `node_modules`-style internal tree at runtime.

## License

MIT. See [LICENSE](LICENSE) and [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md)
for vendored dependencies (Hermes, libuv, c-ares, llhttp, simdutf, Ada, …).
