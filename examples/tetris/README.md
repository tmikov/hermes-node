# tetris

A third-party terminal game, unmodified, packaged into a single executable.

`tetris-cli` is somebody else's npm package. Nothing here is written for
hermes-node -- `play.js` requires it and that is the entire program. The
example exists to show a real interactive TUI surviving the whole pipeline:
disk, then an AOT bundle, then one linked binary that needs neither
hermes-node nor `node_modules`.

```sh
npm install
../../cmake-build-release/bin/hermes-node play.js   # play it
./run.sh                                            # everything below, checked
./build-bundle.sh                                   # bundle + executable, kept in ./dist
./dist/tetris                                       # the binary, on its own
```

Controls are the game's own: arrows to move and rotate, space to drop, `s`
to start or stop, `q` to quit.

| file | what it is |
| --- | --- |
| `play.js` | requires `tetris-cli`; that is the whole program |
| `build-bundle.sh` | builds `dist/tetris.hbb` and, if a link kit exists, `dist/tetris` |
| `run.sh` | runs all three ways and checks each draws a playing field |
| `../pty-run.py` | gives a program a real pty, so a TUI can be checked from a script |

## Why this one is the simple example

`tetris-cli` depends on `chalk`, `lodash` and `random-js`. All of it is
CommonJS, none of it calls `require()` with a computed argument, and there
are no native addons -- so the scanner finds every module and **the producer
emits no warnings at all**. `run.sh` asserts that, because it is the reason
this example is short: `examples/gtop` next door is the same idea against a
package that does none of those things, and it needs a screenful of
`--include`.

The interesting part is what the game needs from the runtime, all of which
has to work identically inside a bundle: `process.stdin.setRawMode`,
`process.stdout.columns`, and `readline`'s keypress decoding, including
escape sequences for the arrow keys.

## Checking a TUI from a script

A terminal program will not start without a terminal. `tetris-cli` calls
`stdin.setRawMode()` on its fifth line, and that method does not exist when
stdin is a pipe -- so a plain `./tetris | grep` proves nothing, and neither
does its failure.

`../pty-run.py` allocates a pseudo-terminal of a stated size, runs the
command on it, optionally sends a keystroke, and prints what was drawn.
`script(1)` would also work, but its arguments differ between Linux and
macOS and these examples run on both; python3 is already required to build
this project.

## Sizes

Measured on Linux x86_64, Release:

| artifact | size |
| --- | --- |
| `node_modules` | 5.7 MB, 10 packages |
| `dist/tetris.hbb` | 272 KB |
| `dist/tetris` | 12.7 MB |

The executable is mostly runtime: the bundle is 272 KB of it. That ratio is
the honest shape of this feature -- a small program pays for a whole Hermes
plus the Node compatibility layer, and a large one costs barely more.
