# Implementation Progress

Tracks progress on `history/initial/2026-02-16-phase5-networking-plan.md`.

The file has two sections: "Status" and "Context Notes".

**Status Section**:

Each row contains the step label from the detailed, plan, a brief description, list of
dependency labels, Status (initially empty), optional brief note (initially empty).

The status of a row is one of:
- "" (empty) initially, before work has started
- "wip" as soon as work on that raw has started.
- "done" when work has completed successfully. Rarely "Brief Note" may contain very brief
explanation. More details in "Context Notes".
- "blocked" when work cannot proceed for some reason. "Brief Note" must contain a brief
explanation. More details in "Context Notes".

Example on start:

| Step 11 | Port util binding | 5 |  |  |

**Context Notes**:

After completing work on a step, either successfully or by blocking, a section for that
step is added. It needs to have the format from the following example (empty bullets can
be omitted):

```
### Step 11: Port util binding
- **Files**: created `foo.c`, modified `bar.c`.
- **Decisions**:
-- Decision 1 concise explanation
-- Decision 2 concise explanation
- **What was done**: ...
- **Issues**: ...
- **Notes for next step**: ...
```

## Status

| Step | Description | Depends On | Status | Brief Note (optional) |
|------|-------------|------------|--------|-----------------------|
| N5.1 | Port `os` binding | — | done | Also added credentials binding |
| N5.2 | Port `credentials` binding | — | done | Done as part of N5.1 |
| N5.3 | Port `tty_wrap` binding | N5.6 | | |
| N5.4 | Integrate simdutf into buffer/encoding | — | done | Already mostly integrated; replaced last hand-rolled UTF-8 trim |
| N5.5 | Integrate Ada into url binding | — | | |
| N5.6 | Implement HandleWrap + LibuvStreamBase | — | | |
| N5.7 | Vendor c-ares | — | | |
| N5.8 | Implement `dns.lookup()` via libuv | N5.7 | | |
| N5.9 | Implement c-ares DNS queries | N5.7, N5.8 | | |
| N5.10 | Port `tcp_wrap` binding | N5.6 | | |
| N5.11 | Port `pipe_wrap` binding | N5.6 | | |
| N5.12 | Verify `net` module works | N5.8, N5.10, N5.11 | | |
| N5.13 | Port `udp_wrap` binding | N5.6 | | |
| N5.14 | Vendor llhttp | — | | |
| N5.15 | Port `http_parser` binding | N5.6, N5.14 | | |
| N5.16 | Verify HTTP works | N5.12, N5.15 | | |
| N5.17 | Port `process_wrap` binding | N5.6, N5.11 | | |
| N5.18 | Port `spawn_sync` binding | N5.17 | | |
| N5.19 | Verify `child_process` module works | N5.17, N5.18 | | |
| N5.20 | Implement `process.stdin` | N5.3, N5.6 | | |
| N5.21 | Add missing `os` constants | N5.1 | | |
| N5.22 | Run Node.js net test subset | N5.12 | | |
| N5.23 | Run Node.js http test subset | N5.16 | | |
| N5.24 | Run Node.js child_process test subset | N5.19 | | |

## Context Notes

### Step N5.1: Port `os` binding
- **Files**: created `include/hermes/node-compat/bindings/node_os.h`, `lib/bindings/node_os.cpp`, `include/hermes/node-compat/bindings/node_credentials.h`, `lib/bindings/node_credentials.cpp`, `test/test-os.js`. Modified `lib/bindings/CMakeLists.txt`, `tools/hermes-node/hermes-node.cpp`.
- **Decisions**:
-- Implemented credentials binding (N5.2) alongside os binding because `libjs-node/os.js` imports `getTempDir` from `internalBinding('credentials')` at load time -- os module cannot load without it.
-- credentials binding includes POSIX credential functions (getuid/geteuid/getgid/getegid/getgroups) in addition to getTempDir/safeGetenv.
-- Error reporting uses the ctx-object pattern matching Node's `getCheckedFunction` in os.js: set errno/message/syscall/code on ctx object, return undefined on error.
- **What was done**: Full `os` binding with 13 functions (getHostname, getOSInformation, getLoadAvg, getUptime, getCPUs, getFreeMem, getTotalMem, getHomeDirectory, getUserInfo, getInterfaceAddresses, setPriority, getPriority, getAvailableParallelism) + isBigEndian property. Full `credentials` binding with getTempDir, safeGetenv, and POSIX credentials. JS test covering all os module APIs.
- **Notes for next step**: N5.2 is already done. Constants (priority, dlopen, signals, errno) were already in node_constants.cpp from earlier phases.

### Step N5.2: Port `credentials` binding
- Done as part of N5.1 (see above).

### Step N5.4: Integrate simdutf into buffer/encoding
- **Files**: modified `lib/bindings/node_buffer.cpp`, `test/test-buffer.js`, `test/test-encoding.js`.
- **What was done**: simdutf was already integrated for UTF-8/ASCII validation, base64 encode/decode, Latin-1↔UTF-8 conversions, and UTF-16 length calculations. Replaced the last hand-rolled code: UTF-8 boundary trimming in `utf8WriteStaticCb` now uses `simdutf::trim_partial_utf8()` instead of manual byte-walking. Added edge-case tests for simdutf code paths: truncated/invalid UTF-8 sequences, base64 edge cases (empty, padding-only, single byte roundtrip), UTF-8 write boundary truncation (2/3/4-byte chars at various maxLength limits), hex write edge cases, emoji encodeInto, Latin-1 full-range decode.
- **Decisions**:
-- Hex codec and substring search remain hand-rolled (simdutf has no support for these).
-- UCS2 slice/write use NAPI UTF-16 functions directly (no transcoding needed).
- **Issues**: Hermes `_decodeUTF8SlowPath` has an OOB read when `napi_create_string_utf8` is called with truncated multi-byte UTF-8 (e.g. buffer = `[0xc3]`). This is a VM-internal bug, not fixable in the NAPI layer. Worked around by testing only full-length invalid sequences (e.g. `[0xc3, 0x00]`) in fatal mode and skipping non-fatal truncated sequence tests.

