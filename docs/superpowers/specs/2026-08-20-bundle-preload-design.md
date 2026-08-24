# Design: bundle preloads

**Status:** Draft for review, 2026-08-20.

Follows `docs/superpowers/specs/2026-08-19-closed-world-bundle-design.md`, which made
a bundle a closed world. This settles what `-r/--require` means once that is
true.

## The problem

`-r <path>` preloads a module before the main script. It is an ordinary
application mechanism, not only a debugging one --
`examples/flow-bundler/run.sh` uses it to install `@babel/register` before
the bundler's own entry point runs:

```sh
"$HERMES_NODE" -r "$HERE/babel-register.js" "$HERE/bundler/buildBundleCLI.js"
```

In bundle mode it does the wrong thing twice over.

**It reads the disk.** Preloads run at step 12c of `runHermesNode`, before
`runBundle` at step 13, through Node's own `Module._preloadModules` against
the real filesystem. So the one file a bundled program needs *first* is the
one file that is not in the container.

**It is an injection point.** Because the preload runs before the bundle
loader is installed, it can reach into the run that follows. Demonstrated
against `dedb781`: a preload that plants `Module._cache[<root>/<identity>]`
replaces a bundled module's exports, and the program never sees the
container's copy.

```
$ hermes-node --bundle=app.hbb
APP got: REAL from container
$ hermes-node --bundle=app.hbb -r ./poison.js
PRE: planted a cache entry for the bundled dep.js
APP got: POISONED by the preload
```

That is not a hole in the closed world as such -- the operator typed the
flag, and `Module._cache` being the loader's only cache is a deliberate
decision (see `progress-aot-bundle.md`). But it means `--bundle` alone and
`--bundle -r x.js` are different worlds, and nothing says so.

## The decision

The preload becomes a property of the artifact, not of the command line.
`--build-bundle --preload=<specifier>` packages the module and records that
it runs before the entry; `--bundle` runs it automatically. Run-time `-r` is
rejected in bundle mode.

A bundle does a specific thing. It is not a general-purpose runtime that
must accept every flag, and the operator of a sealed artifact does not get
to insert code into it. Rejecting `-r` also removes the injection point
above by construction rather than by mitigation: there is no preload phase
left for an outsider to occupy.

## Flag surface

```
hermes-node --build-bundle=app.hbb --preload=./register cli.js
hermes-node --bundle=app.hbb                 # runs ./register, then cli.js
hermes-node --bundle=app.hbb -r ./x.js       # error: --bundle cannot be
                                             # combined with -r/--require
```

`--preload` is repeatable and preserves flag order. Two `--preload` values
that resolve to the same module collapse to one table entry: the second run
would be a `Module._cache` hit and execute nothing, so recording it twice
would promise something the loader cannot do. `-r` with
`--build-bundle` is unchanged and still resolves from disk: a build runs in
the disk world, and its preloads are a build-time concern.

## `--preload` is a discovery root

Not "`--preload` implies `--include`". There is no implication between two
user-facing flags. The producer has one mechanism -- seed a path onto the
worklist before the single walk -- with two callers today, the entry and
`--include` (`lib/bundle/bundle_build.cpp`):

```cpp
pathIndex.emplace(*resolved, static_cast<uint32_t>(paths.size()));
paths.push_back(*resolved);   // the one walk below handles it
```

`--preload` is a third caller that additionally appends to the preload
table. It resolves from the entry's directory exactly as `--include` does,
against the same `DiskFileSource`, so a `package.json` read while resolving
one is packaged like any other.

It cannot be otherwise. A recorded preload that is not packaged is a
container that cannot run: the run would look up an identity with no module
behind it, so the producer would have to reject it at build time. "Record it
but do not package it" has no valid meaning to express, and requiring
`--include=./setup --preload=./setup` would be two spellings of one thing
with a build error for writing half.

The typical preload also needs the seeding. A register or polyfill module is
required by *nothing* -- that is what it is for -- so the ordinary walk never
reaches it. When the target *is* already reachable from the entry, the
existing `pathIndex.count(*resolved)` check makes the seed a no-op and
`--preload` contributes only the ordering record.

## Format v3: the preload table

A new section beside strings, modules, edges and payload: an array of
`uint32_t` module indices, in flag order.

```cpp
constexpr uint32_t kBundleFormatVersion = 3;

struct BundleHeader {
  // ... existing fields ...
  uint32_t preloadTableOffset;
  uint32_t preloadCount;
};
```

A section rather than another `flags` bit on the module record, because
order is part of the meaning and a flag cannot carry it. Indices rather than
string identities, because they are fixed width and validated once at open.

`BundleReader::openImpl` validates every entry: in range, and `kRequirable`.
A preload naming a resolution-input `package.json` is a malformed container,
not a run-time surprise. `--dump` prints the table, in order, so a container
says what it will run before it runs it.

The version bump is the cost. Nothing needs a compatibility path: `open()`
and `openForInspection()` already reject a version mismatch outright.

## Run order

The bootstrap sequence does not change. Because `-r` is refused in bundle
mode, step 12c simply never co-occurs with `--bundle`, and the whole change
lives inside `runBundle`, which already has the seam:

```
open -> install globals -> run bundle-loader -> installBundleLoader(...)
                                                     |
                                                  run()  <- new
```

`installBundleLoader` returns `runEntry` today; it returns `run()` instead.
A new native `bundle.preloads()` hands JavaScript the recorded identities,
and `run()` loads each through the same `loadIdentity` every bundled module
goes through -- so a preload's own `require`s are container requires, with
no second code path -- and then loads the entry.

The entry keeps `isMain`, unless a preload transitively requires the entry
itself, in which case that `require()` instantiates the entry first and the
final, top-level entry load is a `Module._cache` hit that runs the entry as
a non-main module -- Node-faithful, not a bug: checked against real Node
v24.13.1 (`node -r ./cli.js ./cli.js`), which behaves identically, down to
`module.id`. A preload never becomes `require.main` or `process.mainModule`.
Note what follows: while a preload runs, `require.main` is not yet set,
because the entry has not been loaded. That matches Node, where `-r`
preloads also run before the main module exists, and it is worth stating
because a preload that reads `require.main` sees `undefined` rather than
itself or the entry.

A preload that throws propagates out of `run()` and the entry never runs,
which is what Node's `-r` does.

## Error policy

| Situation | Behavior |
| --- | --- |
| `--bundle` with `-r`/`--require` | Error naming both flags, from `checkToolOptions()` |
| `--preload` without `--build-bundle` | Error naming both flags, same place |
| `--preload` that does not resolve | Build error |
| `--preload` resolving to something unpackageable | Build error naming the reason |
| `--preload` that does not parse or compile | Build error, naming it as the preload |
| Preload index out of range, or not `kRequirable` | Container rejected at open |
| A preload throws at run time | Propagates; the entry does not run |

The last row is the one the walk would otherwise tolerate. A non-entry file
the parser or compiler rejects is packaged as a module that throws when
required, because the run may never reach it -- but a preload is reached by
definition, so it fails the build for the same reason the entry does.

## Out of scope

- **No `--no-preload`.** The artifact decides what runs; a run-time opt-out
  hands that back to the command line, which is the property being removed.
- **No disk preload in bundle mode, under any flag.** `--bundle` already
  refuses `--inspect`: debugging happens from source, not from the
  container. A `--preload-from-disk` would reopen by the front door what
  `dedb781` closed.
- **No automatic discovery of preloads.** The producer cannot know that a
  module is meant to run first; that is what the flag is for.

## Testing

Producer:
- `--preload=./setup` packages `setup.js` and records it, with `setup.js`
  required by nothing, so it is in the container only because of the flag.
- `--dump` prints the preload table in flag order.
- Two `--preload` flags record both, in order; the same value twice records
  one entry.
- A `--preload` already reachable from the entry yields one module record
  and one preload entry.
- An unresolvable `--preload`, and one resolving to a `.node` addon, are
  build errors.

Consumer, each with the source tree deleted before the run:
- The preload runs before the entry, and its output precedes the entry's.
- A preload's own `require` resolves from the container.
- A preload is not `require.main`: it observes `undefined` while it runs,
  and the entry is `require.main` once the entry runs.
- Two preloads run in flag order.
- A throwing preload stops the run before the entry executes.

Flags and format:
- `--bundle -r x.js` errors naming both flags; this is the regression test
  for the injection point, which is now unreachable.
- `--preload` without `--build-bundle` errors.
- `BundleFormatTest`: v3 round-trip, and rejection of a preload index that
  is out of range or names a non-requirable module.

## Risks

**A container can now run code before its entry, and only `--dump` says
so.** That is the point of the feature, but it is worth stating: a bundle is
no longer "run the entry". Anyone auditing a container needs to read the
preload table, which is why `--dump` prints it rather than making it
inspectable only through the format.

**`examples/flow-bundler` is the natural end-to-end case and is not
bundled today.** Bundling it would exercise this properly. That is a larger
piece of work than this design -- the bundler loads Flow sources through
`@babel/register` at run time, which is the computed-require problem in its
strongest form -- so this design is tested with synthetic fixtures, and
the example is left as it is.
