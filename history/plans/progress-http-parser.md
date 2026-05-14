# Implementation Progress

Tracks progress on `history/plans/http-parser-connections-list.md`.

The file has two sections: "Status" and "Context Notes".

**Status Section**:

Each row contains the step label from the detailed plan, a brief description, list of
dependency labels, Status (initially empty), optional brief note (initially empty).

The status of a row is one of:
- "" (empty) initially, before work has started
- "wip" as soon as work on that row has started.
- "done" when work has completed successfully. Rarely "Brief Note" may contain very brief
explanation. More details in "Context Notes".
- "blocked" when work cannot proceed for some reason. "Brief Note" must contain a brief
explanation. More details in "Context Notes".

**Context Notes**:

After completing work on a step, either successfully or by blocking, a section for that
step is added.

## Status

| Step | Description | Depends On | Status | Brief Note (optional) |
|------|-------------|------------|--------|-----------------------|
| Step 1 | Replace vector<napi_ref> with dual-set + per-parser ref | — | done | Bundled with Steps 2-4 -- single rewrite of `ConnectionsList`. |
| Step 2 | Track active subset (Push/PopActive at lifecycle hooks) | 1 | done | |
| Step 3 | Track lastMessageStart_ across lifecycle hooks | 1, 2 | done | |
| Step 4 | Implement expired() per Node spec | 3 | done | |
| Step 5 | Documentation + cleanup | 1-4 | done | |

## Context Notes

### Steps 1-4 (bundled): ConnectionsList rewrite

The four implementation steps are tightly coupled (the dual-set storage,
the Active subset, the timestamp field, and the Expired() walker all
share the `ParserComparator` / std::set machinery), so they were landed
together in a single edit to `lib/bindings/node_http_parser.cpp`.

- **Files**:
  - modified `lib/bindings/node_http_parser.cpp`
  - created `test/test-http-close-idle.js`
  - created `test/test-http-headers-timeout.js`
  - created `test/test-http-request-timeout.js`

- **Decisions**:
  - **Storage**: matched Node's design exactly --
    `std::set<Parser*, ParserComparator>` for both `all_` and `active_`.
    Did NOT add the per-list `unordered_map<Parser*, napi_ref>` the plan
    suggested -- each `Parser` already holds a strong `selfRef_` keeping
    its JS object alive, so `Parser::getJsObject()` is enough to recover
    the JS object when building result arrays. Avoids ref bookkeeping on
    every Pop/Push (which happens on every `on_message_begin` /
    `on_message_complete`).
  - **Comparator**: copied Node's special case for idle parsers
    (`lastMessageStart_ == 0` sorts before any active parser). Required
    so `Idle()` works by walking `all_` and stopping when it would hit
    active entries -- though we keep the simpler "filter by timestamp"
    implementation that matches Node's `Idle()`.
  - **Class ordering**: `ConnectionsList` had to move BEFORE `Parser` in
    the file (with method bodies still defined out-of-line after Parser).
    Inline lifecycle methods on `Parser` (`sOnMessageBegin`,
    `sOnMessageComplete`, `~Parser`) call `connectionsList_->pop(...)`,
    so `ConnectionsList` must be a complete type when those bodies are
    parsed. `ParserComparator` is forward-declared with operator() body
    out-of-line after `Parser` is complete (its body needs Parser
    members).
  - **Friendship**: `ConnectionsList` and `ParserComparator` are friends
    of `Parser` so they can read `lastMessageStart_`, `headersCompleted_`,
    and call `getJsObject()` without exposing them publicly.
  - **Expired() semantics**: matched Node's actual code (iterate `active_`,
    erase expired entries in place). Node's reference also includes a
    `headersTimeout`/`requestTimeout` swap when the latter is smaller --
    we preserved that. The plan's text described iterating `all_` with
    early-out, which differs from Node's actual implementation; chose
    Node's implementation since the plan's title is "Match Node 1:1".
  - **Defensive Pop in `~Parser`**: added because the destructor runs
    from the GC finalizer (`pointerCb`), and although `freeParser()` in
    JS calls `parser.remove()` first, it's cheap insurance against any
    code path that destroys a Parser still referenced by a list.

- **What was done**:
  - `Parser` gained `uint64_t lastMessageStart_ = 0;` (already had
    `headersCompleted_`).
  - `ConnectionsList::push`/`pop`/`pushActive`/`popActive` now take
    `Parser*` and operate on the std::sets.
  - Lifecycle hooks (`sOnMessageBegin`, `sOnMessageComplete`,
    `ParserInitialize`, `ParserRemove`, `~Parser`) Pop-then-mutate-then-
    Push around timestamp changes, mirroring the Node source comments
    "Important: Pop from the lists BEFORE resetting last_message_start_".
  - `Expired()` now does the headers-vs-request branch and the
    underflow-safe deadline math from Node's `Expired()`.
  - `ConnectionsListExpired` switched from `napi_get_value_double` to
    `napi_get_value_uint32` to match Node's `Uint32::Value()` and the
    type passed in from the JS side.
  - JS-callable surface (`HTTPParser`, `ConnectionsList`, `methods`,
    `allMethods`, all the constants) is unchanged.

- **Tests**:
  - `test/test-http.js` test 12 (closeAllConnections regression): still
    passes.
  - `test/test-http-close-idle.js`: keep-alive idle socket destroyed by
    `closeIdleConnections()`; mid-request socket left untouched.
  - `test/test-http-headers-timeout.js`: partial request line is closed
    by the periodic `checkConnections()` once `headersTimeout` elapses;
    server writes 408.
  - `test/test-http-request-timeout.js`: complete headers + partial body
    is closed when `requestTimeout` elapses; server writes 408.
  - All other lit tests (besides 4 pre-existing failures unrelated to
    HTTP: `test-fs-async-verify`, `test-inspect`, `test-native-addon`,
    `test-native-addon-pkg`) still pass.

- **Issues**: None. Initial test draft for close-idle hit
  `res.socket === null` in the `'end'` listener; switched to capturing
  the socket via `req.on('socket', ...)`. Initial test process hung
  because partial body byte counts didn't add up to `Content-Length`;
  fixed by sending exactly the declared body size.

### Step 5: Documentation + cleanup

- **Files**: modified `doc/DESIGN.md`,
  modified `lib/bindings/node_http_parser.cpp` (replaced the
  "simplified" disclaimer comment block on `ConnectionsList` with a
  description of the actual two-set semantics).

- **What was done**: Added a paragraph to the HTTP Parser section of
  `doc/DESIGN.md` describing the `ConnectionsList` two-set invariant and
  the Pop-then-mutate-then-Push discipline. The binding header comment
  on `ConnectionsList` now describes the lifecycle invariants instead of
  apologizing for a missing implementation.

- **Notes for future work**: The plan ends with a "Process change to
  prevent regressions" section recommending per-method coverage when
  porting bindings. That's a workflow change, not a code change, so it
  isn't tracked here.
