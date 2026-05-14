# Phase 5 — Networking: Detailed Implementation Plan

Extends the hermes-node-compat project from file system support (Phase 4, Steps 1–33) through
full TCP/UDP networking and HTTP. Standalone CLI only.

## Notation

- `[depends: N5.3, N5.5]` — cannot start until those steps are complete
- `[native]` — requires writing C++ (Node-API) code
- `[js]` — requires writing JavaScript
- `[cmake]` — build system changes
- `[vendor]` — vendoring external dependency
- `[verify]` — testing/verification milestone

---

## Overview

Phase 5 adds:
1. Supporting infrastructure: `os` module, `credentials` binding, `tty_wrap`, simdutf/Ada integration
2. DNS resolution: `dns.lookup()` via libuv, `dns.resolve*()` via c-ares
3. Stream infrastructure: HandleWrap/StreamBase class hierarchy for native streams
4. TCP/IPC networking: `net` module (client + server)
5. UDP: `dgram` module
6. HTTP: `http` module via llhttp parser
7. Child processes: `child_process` module

TLS/HTTPS is deferred to a later phase (requires OpenSSL, security-sensitive).

## Background: Node's Native Stream Architecture

Node's networking is built on a C++ class hierarchy:

```
AsyncWrap (async resource tracking — stubbed for us)
  └─ HandleWrap (uv_handle_t lifecycle: ref/unref/close)
       └─ LibuvStreamWrap (uv_stream_t: readStart/readStop/write*/shutdown)
            ├─ TCPWrap (uv_tcp_t: bind/listen/connect)
            ├─ PipeWrap (uv_pipe_t: bind/listen/connect)
            └─ TTYWrap (uv_tty_t: setRawMode/getWindowSize)
```

All stream types share the same JS-visible methods (`readStart`, `readStop`, `writeBuffer`,
`writeUtf8String`, `writeLatin1String`, `writeAsciiString`, `writev`, `shutdown`) from
`StreamBase::AddMethods()`. The JS layer (`internal/stream_base_commons.js`) calls these
methods on the handle object.

**Our approach:** Implement equivalent C++ base classes using NAPI. A `HandleWrapBase` provides
ref/unref/close. A `LibuvStreamBase` provides the read/write methods. TCP/Pipe/TTY inherit.
Each wrap uses `napi_wrap` to associate native data with the JS constructor instance.

---

## A. Supporting Infrastructure

### Step N5.1: Port `os` binding [native] [depends: —]

**Source:** `src/node_os.cc`
**Binding name:** `internalBinding('os')`

The `os` module is used by networking code (`net.js` uses `os.hostname()` indirectly, tests use
`os.tmpdir()`, `os.networkInterfaces()` is essential for network testing).

Functions to implement (all via libuv):

| Function | libuv call | Notes |
|---|---|---|
| `getHostname(ctx)` | `uv_os_gethostname` | ctx-based error reporting |
| `getOSInformation(ctx)` | `uv_os_uname` | returns [sysname, release, version] |
| `getLoadAvg(arr)` | `uv_loadavg` | writes 3 doubles into Float64Array |
| `getUptime(ctx)` | `uv_uptime` | |
| `getCPUs()` | `uv_cpu_info` | returns array of {model, speed, times} |
| `getFreeMem()` | `uv_get_free_memory` | |
| `getTotalMem()` | `uv_get_total_memory` | |
| `getHomeDirectory(ctx)` | `uv_os_homedir` | |
| `getUserInfo(ctx, options)` | `uv_os_get_passwd` | returns {uid, gid, username, homedir, shell} |
| `getInterfaceAddresses(ctx)` | `uv_interface_addresses` | returns {name -> [{address, netmask, family, mac, internal, cidr}]} |
| `setPriority(pid, priority, ctx)` | `uv_os_setpriority` | |
| `getPriority(pid, ctx)` | `uv_os_getpriority` | |
| `getAvailableParallelism()` | `uv_available_parallelism` | |
| `isBigEndian` | compile-time | boolean property |

Also set on exports: `isBigEndian` (boolean).

**Error reporting:** Most functions use ctx-based error reporting — write errno/syscall/message
to a ctx object on failure, return undefined; the JS side throws `ERR_SYSTEM_ERROR`.

**Test:** JS test via `hermes-node`:
```js
const os = require('os');
assert(typeof os.hostname() === 'string');
assert(typeof os.type() === 'string');
assert(typeof os.release() === 'string');
assert(typeof os.platform() === 'string');
assert(os.platform() === process.platform);
assert(typeof os.tmpdir() === 'string');
assert(typeof os.homedir() === 'string');
assert(os.cpus().length > 0);
assert(typeof os.cpus()[0].model === 'string');
assert(typeof os.loadavg() === 'object' && os.loadavg().length === 3);
assert(typeof os.totalmem() === 'number' && os.totalmem() > 0);
assert(typeof os.freemem() === 'number' && os.freemem() > 0);
assert(typeof os.uptime() === 'number' && os.uptime() > 0);
assert(typeof os.userInfo().username === 'string');
const ifaces = os.networkInterfaces();
assert(typeof ifaces === 'object');
assert(typeof os.EOL === 'string');
console.log('PASS');
```

### Step N5.2: Port `credentials` binding [native] [depends: —]

**Source:** `src/node_credentials.cc`
**Binding name:** `internalBinding('credentials')`

The `os` module's `os.tmpdir()` calls `getTempDir()` from the credentials binding. Additionally,
`os.userInfo()` uses `getUserInfo()` (from os binding, not credentials), but `getuid`/`getgid`
etc. are on credentials.

Functions to implement:

| Function | Implementation |
|---|---|
| `getTempDir(ctx)` | Check `TMPDIR`/`TMP`/`TEMP` env vars, fall back to `/tmp` |
| `safeGetenv(key)` | `getenv()` (simplified — Node checks if running as root) |
| `getuid()` | `getuid()` |
| `geteuid()` | `geteuid()` |
| `getgid()` | `getgid()` |
| `getegid()` | `getegid()` |
| `getgroups()` | `getgroups()` |

Mutating functions (`setuid`, `setgid`, `initgroups`, etc.) can be deferred or stubbed since
they are rarely used in normal application code.

**Test:** JS test verifying `getTempDir()` returns a string, `getuid()` returns a number, etc.

### Step N5.3: Port `tty_wrap` binding [native] [depends: N5.6]

**Source:** `src/tty_wrap.cc`
**Binding name:** `internalBinding('tty_wrap')`

Needed by `lib/tty.js`, which is used by process.stdout/stderr upgrade and by `internal/readline`.
The TTY wraps `uv_tty_t` and inherits from our LibuvStreamBase.

Exports:
- `TTY` constructor — wraps `uv_tty_t`, takes `(fd, reading)` args
- `isTTY(fd)` — static, returns boolean via `uv_guess_handle`
- Prototype methods (inherited from LibuvStreamBase): `readStart`, `readStop`, `writeBuffer`,
  `writeUtf8String`, etc.
- Own methods: `getWindowSize(out)` — writes [cols, rows] to array,
  `setRawMode(mode)` — `uv_tty_set_mode`

**Depends on:** HandleWrap + LibuvStreamBase infrastructure (Step N5.6).

**Test:** JS test:
```js
const { TTY, isTTY } = internalBinding('tty_wrap');
assert(typeof isTTY === 'function');
assert(typeof isTTY(0) === 'boolean');
assert(typeof TTY === 'function');
// Only construct if actually a TTY
if (isTTY(1)) {
  const handle = new TTY(1, false);
  const size = [0, 0];
  handle.getWindowSize(size);
  assert(size[0] > 0 && size[1] > 0);
  handle.close();
}
console.log('PASS');
```

### Step N5.4: Integrate simdutf into buffer/encoding bindings [native] [depends: —]

**Source:** `external/simdutf/` (already vendored)
**TODO item:** `history/initial/TODO.md` — "Removed dependencies on Ada and Simdutf should be put back"

Replace our hand-rolled implementations in the buffer and encoding bindings with simdutf calls
for:
- UTF-8 validation (`simdutf::validate_utf8`)
- ASCII validation (`simdutf::validate_ascii`)
- UTF-8 byte length of Latin-1 (`simdutf::utf8_length_from_latin1`)
- Base64 encode/decode (`simdutf::binary_to_base64`, `simdutf::base64_to_binary`)
- UTF-8 ↔ UTF-16 transcoding (for `ucs2Slice`/`ucs2Write`)

This matches the project philosophy: "vendor and use that same library rather than hand-rolling."

**Files:** Modify `lib/bindings/node_buffer.cpp`, `lib/bindings/node_encoding.cpp`,
`lib/bindings/CMakeLists.txt` (add `simdutf` link).

**Test:** All existing buffer and encoding tests must continue to pass. Add specific tests for
edge cases in simdutf code paths (empty strings, max-length buffers, invalid base64).

### Step N5.5: Integrate Ada into url binding [native] [depends: —]

**Source:** `external/ada/` (already vendored)
**Binding name:** `internalBinding('url')` — currently a stub shim

Replace the stub `libjs/shims/internal/url.js` with a proper implementation backed by the Ada
URL parser. This is needed by `http` (URL parsing), `net` (host/port parsing), and `dns`.

Key functions to implement:
- `domainToASCII(domain)` — IDNA conversion via `ada::idna::to_ascii`
- `domainToUnicode(domain)` — via `ada::idna::to_unicode`
- `URL` class — `parse(input, base)`, `canParse(input, base)`, getters for href/origin/protocol/
  username/password/host/hostname/port/pathname/search/hash
- `pathToFileURL(path)`, `fileURLToPath(url)` — already have stubs, make them correct
- `toPathIfFileURL(url)` — already has stub

**Approach:** Create a native `url` binding (`lib/bindings/node_url.cpp`) that wraps Ada's C++
API. The JS shim `libjs/shims/internal/url.js` is replaced with a thinner shim that delegates
to the native binding for parsing, keeping `toPathIfFileURL` logic in JS.

Alternatively, since `internal/url.js` in Node.js is a large file with many dependencies, we
can implement just the native binding and keep our URL shim but wire in the Ada-backed functions
for `domainToASCII`/`domainToUnicode` and URL parsing. The HTTP module primarily needs hostname
parsing, not the full URL API.

**Test:**
```js
const url = require('url');
// Basic URL parsing
const u = new URL('http://example.com:8080/path?query=1#hash');
assert(u.hostname === 'example.com');
assert(u.port === '8080');
assert(u.pathname === '/path');
// domainToASCII
assert(typeof url.domainToASCII === 'function');
console.log('PASS');
```

---

## B. DNS Resolution

### Step N5.6: Implement HandleWrap + LibuvStreamBase infrastructure [native] [depends: —]

**Source:** `src/handle_wrap.cc`, `src/stream_wrap.cc`, `src/stream_base.cc`

This is the most architecturally significant step. All native stream types (TCP, Pipe, TTY, UDP)
need a common base for handle lifecycle and stream I/O. We implement this as reusable C++ classes
using NAPI.

**HandleWrapBase** (wraps any `uv_handle_t`):
- `ref()` / `unref()` / `hasRef()` — `uv_ref` / `uv_unref`
- `close(callback?)` — `uv_close` with optional JS callback
- GC destructor via `napi_wrap` — closes handle if not already closed
- `getAsyncId()` — stub returning 0 (async hooks not implemented)
- Prevent-GC ref while handle is active

**LibuvStreamBase** (wraps any `uv_stream_t`, inherits HandleWrapBase):
- `readStart()` — `uv_read_start` with alloc callback + read callback
- `readStop()` — `uv_read_stop`
- `writeBuffer(req, buffer)` — `uv_write` with Buffer data
- `writeUtf8String(req, string)` — extract UTF-8 from JS string, `uv_write`
- `writeLatin1String(req, string)` — extract Latin-1, `uv_write`
- `writeAsciiString(req, string)` — extract ASCII, `uv_write`
- `writev(req, chunks, allBuffers)` — scatter-gather `uv_write`
- `shutdown(req)` — `uv_shutdown`
- `useUserBuffer(enable)` — toggle allocation mode
- `getWriteQueueSize()` — `uv_stream_get_write_queue_size`

**Alloc/Read callback flow:**
1. `readStart()` calls `uv_read_start(stream, allocCb, readCb)`
2. `allocCb`: allocate a Buffer (or use user-provided buffer)
3. `readCb`: on data, set `streamBaseState[kReadBytesOrError]` and call `handle[owner_symbol].onread(nread, buf)`
4. On EOF (`nread == UV_EOF`): call `onread` with nread = UV_EOF
5. On error: call `onread` with negative nread

**Write completion flow:**
1. Write call returns sync result (0 = queued, negative = error)
2. If async, `uv_write_cb` fires; set `streamBaseState[kBytesWritten]`, call `req.oncomplete(status)`
3. `streamBaseState[kLastWriteWasAsync]` indicates sync vs async completion

**Updated `stream_wrap` binding exports:**
- Enhance existing stub `initStreamWrapBinding` with real WriteWrap/ShutdownWrap that interact
  with the stream infrastructure
- WriteWrap: associated with a `uv_write_t`, fires `oncomplete` callback
- ShutdownWrap: associated with a `uv_shutdown_t`, fires `oncomplete` callback

**Files:**
- `include/hermes/node-compat/bindings/handle_wrap_base.h` — HandleWrapBase class
- `include/hermes/node-compat/bindings/libuv_stream_base.h` — LibuvStreamBase class
- `lib/bindings/handle_wrap_base.cpp`
- `lib/bindings/libuv_stream_base.cpp`
- Modified: `lib/bindings/node_stream_wrap.cpp` (enhanced from stub)
- `lib/bindings/CMakeLists.txt`

**Test:** GTest unit tests:
1. HandleWrapBase: create a `uv_timer_t` (simplest handle), wrap it, test ref/unref/hasRef/close.
2. LibuvStreamBase: create a `uv_pipe_t` pair, wrap one end, test writeBuffer → read on other end.
3. JS test: create a pipe pair via native helper, write data, verify read callback fires.

### Step N5.7: Vendor c-ares [vendor] [cmake] [depends: —]

Vendor c-ares from Node.js v24.13.0 `deps/cares/` under `external/cares/`:

```
external/cares/
├── README.md           # provenance: upstream URL, version, git hash
├── CMakeLists.txt      # wrapper that builds c-ares as static library
└── cares/              # unmodified vendored c-ares source
```

c-ares has its own CMake build. The wrapper CMake should:
- Set `CARES_STATIC ON`, `CARES_SHARED OFF`, `CARES_BUILD_TOOLS OFF`, `CARES_BUILD_TESTS OFF`
- Export target: `c-ares::cares_static` or wrapper `cares_a`

**Test:** GTest unit test that calls `ares_library_init()` and `ares_version()`, asserts version > 0.

### Step N5.8: Implement `dns.lookup()` via libuv [native] [depends: N5.7]

**Source:** `src/cares_wrap.cc` (GetAddrInfoReqWrap)
**Binding name:** `internalBinding('cares_wrap')`

`dns.lookup()` is the most commonly used DNS function — it's what `net.connect()` calls.
It uses `uv_getaddrinfo` (which delegates to the OS resolver), NOT c-ares.

Implement the `cares_wrap` binding with just the `getaddrinfo` path initially:

| Export | Implementation |
|---|---|
| `GetAddrInfoReqWrap` constructor | JS object for async request |
| `getaddrinfo(req, hostname, family, hints, verbatim)` | `uv_getaddrinfo` async |
| `convertIpv6StringToBuffer(ip)` | parse IPv6 string to 16-byte buffer |
| `AF_INET`, `AF_INET6`, `AF_UNSPEC` | constants |
| `AI_ADDRCONFIG`, `AI_ALL`, `AI_V4MAPPED` | hint constants |
| `isIP(input)` | classify 0 (invalid), 4 (IPv4), 6 (IPv6) |

The async pattern: `getaddrinfo(reqWrap, hostname, family, hints, verbatim)` queues work via
`uv_getaddrinfo`. On completion, calls `reqWrap.oncomplete(err, addresses)`.

**Test:**
```js
const dns = require('dns');
dns.lookup('localhost', (err, address, family) => {
  assert(!err, 'dns.lookup failed: ' + err);
  assert(typeof address === 'string');
  assert(family === 4 || family === 6);
  console.log('PASS');
});
```

### Step N5.9: Implement c-ares DNS queries [native] [depends: N5.7, N5.8]

**Source:** `src/cares_wrap.cc` (ChannelWrap, query functions)

Extend the `cares_wrap` binding with c-ares-based resolution for `dns.resolve*()`:

| Export | Implementation |
|---|---|
| `ChannelWrap` constructor | wraps `ares_channel`, manages socket polling via `uv_poll_t` |
| `QueryA/AAAA/MX/TXT/SRV/NS/SOA/CAA/CNAME/PTR/NAPTR/TLSA` | `ares_query` with type-specific parsing |
| `GetHostByAddr` | `ares_gethostbyaddr` (reverse DNS) |
| `GetNameInfoReqWrap` constructor | for `getnameinfo` |
| `getnameinfo(req, addr, port, flags)` | `uv_getnameinfo` async |
| `GetServers()` | get c-ares nameserver list |
| `SetServers(servers)` | set c-ares nameservers |
| `Cancel(req)` | cancel pending query |

**c-ares event loop integration:**
- c-ares provides file descriptors it wants polled via `ares_socket_callback`
- For each fd, create a `uv_poll_t` handle watching for read/write readiness
- When fd becomes ready, call `ares_process_fd(channel, read_fd, write_fd)`
- This pattern is identical to Node's `NodeAresTask` in `cares_wrap.cc`

**Test:**
```js
const dns = require('dns');
// dns.resolve (uses c-ares)
dns.resolve4('localhost', (err, addresses) => {
  // May fail in sandboxed environments, so just verify the API works
  assert(typeof err === 'object' || Array.isArray(addresses));
});
dns.reverse('127.0.0.1', (err, hostnames) => {
  assert(typeof err === 'object' || Array.isArray(hostnames));
});
// dns.promises
const dnsPromises = dns.promises;
assert(typeof dnsPromises.lookup === 'function');
assert(typeof dnsPromises.resolve4 === 'function');
console.log('PASS');
```

---

## C. TCP/IPC Networking

### Step N5.10: Port `tcp_wrap` binding [native] [depends: N5.6]

**Source:** `src/tcp_wrap.cc`
**Binding name:** `internalBinding('tcp_wrap')`

TCP is the core networking primitive. `TCPWrap` inherits from LibuvStreamBase.

Exports:
- `TCP` constructor — creates `uv_tcp_t`, takes `(type)` arg (SOCKET or SERVER)
- `TCPConnectWrap` constructor — request object for async connect
- `constants`: `SOCKET`, `SERVER`, `UV_TCP_IPV6ONLY`, `UV_TCP_REUSEPORT`

Prototype methods (on TCP instances):
- `open(fd)` — `uv_tcp_open`
- `bind(addr, port, flags)` — `uv_tcp_bind` (IPv4)
- `bind6(addr, port, flags)` — `uv_tcp_bind` (IPv6)
- `listen(backlog)` — `uv_listen`, fires `onconnection(err, clientHandle)` callback
- `connect(req, addr, port)` — `uv_tcp_connect` (IPv4), fires `req.oncomplete(status, ...)`
- `connect6(req, addr, port)` — `uv_tcp_connect` (IPv6)
- `getsockname(out)` — `uv_tcp_getsockname`, writes to object
- `getpeername(out)` — `uv_tcp_getpeername`, writes to object
- `setNoDelay(enable)` — `uv_tcp_nodelay`
- `setKeepAlive(enable, delay)` — `uv_tcp_keepalive`
- `reset(callback)` — reset the connection

Plus all inherited StreamBase methods (readStart/readStop/write*/shutdown) and HandleWrap methods
(ref/unref/close).

**Connection acceptance flow:**
1. Server calls `listen(backlog)`
2. On incoming connection, libuv fires `onconnection` callback
3. Native side creates a new `uv_tcp_t` for the client, calls `uv_accept`
4. Wraps client in a new TCP JS object, calls `server.onconnection(err, clientHandle)`

**Test:**
```js
const net = require('net');
const server = net.createServer((socket) => {
  socket.write('hello');
  socket.end();
});
server.listen(0, () => {
  const port = server.address().port;
  const client = net.connect(port, () => {
    let data = '';
    client.on('data', (chunk) => { data += chunk; });
    client.on('end', () => {
      assert(data === 'hello');
      server.close();
      console.log('PASS');
    });
  });
});
```

### Step N5.11: Port `pipe_wrap` binding [native] [depends: N5.6]

**Source:** `src/pipe_wrap.cc`
**Binding name:** `internalBinding('pipe_wrap')`

Unix domain sockets / named pipes. `PipeWrap` inherits from LibuvStreamBase.

Exports:
- `Pipe` constructor — creates `uv_pipe_t`, takes `(type, ipc)` args
- `PipeConnectWrap` constructor — request object for async connect
- `constants`: `SOCKET`, `SERVER`, `IPC`, `UV_READABLE`, `UV_WRITABLE`

Prototype methods:
- `open(fd)` — `uv_pipe_open`
- `bind(name)` — `uv_pipe_bind`
- `listen(backlog)` — `uv_listen`
- `connect(req, name)` — `uv_pipe_connect`
- `fchmod(mode)` — `uv_pipe_chmod`

Plus inherited StreamBase + HandleWrap methods.

**Test:**
```js
const net = require('net');
const os = require('os');
const path = require('path');
const fs = require('fs');

const sockPath = path.join(os.tmpdir(), 'hermes-test-' + process.pid + '.sock');
try { fs.unlinkSync(sockPath); } catch(e) {}

const server = net.createServer((socket) => {
  socket.write('pipe-hello');
  socket.end();
});
server.listen(sockPath, () => {
  const client = net.connect(sockPath, () => {
    let data = '';
    client.on('data', (chunk) => { data += chunk; });
    client.on('end', () => {
      assert(data === 'pipe-hello');
      server.close(() => {
        fs.unlinkSync(sockPath);
        console.log('PASS');
      });
    });
  });
});
```

### Step N5.12: Verify `net` module works [verify] [depends: N5.8, N5.10, N5.11]

Comprehensive verification of the `net` module:

```js
const net = require('net');

// Test 1: TCP echo server
// Test 2: Multiple concurrent connections
// Test 3: Server address()
// Test 4: Socket properties (localAddress, localPort, remoteAddress, remotePort)
// Test 5: setNoDelay, setKeepAlive
// Test 6: socket.setTimeout
// Test 7: net.isIP, net.isIPv4, net.isIPv6
// Test 8: Error handling (ECONNREFUSED on connect to unused port)
// Test 9: Pipe (Unix domain socket) server + client
// Test 10: dns.lookup integration (connect by hostname 'localhost')
```

**Test:** Run comprehensive net module test. Additionally, port at least 3 Node.js `test/parallel/test-net-*.js` tests.

---

## D. UDP

### Step N5.13: Port `udp_wrap` binding [native] [depends: N5.6]

**Source:** `src/udp_wrap.cc`
**Binding name:** `internalBinding('udp_wrap')`

`UDPWrap` inherits from HandleWrapBase (NOT LibuvStreamBase — UDP is not a stream).

Exports:
- `UDP` constructor — creates `uv_udp_t`
- `SendWrap` constructor — request object for async send

Prototype methods:
- `open(fd)` — `uv_udp_open`
- `bind(addr, port, flags)` — `uv_udp_bind`
- `connect(addr, port)` — `uv_udp_connect` (set default dest)
- `disconnect()` — `uv_udp_connect(NULL, 0)`
- `send(req, bufs, addr, port, hasCallback)` — `uv_udp_send`
- `recvStart()` — `uv_udp_recv_start`
- `recvStop()` — `uv_udp_recv_stop`
- `getsockname(out)` — `uv_udp_getsockname`
- `getpeername(out)` — `uv_udp_getpeername`
- `addMembership(group, iface)` — `uv_udp_set_membership(UV_JOIN_GROUP)`
- `dropMembership(group, iface)` — `uv_udp_set_membership(UV_LEAVE_GROUP)`
- `addSourceSpecificMembership(...)` — `uv_udp_set_source_membership`
- `dropSourceSpecificMembership(...)` — `uv_udp_set_source_membership`
- `setMulticastInterface(addr)` — `uv_udp_set_multicast_interface`
- `setMulticastLoopback(enable)` — `uv_udp_set_multicast_loop`
- `setMulticastTTL(ttl)` — `uv_udp_set_multicast_ttl`
- `setBroadcast(enable)` — `uv_udp_set_broadcast`
- `setTTL(ttl)` — `uv_udp_set_ttl`
- `bufferSize` — `uv_send_buffer_size` / `uv_recv_buffer_size`

**Recv callback flow:**
1. `recvStart()` calls `uv_udp_recv_start(handle, allocCb, recvCb)`
2. `allocCb`: allocate a Buffer
3. `recvCb`: on data, call `handle.onmessage(nread, handle, buf, rinfo)` where rinfo has
   address/port/family

**Also needed:** Add `UV_UDP_REUSEADDR` and other UDP constants to the existing `constants`
binding (`constants.os`). Check what's already there.

**Test:**
```js
const dgram = require('dgram');
const server = dgram.createSocket('udp4');
server.on('message', (msg, rinfo) => {
  assert(msg.toString() === 'hello-udp');
  assert(typeof rinfo.address === 'string');
  assert(typeof rinfo.port === 'number');
  server.close();
  console.log('PASS');
});
server.bind(0, () => {
  const port = server.address().port;
  const client = dgram.createSocket('udp4');
  client.send('hello-udp', port, '127.0.0.1', () => {
    client.close();
  });
});
```

---

## E. HTTP

### Step N5.14: Vendor llhttp [vendor] [cmake] [depends: —]

Vendor llhttp from Node.js v24.13.0 `deps/llhttp/` under `external/llhttp/`:

```
external/llhttp/
├── README.md           # provenance
├── CMakeLists.txt      # wrapper — builds as static library
└── llhttp/             # unmodified vendored source (include/ + src/)
```

llhttp is small (3 C files, 1 header) and has no dependencies.
Wrapper CMake builds a static `llhttp_a` target.

**Test:** GTest unit test that:
1. Creates an `llhttp_t` parser
2. Parses a simple HTTP request: `"GET / HTTP/1.1\r\nHost: localhost\r\n\r\n"`
3. Asserts method = HTTP_GET, URL = "/"

### Step N5.15: Port `http_parser` binding [native] [depends: N5.6, N5.14]

**Source:** `src/node_http_parser.cc`
**Binding name:** `internalBinding('http_parser')`

Wraps llhttp to provide HTTP/1.x request/response parsing.

Exports:
- `HTTPParser` constructor — creates parser instance
- `methods` array — HTTP method name strings (GET, POST, PUT, etc.)
- `allMethods` array — all method strings including CONNECT, OPTIONS, etc.
- `ConnectionsList` constructor — tracks active HTTP connections for a server
- Constants: `REQUEST`, `RESPONSE`, `kOnMessageBegin`, `kOnHeaders`, `kOnHeadersComplete`,
  `kOnBody`, `kOnMessageComplete`, `kOnExecute`, `kOnTimeout`, `kLenientNone`, `kLenientAll`,
  plus individual lenient flags

HTTPParser prototype methods:
- `initialize(type, maxHeaderSize)` — init parser as REQUEST or RESPONSE
- `reinitialize(type)` — reset for keep-alive
- `execute(data)` — parse chunk, fire JS callbacks
- `finish()` — signal end of stream
- `close()` — cleanup
- `pause()` / `resume()` — flow control
- `getCurrentBuffer()` — get current parsing buffer
- `consume(stream)` — consume from a native stream handle directly
- `unconsume()` — stop consuming

**Callback mechanism:**
The parser fires callbacks by index on the JS parser object:
- `parser[kOnMessageBegin]()` — new message starts
- `parser[kOnHeaders](headers, url)` — headers accumulate
- `parser[kOnHeadersComplete](versionMajor, versionMinor, headers, method, url, statusCode,
  statusMessage, upgrade, shouldKeepAlive)` — headers fully received
- `parser[kOnBody](chunk, offset, length)` — body data
- `parser[kOnMessageComplete]()` — message ends

Node's JS HTTP layer (`_http_common.js`, `_http_server.js`, `_http_client.js`) sets these
callbacks and builds `IncomingMessage` / `ServerResponse` objects from the parsed data.

**Test:**
```js
const http = require('http');

const server = http.createServer((req, res) => {
  assert(req.method === 'GET');
  assert(req.url === '/test');
  res.writeHead(200, { 'Content-Type': 'text/plain' });
  res.end('Hello from hermes-node');
});

server.listen(0, () => {
  const port = server.address().port;
  http.get(`http://127.0.0.1:${port}/test`, (res) => {
    let data = '';
    res.on('data', (chunk) => { data += chunk; });
    res.on('end', () => {
      assert(res.statusCode === 200);
      assert(data === 'Hello from hermes-node');
      server.close();
      console.log('PASS');
    });
  });
});
```

### Step N5.16: Verify HTTP works [verify] [depends: N5.12, N5.15]

Comprehensive HTTP verification:

```js
// Test 1: Simple GET request + response
// Test 2: POST with request body
// Test 3: Chunked transfer encoding
// Test 4: HTTP keep-alive (multiple requests on same connection)
// Test 5: Request headers received correctly
// Test 6: Response headers set correctly
// Test 7: HTTP status codes
// Test 8: Large response body (streaming)
// Test 9: Client timeout
// Test 10: Server close while connections active
// Test 11: http.request with hostname resolution (dns.lookup)
```

**Test:** Port at least 3 Node.js `test/parallel/test-http-*.js` tests.

---

## F. Child Processes

### Step N5.17: Port `process_wrap` binding [native] [depends: N5.6, N5.11]

**Source:** `src/process_wrap.cc`
**Binding name:** `internalBinding('process_wrap')`

Wraps `uv_process_t` for spawning child processes.

Exports:
- `Process` constructor — wraps `uv_process_t`

Prototype methods:
- `spawn(options)` — `uv_spawn` with stdio config
- `kill(signal)` — `uv_process_kill`

The `options` argument to `spawn` is a JS object with:
- `file` — executable path
- `args` — argument array
- `cwd` — working directory
- `envPairs` — environment as `["KEY=VALUE", ...]` array
- `stdio` — array of stdio descriptors: `{type: 'pipe'|'inherit'|'ignore'|'fd', fd: N}`
- `uid`, `gid` — optional
- `detached` — boolean
- `windowsVerbatimArguments`, `windowsHide` — Windows-only, ignore

**Stdio pipe setup:**
For each stdio descriptor with `type: 'pipe'`, create a `uv_pipe_t` and pass it as
`uv_stdio_container_t` with `UV_CREATE_PIPE`. After spawn, wrap the pipe and return it as a
JS PipeWrap to the caller.

**Exit callback:**
When the child exits, `uv_exit_cb` fires. Call `process.onexit(exitCode, signalCode)` on
the JS wrapper.

**Test:**
```js
const { execSync, exec, spawn } = require('child_process');

// execSync
const result = execSync('echo hello').toString().trim();
assert(result === 'hello');

// spawn
const child = spawn('echo', ['world']);
let output = '';
child.stdout.on('data', (data) => { output += data; });
child.on('close', (code) => {
  assert(code === 0);
  assert(output.trim() === 'world');
  console.log('PASS');
});
```

### Step N5.18: Port `spawn_sync` binding [native] [depends: N5.17]

**Source:** `src/spawn_sync.cc`
**Binding name:** `internalBinding('spawn_sync')`

Synchronous child process execution, used by `child_process.execSync`, `spawnSync`,
`execFileSync`.

Exports:
- `spawn(options)` — synchronous spawn that blocks until child exits

The sync spawn is more complex than it appears: it creates a temporary `uv_loop_t`, spawns the
child on that loop, runs the loop until the child exits, collects stdout/stderr/status, and
returns everything as a single result object.

Result object: `{ pid, output: [null, stdout, stderr], stdout, stderr, status, signal, error }`

**Test:**
```js
const { spawnSync, execFileSync } = require('child_process');
const result = spawnSync('echo', ['sync-hello']);
assert(result.status === 0);
assert(result.stdout.toString().trim() === 'sync-hello');
// execFileSync
const out = execFileSync('echo', ['execfile-test']).toString().trim();
assert(out === 'execfile-test');
console.log('PASS');
```

### Step N5.19: Verify `child_process` module works [verify] [depends: N5.17, N5.18]

```js
// Test 1: spawn with stdio pipe (capture stdout)
// Test 2: exec callback API
// Test 3: execSync
// Test 4: spawnSync
// Test 5: execFile + execFileSync
// Test 6: Child environment variables
// Test 7: Child working directory
// Test 8: Signal handling (kill child)
// Test 9: Exit code propagation
// Test 10: stderr capture
```

**Test:** Port at least 3 Node.js `test/parallel/test-child-process-*.js` tests.

---

## G. Process.stdin and TTY Upgrade

### Step N5.20: Implement `process.stdin` [native] [js] [depends: N5.3, N5.6]

**Source:** Bootstrap in `hermes-node.cpp`

Currently `process.stdout`/`process.stderr` are minimal objects with synchronous write.
Once the stream infrastructure and tty_wrap are available:

1. Create `process.stdin` as a proper readable stream backed by a TTY or Pipe handle
2. Optionally upgrade `process.stdout`/`process.stderr` to proper Writable streams backed
   by TTY or Pipe handles (using `uv_guess_handle` to pick the right type)

For `process.stdin`:
- Use `uv_guess_handle(0)` to determine type (TTY, Pipe, File)
- If TTY: create `TTYWrap(0, true)` for reading
- If Pipe: create `PipeWrap(SOCKET)` + `open(0)`
- Wrap in a `net.Socket` or `tty.ReadStream`

**Test:**
```js
// Can't easily test interactive stdin in automated tests, but verify properties:
assert(typeof process.stdin === 'object');
assert(typeof process.stdin.read === 'function' || typeof process.stdin.on === 'function');
assert(typeof process.stdin.isTTY === 'boolean' || process.stdin.isTTY === undefined);

// Test stdout/stderr are proper streams
assert(typeof process.stdout.write === 'function');
assert(typeof process.stderr.write === 'function');
process.stdout.write('stdout-ok\n');
process.stderr.write('stderr-ok\n');
console.log('PASS');
```

---

## H. Supplementary and Verification

### Step N5.21: Add missing `os` constants [native] [depends: N5.1]

Ensure the existing `constants` binding (`internalBinding('constants').os`) includes all
constants needed by networking modules:
- `UV_UDP_REUSEADDR` (needed by `dgram.js`)
- `UV_UDP_IPV6ONLY`, `UV_UDP_PARTIAL`, `UV_UDP_REUSEPORT`
- Socket constants: `SOCK_STREAM`, `SOCK_DGRAM`, `SOCK_RAW` (if not already present)
- Additional signal constants if missing

**Test:** JS test asserting all required constants exist and have numeric values.

### Step N5.22: Run Node.js net test subset [verify] [depends: N5.12]

Set up and run a subset of Node's `test/parallel/test-net-*.js` tests:
- `test-net-server-listen-port.js`
- `test-net-connect-options-port.js`
- `test-net-socket-timeout.js`
- `test-net-server-close.js`
- `test-net-pipe-connect-errors.js`

Fix failures. Target: at least 5 of 8 pass.

### Step N5.23: Run Node.js http test subset [verify] [depends: N5.16]

Run a subset of Node's `test/parallel/test-http-*.js` tests:
- `test-http-server-response-end.js`
- `test-http-response-statuscode.js`
- `test-http-set-timeout-server.js`
- `test-http-keepalive-close.js`
- `test-http-client-get-url.js`

Fix failures. Target: at least 4 of 8 pass.

### Step N5.24: Run Node.js child_process test subset [verify] [depends: N5.19]

Run a subset of Node's `test/parallel/test-child-process-*.js` tests:
- `test-child-process-exec-timeout.js`
- `test-child-process-spawn-args.js`
- `test-child-process-spawnsync.js`
- `test-child-process-exit-code.js`

Fix failures. Target: at least 3 of 5 pass.

---

## Dependency Graph

```
Independent (no dependencies within Phase 5):
  N5.1  (os binding)
  N5.2  (credentials binding)
  N5.4  (simdutf integration)
  N5.5  (Ada/url integration)
  N5.6  (HandleWrap + LibuvStreamBase)
  N5.7  (vendor c-ares)
  N5.14 (vendor llhttp)

First-tier dependencies:
  N5.3  (tty_wrap)         ← N5.6
  N5.8  (dns.lookup)       ← N5.7
  N5.10 (tcp_wrap)         ← N5.6
  N5.11 (pipe_wrap)        ← N5.6
  N5.13 (udp_wrap)         ← N5.6
  N5.15 (http_parser)      ← N5.6, N5.14

Second-tier dependencies:
  N5.9  (c-ares queries)   ← N5.7, N5.8
  N5.12 (verify net)       ← N5.8, N5.10, N5.11
  N5.17 (process_wrap)     ← N5.6, N5.11
  N5.20 (process.stdin)    ← N5.3, N5.6
  N5.21 (os constants)     ← N5.1

Third-tier dependencies:
  N5.16 (verify HTTP)      ← N5.12, N5.15
  N5.18 (spawn_sync)       ← N5.17
  N5.22 (net tests)        ← N5.12
  N5.23 (http tests)       ← N5.16
  N5.19 (verify child_proc)← N5.17, N5.18
  N5.24 (child_proc tests) ← N5.19
```

## Critical Path

The longest dependency chain is:

```
N5.6 (HandleWrap+StreamBase) → N5.10 (tcp_wrap) → N5.12 (verify net) → N5.16 (verify HTTP) → N5.23 (http tests)
```

The HandleWrap/LibuvStreamBase infrastructure (N5.6) is the gating step — all native stream
types depend on it.

## Parallelization Opportunities

These groups can be worked on in parallel:

**Group A (independent vendoring + integration):**
- N5.4 (simdutf)
- N5.5 (Ada/url)
- N5.7 (vendor c-ares)
- N5.14 (vendor llhttp)

**Group B (independent bindings):**
- N5.1 (os)
- N5.2 (credentials)
- N5.6 (HandleWrap + StreamBase) — critical path

**Group C (after N5.6 — all parallelizable):**
- N5.3 (tty_wrap)
- N5.10 (tcp_wrap)
- N5.11 (pipe_wrap)
- N5.13 (udp_wrap)
- N5.15 (http_parser)

## Modules NOT Ported (deferred beyond Phase 5)

- `tls` / `https` — requires OpenSSL integration (large, security-sensitive)
- `http2` — requires nghttp2 vendoring
- `crypto` — requires OpenSSL (can be a dedicated phase)
- `zlib` — requires zlib/brotli vendoring
- `worker_threads` — requires Hermes threading design
- `cluster` — requires `child_process` + IPC + load balancing
