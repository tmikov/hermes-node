# gtop

A third-party system monitor, unmodified, packaged into one executable plus
its data files.

`gtop` is somebody else's npm package -- a blessed dashboard over
`systeminformation`, with CPU history, memory and swap gauges, network,
disk and a process table. `monitor.js` requires it and that is the whole
program. It is the awkward sibling of `examples/tetris`: the same idea
against a package that does everything a static bundler cannot follow.

```sh
npm install
../../cmake-build-release/bin/hermes-node monitor.js   # run it
./run.sh                                               # everything below, checked
./build-bundle.sh                                      # bundle + executable, kept in ./dist
./dist/gtop                                            # the binary
```

| file | what it is |
| --- | --- |
| `monitor.js` | `require('gtop').init()` -- see below, the `.init()` matters |
| `build-bundle.sh` | the `--include` list and the terminfo staging live here |
| `run.sh` | runs all three ways and checks the dashboard draws |
| `../pty-run.py` | gives a program a real pty, so a TUI can be checked from a script |

## Requiring gtop is not the same as running it

`require('gtop')` returns `{init, monitor}`. Its module body builds the
screen and the grid, so **every panel draws** -- and stays empty forever,
because the monitors that fill them only start when `init()` is called,
which is what gtop's own `bin/gtop` does.

An empty dashboard looks like a working one at a glance. It is a convincing
way to demo nothing, and it is what this example did until `run.sh` learned
to count the percentage readings rather than the panel titles. gtop also
installs `process.on('uncaughtException', function () {})`, so anything that
does go wrong inside it fails silently -- worth knowing before debugging one
of these.

## The two things this example is actually about

**blessed loads its widgets by computed name.** `lib/widget.js` does
`require('./widgets/' + name)` in a loop. No scanner can follow that, so the
producer warns and packages none of them, and the run-time loader throws
`MODULE_NOT_FOUND` for the first widget the layout touches -- naming the
exact `--include` to add. `build-bundle.sh` derives the list from the
directory rather than hardcoding it, so a blessed upgrade that adds a widget
cannot silently drop it.

This is the closed world behaving as designed rather than getting in the
way. A computed specifier is not knowable until the program runs, so the
choice is between refusing it and quietly reading code off the deployment
machine. Refusing it, with a message naming the fix, is the useful half.

**blessed ships its own terminfo database**, under `usr/`, and reads it
relative to `__dirname`. Those are data files; the bundler packages
JavaScript and JSON and nothing else. So they travel beside the artifact, at
the path the bundled module still resolves -- `node_modules/blessed/usr`,
because a bundled module's `__dirname` keeps the identity it had at build
time. `run.sh` asserts the staged tree contains no `.js`, since JavaScript
appearing there would mean something was left on disk rather than packaged.

The result is honest about itself: this artifact is a binary **plus a
directory**, and `build-bundle.sh` says so when it finishes.

## What still is not packaged

A successful build still prints warnings, and every one of them is correct.

Five specifiers resolve to nothing at build time:

- `term.js` and `pty.js`, from blessed's `terminal` widget. Optional
  dependencies that are not installed; the widget itself is packaged, and
  nothing in this dashboard instantiates it.
- `osx-temperature-sensor` (`systeminformation/lib/cpu.js`) and
  `macos-temperature-sensor` (from **two** sites, `cpu.js` and
  `graphics.js`). macOS-only optional dependencies, absent on Linux.

The producer leaves each to the run-time loader, which throws the same
`MODULE_NOT_FOUND` a disk run would; an optional-dependency probe catches it
and moves on, which is what happens here.

Two computed calls are also reported, and only one of them is the widget
loader:

- `blessed/lib/widget.js:48:33` -- the `require('./widgets/' + name)` the
  `--include` list exists for.
- `event-stream/index.js:587:20` -- reached through `systeminformation`, and
  never called on this path.

A computed call is a warning rather than an error because the static walk
reaches code the run never does. The `--include` list is how you convert the
ones that matter into packaged modules; the rest stay warnings, and the
closed world catches any that turn out to matter after all.

## Sizes

Measured on Linux x86_64, Release:

| artifact | size |
| --- | --- |
| `node_modules` | 18 MB, 56 packages |
| `dist/gtop.hbb` | 2.3 MB |
| `dist/gtop` | 14.4 MB |
| terminfo beside it | 744 KB, mostly two bitmap font JSONs |

## A caveat worth stating

`systeminformation` gets much of what it reports by running command-line
tools and reading `/proc`. Bundling changes none of that: the executable
still shells out, and still needs those tools present. Packaging a program
does not make it independent of its environment -- only of its
`node_modules`.
