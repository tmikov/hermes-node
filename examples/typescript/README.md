# typescript

The TypeScript compiler itself, type-checking and emitting under hermes-node.

`tsc` is somebody else's npm package and nothing here is written for
hermes-node: `tsc.js` requires `typescript/lib/_tsc.js` and that is the whole
program. What makes it worth an example of its own is its shape. Every other
example here is a graph of small modules; this one is **a single
6,213,092-byte file** with no dependencies of its own, which turns it into the
sharpest measurement of the compile cache in the tree and into a hard case for
native compilation.

```sh
npm install
../../cmake-build-release/bin/hermes-node tsc.js --outDir /tmp/out src/area.ts
./build-bundle.sh                      # dist/tsc.hbb, plus dist/tsc if a kit exists
./run.sh                               # everything below, checked
TSC_BUILD_NATIVE=1 ./run.sh            # and the native build (~2 min, ~6 GB RSS)
```

| file | what it is |
| --- | --- |
| `tsc.js` | requires `typescript/lib/_tsc.js`; that is the whole program |
| `src/area.ts` | four lines of TypeScript with one deliberate type error |
| `build-bundle.sh` | builds the AOT container, and the executable when a kit is there |
| `run.sh` | runs tsc every way this runtime can, checks each, measures the cache |

## What is actually checked

`run.sh` does not settle for an exit status, and the reason is specific to
this program: `tsc` exits 2 for *any* diagnostic, so a compiler that had
failed to load its standard library and was complaining about `Number` would
pass a status check. Four things are asserted instead, all of them things
tsc produced:

- status 2, tsc's own "finished, with errors";
- **exactly one** diagnostic -- a checker running without `lib.d.ts` reports
  errors too, just a great many more;
- that diagnostic verbatim, position included:
  `src/area.ts(12,7): error TS2322: Type 'string' is not assignable to type 'number'.`
- the emitted JavaScript, which is the other half of tsc and can be lost on
  its own: annotations stripped and the CommonJS exports synthesized.

That is not a hypothetical failure mode. It is exactly what a bundled run does
when the `lib*.d.ts` files are not beside the container: eight `TS2318 Cannot
find global type` lines, a `TS6053` naming the `lib.es2020.full.d.ts` it could
not open, and exit 2 -- which a status check calls a pass.

Since the position is asserted, moving the lines in `src/area.ts` means
updating `run.sh`.

All four ways of running the program get the same four assertions, and each of
the three artifacts is then **diffed** against the interpreted run's emit and
diagnostics. Asserting each separately would let them drift as far apart as
the assertions are loose.

## The four ways, measured

macOS arm64, Release. Type-check is `src/area.ts` with the flags `run.sh`
uses, best of 5; startup is `tsc --version`, best of 9. Both timed from a
Python parent around `subprocess.run`, so the interpreter's own startup is
outside the measurement.

| how | type-check | startup |
| --- | --- | --- |
| interpreted, cache off | 2,643 ms | 553 ms |
| interpreted, cache cold (filling it) | 6,617 ms | 5,674 ms |
| interpreted, cache warm | 946 ms | 23 ms |
| `--bundle`, no cache at all | 944 ms | 19 ms |
| `--build-exe` artifact, no cache at all | 940 ms | 13 ms |
| `build-native` artifact | 505 ms | 18 ms |
| node v24.4.1 | 354 ms | 75 ms |

The three artifacts' startup figures are noise apart from each other; what
separates them from the interpreted rows is the compile that is not happening.

## The compile cache, on one six-megabyte module

Measured with a cache directory of the example's own under `./out` -- so the
figures do not depend on the state of the developer's real cache and do not
leave 3.9 MB in it. The cold run lands 2 entries and about 3.9 MB: `_tsc.js`,
and `tsc.js` itself.

The startup column above is the interesting one. `tsc --version` does no
compiling of its own, so what it measures is startup alone, and startup here
is almost entirely Hermes dealing with `_tsc.js`: **about 250x**, from 5,674 ms
to 23 ms. `run.sh` asserts on the type-check figure instead, and requires only
3x: the claim worth defending is that caching one enormous module is worth
multiples, and a tight bound on a wall-clock ratio would go red under parallel
load instead of telling anyone anything.

`run.sh` also diffs the warm run's emit and diagnostics against the cold run's.
A cache is only worth having if it changes nothing but the time.

One row deserves an explanation, because it looks like the cache making the
*program* faster, which a cache has no business doing: type-checking takes
2,643 ms with the cache off and 946 ms warm. Turning the cache on also turns
the optimizer on. `resolveOptimize()` in
`lib/runtime/hermes_node_runtime.cpp` defaults `optimize` to whether a cache
is active, on the argument that optimizing only pays when the result is kept.
So the cache-off row is running unoptimized bytecode, and the cold row is
paying for an optimizing compile of 6.2 MB of JavaScript -- which is why it
costs 6,617 ms where the unoptimized compile in the row above it costs 553 ms
of startup.

## What a bundle buys that the cache does not

The container is compiled with the optimizer on unconditionally, so what it
carries is exactly the bytes a warm cache would have served. It therefore
lands on the warm row -- 944 ms against 946 ms -- **on the first run, on a
machine that has never seen the program, with the cache switched off
entirely.** That is 2.8x faster than the uncached interpreted run and 7.0x
faster than the run that fills a cache.

There is no arrangement in which the cache beats this. A cache's best case is
a tie, and it gets there by first paying the 6,617 ms cold run on every
machine, in every fresh container, after every `git describe` that moves the
generation tag. A bundle pays that once, at build time, on the machine that
builds it.

The rest of what it buys is not about time:

- one file, plus the standard library (see below), rather than a 23 MB
  `node_modules` and a source tree;
- nothing per-machine and nothing stateful -- no `~/.cache` to be cold, to be
  unwritable, to be pruned, or to be absent in a container image;
- with `--build-exe`, no hermes-node on the target machine either. `run.sh`
  checks that one by moving `node_modules` out of the way before running it,
  which proves the claim rather than asserting it.

This README used to say there was no `build-bundle.sh` here because a bundle
would gain nothing the compile cache does not already give. That was
measurably backwards, and the 944-against-6,617 line above is the measurement
that settles it. The two were never alternatives in the first place: a cache
makes the *next* run of a program on *this* machine cheaper, and a bundle is
how the program gets to another machine at all.

### Arguments for a bundled program go after `--`

```sh
hermes-node --bundle=dist/tsc.hbb -- --noEmit src/area.ts
```

Without the `--`, hermes-node parses the flags as its own. For anything it
does not recognise that is an error (`unknown option '--noEmit'`), which is
survivable. For anything it does recognise it is **silent**:
`hermes-node --bundle=dist/tsc.hbb --version` prints hermes-node's version,
which looks exactly like a tsc version string until you read it. The
executable needs no `--`, because there every argument belongs to the program
already.

`--build-exe` has the mirror-image rule at build time: its options go *before*
the container, never after. `--build-exe=out tsc.hbb --kit=...` is refused by
name.

## tsc's standard library has to travel beside every artifact

`tsc` finds `lib.es2020.d.ts` and its ninety-nine siblings by looking beside
the file it is executing. A bundled module's `__dirname` is its build-time
identity re-rooted at the container's own directory -- and at the
executable's own directory for a `--build-exe` or `build-native` artifact --
so the 100 `lib*.d.ts` files have to be at
`<artifact dir>/node_modules/typescript/lib`. `build-bundle.sh` puts them
there once, which serves the container and the executable both, since it
writes both into the same directory; `run.sh` does the same for the native
build, which goes somewhere else.

This is the producer packaging code and not data, the same reason `gtop`'s
terminfo travels beside its artifact.

## The producer's warnings here are all benign

Unlike `tetris` and `ditz2`, this graph is not fully static, so `run.sh` does
not assert a silent build. Three warnings, and each is a thing `_tsc.js` does
inside a `try`:

- `source-map-support` -- an optional dependency, not installed, loaded only
  by `tryEnableSourceMapsForHost()`;
- `inspector` -- a Node builtin hermes-node does not have, reached only under
  `--cpu-prof`;
- one computed `require()` at `_tsc.js:5023:28` -- `ts.sys.require()`, which
  loads a module named at run time (a custom transformer, say). A plain
  `tsc` invocation never reaches it.

None is on the path this example exercises, which is why every artifact here
works without an `--include` for any of them.

## The native build, and why it is behind a variable

`TSC_BUILD_NATIVE=1 ./run.sh` compiles the whole program to machine code with
`hermes-node build-native`. It is not part of the default run, and not part of
`check-hermes-node-examples`, because of what it costs. Measured, same
machine:

| | |
| --- | --- |
| build time | 102.9 s (`shermes` + `cc` on `_tsc.js` alone: 102.4 s) |
| peak RSS | 6,205,259,776 bytes (5.8 GB) |
| generated C | 68,044,125 bytes, from 6,213,092 bytes of JavaScript |
| object file | 28,015,832 bytes |
| artifact | 44,046,416 bytes, thin arm64 |

The peak RSS is the number that keeps this out of the suite. The
native-compilation spec records ~3.66 GB as the realistic `cc` ceiling it had
measured, on a 1,500-module build; one six-megabyte module beats that by
nearly a factor of two, because the peak is set by the largest single
generated C file rather than by the module count, and `--jobs` cannot help
with a graph that is one big file.

What it buys is the only hermes-node row in the table above that beats a
bundle rather than tying it: 505 ms against 944 ms on the type-check, which is
real compiler work running roughly twice as fast, and the only row that closes
any of the gap to node's 354 ms. Startup is unchanged, because there was
nothing left to remove.

`run.sh` checks one thing about the artifact beyond running it and diffing it:
`test/fixtures/native/count-magic.py <binary> 1`, which requires exactly one
Hermes bytecode blob -- Hermes's own extensions unit. A second would mean some
module had been packaged as bytecode after all, and the build was not fully
native.

## Sizes

| | build time | size |
| --- | --- | --- |
| `node_modules` | `npm install` | 23 MB, 1 package |
| `node_modules/typescript/lib/_tsc.js` | | 6,213,092 bytes |
| compile cache after one run | 6.6 s | 3.9 MB, 2 entries |
| `--build-bundle` container | 5.8 s | 4,090,032 bytes |
| `--build-exe` executable | 0.1 s to link | 16,754,896 bytes |
| `build-native` executable | 102.9 s | 44,046,416 bytes |
| the standard library beside an artifact | | 100 files, 3,141,835 bytes |

The executable is the container laid on top of 12,664,864 bytes of runtime,
and linking it is free next to producing the container it embeds. The native
artifact is about 2.6x the executable, and essentially all of the difference
is `_tsc.js` compiled to arm64.
