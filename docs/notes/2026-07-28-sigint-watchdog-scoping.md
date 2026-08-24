# SIGINT Watchdog Scoping Design

Date: 2026-07-28. Status: implemented (this doc records the design and its
rationale; no separate progress file -- the work landed in one pass).

## Problem

`breakOnSigint` means "SIGINT interrupts *this vm script evaluation*". The
old implementation had a scope mismatch: the signal handler unconditionally
triggered a Hermes async break, which detonates at the next `AsyncBreakCheck`
in *any* JS as an uncatchable `TimeoutError`. Consequences:

- A SIGINT in the arm-to-run window (repl.js arms the watchdog, then runs
  more JS before the script starts) killed the process instead of being
  caught. `test-sigint-watchdog.js` Test 3 hit exactly this.
- A SIGINT racing script completion left the break pending in the runtime,
  where it would terminate the next, unrelated JS execution (delayed
  process kill).
- Any script failure while a SIGINT flag was latched was mis-converted to
  `ERR_SCRIPT_EXECUTION_INTERRUPTED`, swallowing the script's real
  exception.

Node inherits a similar arm-to-run window because V8 gives it no better
hook; we own both sides of the boundary, so we do not have to.

## Model: pending interrupts

Classic pending-signal / cancellation-token semantics:

1. **The handler only records.** It latches "SIGINT pending" and triggers
   the async break only while a vm script is executing (atomic depth
   counter `s_vmScriptDepth`; a counter, not a bool, for nested
   evaluations). Async-signal-safe: atomics only.
2. **The region checks on entry.** `contextifyScriptRunInContext`
   increments depth, then tests the latch; if set, it throws catchable
   `ERR_SCRIPT_EXECUTION_INTERRUPTED` *without running the script* (a
   cancellation request that precedes the work cancels the work). The
   handler's record-then-test mirrored against the binding's
   increment-then-check closes every interleaving.
3. **The region cancels on exit.** Any async break the script did not
   consume is cancelled so it cannot leak into later JS. The cancel's
   return value distinguishes "the failure was the interrupt firing"
   (request consumed -> convert to catchable error) from "the script threw
   its own exception" (request still pending -> propagate unchanged).
4. **Deliver-once.** Delivering the interrupt (entry throw or conversion)
   clears the latch; `stopSigintWatchdog()` reports only undelivered
   signals, matching repl.js's contract ("pending SIGINTs after the script
   terminated without being interrupted"), and also cancels any unconsumed
   break.

A SIGINT outside any script is therefore: latched, reported by `stop`,
surfaced by the REPL as `'SIGINT'` -- never a process kill.

## Hermes API: ICancelAsyncTimeout

Step 3 needs a cancel primitive Hermes lacked: `asyncTriggerTimeout()` had
no counterpart (V8: `TerminateExecution` / `CancelTerminateExecution`).

Adding a virtual to `IHermes` would change its vtable layout and break the
JSI ABI -- the castInterface/UUID discipline exists to prevent exactly
that. So the cancel is a new, purely additive UUID'd side interface in
`jsi/hermes-interfaces.h` (the shared home of Hermes-backend interfaces;
the sandbox depends only on the jsi package, so it cannot live in
`hermes.h`):

- `ICancelAsyncTimeout::asyncCancelTimeout()` -> bool (was a request
  pending). JS-thread only: between executions or from a native frame
  invoked by executing JS; other threads would race the interpreter's
  consumption. Native impl asserts via `NoMutatorScope`.
- `HermesRuntimeImpl` implements it (+ `castInterface` case);
  `vm::Runtime::cancelTimeoutAsyncBreak()` is the public VM-level wrapper.
- `HermesSandboxRuntimeImpl` implements it via its own `castInterface`
  override; the abstract sandbox class's vtable is untouched.
- Callers feature-detect: `castInterface` returns null on runtimes without
  support. hermes-node then leaves `RuntimeState::cancelAsyncBreakFn` null
  and the contextify binding (which null-checks every use) degrades to the
  old behavior minus the leak protection.

Hermes commit: "Add ICancelAsyncTimeout to cancel a pending timeout async
break" on the `n-api` branch.

## Known limitations

- The "break triggered mid-script but unconsumed" interleaving is not
  deterministically constructible from single-threaded JS; the exit-path
  cancel is exercised probabilistically by the REPL child-process test.
- The exit-path cancel is gated on the watchdog being active so it only
  discards breaks the watchdog itself caused; hermes-node currently has no
  other producer of timeout breaks (no `watchTimeLimit` use, vm `timeout`
  option not implemented).
