# Note: there is no runtime-configuration surface

**Status:** a note, not a plan. Nothing here is scheduled and no code is
proposed. Written 2026-08-24, prompted by the question "in binary mode, how
do we pass JSVM args?" -- to which the answer turned out to be more
interesting than the question assumed.

## The observation

You cannot pass VM arguments to a `--build-exe` executable. You also cannot
pass them to `hermes-node` itself. The Hermes runtime config is a literal in
`runHermesNode()` (`lib/runtime/hermes_node_runtime.cpp`):

```cpp
auto rtConfig = hermes::vm::RuntimeConfig::Builder()
                    .withMicrotaskQueue(true)
                    .withEnableAsyncGenerators(true)
                    .withES6BlockScoping(true)
                    .build();
```

Nothing reaches it. There are no `-X` flags, no heap-size or GC knobs, and no
environment variable that touches it, in any mode. So a produced executable
is not missing a capability the CLI has -- **neither has one**, and the
single-executable work did not narrow anything.

That is worth writing down precisely because it looks like a gap in
`--build-exe` and is not one.

## What a produced executable can still be told

Measured, not assumed -- each verified against a linked example executable:

| Variable | Effect |
|---|---|
| `HERMES_NODE_DEBUG_NATIVE=BUNDLE` | logs every bundled `require` -- edge-table hit, container-resolve hit, or miss |
| `HERMES_NODE_DISABLE_COMPILE_CACHE=1` | disables the compile cache, and with it the unused `~/.cache/hermes-node/compile-cache/v1/<gen>/` tree a produced binary otherwise creates on every run |
| `HERMES_NODE_COMPILE_CACHE=<dir>` | relocates that tree |

Plus the generic ones the runtime already honours: `XDG_CACHE_HOME`,
`TMPDIR`, `HOME`, `LANG`/`LC_*`.

Note what the second row implies. A bundle's bytecode is already compiled, so
the compile cache is vestigial in a bundled program -- and an executable
shipped to an end user creates a cache directory it never writes to, in that
user's home, with no way to opt out except an environment variable they have
to know about. That is a wart today rather than a design question, but it is
the kind of thing a configuration surface would absorb.

## What is unreachable in an executable, and why it does not currently matter

`hermes-node`'s own flags are parsed in `tools/hermes-node/hermes-node.cpp`,
which an executable does not link -- that is the point of the two link
configurations. Every flag a bundled program might have wanted is already
answered elsewhere:

- `--inspect` / `--inspect-brk` -- already refused with `--bundle`. Bundled
  bytecode is compiled at `DebugInfoSetting::THROWING`; the debugger needs
  `ALL`.
- `-r` / `--require` -- already refused with `--bundle`. Preloads are a
  property of the artifact instead, recorded in the container by
  `--preload` at build time (format v3). This is already the Bun/SEA model.
- `--optimize` -- affects compilation, which happened at build time.
- `--compile-cache` -- see above; a bundle is already compiled.

So the run-time surface an executable lacks is exactly the surface that would
have been refused or meaningless anyway. Nothing is owed here yet.

## Prior art, for when something is owed

Both runtimes that ship single-file executables split this in two: bake a set
of arguments into the artifact at build time, and allow a limited extension
at run time.

- **Bun**: `--compile-exec-argv="--smol --user-agent=X"` bakes flags in,
  readable from the program as `process.execArgv`. At run time `BUN_OPTIONS`
  extends them without rebuilding.
- **Node SEA**: `execArgv` in the preparation-blob config bakes flags in, and
  `execArgvExtension` decides what may extend them -- `"none"` (nothing, and
  `NODE_OPTIONS` is ignored), `"env"` (the default, `NODE_OPTIONS` applies),
  or `"cli"` (the executable accepts `--node-options="..."`, parsed as
  runtime flags rather than passed to the program).

That third value is the interesting one: it exists because in a single-file
executable *every argument belongs to the program*, so a runtime flag needs
an explicitly reserved spelling to be distinguishable at all.

## The shape, if and when

Three things, in this order. The order is the point.

1. **A `RuntimeConfig` surface on `hermes-node` first.** Whatever knob is
   wanted -- heap size for a memory-constrained deployment is the likely
   first one -- is wanted for `hermes-node script.js` and `--bundle` too, not
   only for executables. Building it into `--build-exe` alone would be
   solving the third case first and would leave the CLI unable to reproduce
   what an executable does.
2. **A build-time way to bake a chosen set into the container.** SEA's
   `execArgv` is the closer model than Bun's, because the container already
   carries a preload table and this is the same kind of thing: a property of
   the artifact rather than of the command line that launches it. Baking
   beats an environment variable an operator has to remember, and it keeps
   the artifact self-describing -- `--dump` could print it the way it prints
   `PRELOADS`.
3. **Only then, a run-time extension, if it is still wanted.** It may not be.

## Questions to settle then, not now

- Does a baked-in config belong in the **container** (so `--bundle` and an
  executable behave identically, and `--dump` can show it) or in the
  **executable** (so the same container can be linked two ways)? The
  container is the more consistent answer and the preload table is the
  precedent, but it means `--build-exe` cannot override what
  `--build-bundle` chose.
- If a run-time extension exists, does it follow SEA in reserving a spelling
  (`--hermes-node-options="..."`), or Bun in using the environment only? The
  reserved spelling costs an argument the program can then never receive.
- Should `process.execArgv` report the baked-in set? Node's SEA does, and
  code that inspects it is not rare.
- Does the vestigial compile cache in a bundled program get switched off by
  default here, rather than needing an environment variable?

## What not to do

Do not add a flag surface to `--build-exe` by itself. The gap is in the
runtime, not in the executable, and fixing it only for the artifact would
make `hermes-node` the odd one out and the two paths hard to keep honest.
