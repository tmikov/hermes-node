# ditz2

A real command-line application, kept as a submodule, packaged into a single
executable.

[ditz2](https://github.com/tmikov/ditz2) is an issue tracker that keeps issues
as files in your repository. It is a submodule rather than an npm dependency
for a plain reason: it is not published to npm, so there is nothing to
`npm install`. It is also the first example here that is written in
TypeScript, and the first that hermes-node cannot run as its author ships it
-- see below.

```sh
git submodule update --init ditz2      # from this directory
npm install
./build-cjs.sh                         # recompile the submodule as CommonJS
./run.sh                               # everything below, checked
./build-bundle.sh                      # bundle + executable, kept in ./dist
./dist/dz --help                       # the binary, on its own
```

Then use it like any tracker:

```sh
cd /some/project
export DZ_AUTHOR="Jane Doe <jane@example.com>"   # or let it probe git
/path/to/dist/dz init --name myproject
/path/to/dist/dz add "Parser drops trailing newline" --type bug
/path/to/dist/dz list
```

| file | what it is |
| --- | --- |
| `ditz2/` | the upstream repository, as a submodule |
| `tsconfig.cjs.json` | the submodule's own tsconfig with two settings changed |
| `build-cjs.sh` | recompiles `ditz2/src` into `./dist-cjs` |
| `build-bundle.sh` | builds `dist/ditz2.hbb` and, if a link kit exists, `dist/dz` |
| `run.sh` | runs all three ways and drives a real workflow through each |

## The catch: ditz2 ships ESM, and there is no ESM loader yet

ditz2 is `"type": "module"`. Pointed at the `dist/` its own build produces,
hermes-node fails in the CommonJS loader's hand-off to an ES module loader
that does not exist:

```
TypeError: undefined is not a function
    at getOrInitializeCascadedLoader (internal/modules/esm/loader:855:34)
```

So `build-cjs.sh` recompiles the submodule's TypeScript with `module:
commonjs`. **No ditz2 source is patched.** `tsconfig.cjs.json` is the
submodule's own tsconfig with exactly two settings changed -- `module` and
`moduleResolution`, which have to move together, since NodeNext resolution is
not allowed with CommonJS emit. What runs in all three modes below is ditz2's
real logic, not a port of it.

That it recompiles at all is luck rather than design: the source contains no
`import.meta` and no top-level `await`, either of which has no CommonJS
equivalent and would have made this a rewrite. A package that used either
could not be run here at all today.

When the ESM loader lands, delete `build-cjs.sh` and `tsconfig.cjs.json` and
point the example at `ditz2/dist`. Nothing else here should need to change.

## What it exercises

Unlike `tetris` and `gtop` next door, nothing here draws a screen. This one is
about the filesystem and process surface a normal CLI tool leans on, and it
found two real bugs in that surface:

- **Atomic writes.** ditz2 writes config by writing a temporary, renaming it
  over the target, and removing the temporary in a `finally`. That last step
  is a no-op by design -- the rename already consumed the file -- and it threw
  ENOENT, because `fs.rmSync`'s `force` option was dropped in the binding.
  Every `dz init` failed on its success path.
- **`crypto.randomUUID`.** ditz2 mints a token per lock acquisition. The
  function did not exist, so every `dz add` died with `undefined is not a
  function`.

Both are fixed. Also exercised, and working already: `child_process` (ditz2
probes git and Sapling for your identity), `os.hostname`, `path`, and enough
of `fs` to read, write and scan a directory of Markdown files.

`run.sh` does not settle for exit codes. For each of the three modes it
creates a project in a temporary directory, files two issues, lists them,
counts the files on disk, closes one by id prefix, confirms it comes back
closed, and finishes with ditz2's own `doctor` check. It sets `DZ_AUTHOR` so
the run does not depend on the machine having a git identity.

## Why this one needs no `--include`

Once it is CommonJS, ditz2's graph is entirely static: no computed
`require()`, no native addons, and its three runtime dependencies
(`commander`, `uuid`, `yaml`) are literal requires the whole way down. The
producer emits **no warnings at all**, and `run.sh` asserts that, because a
warning appearing later would mean a dynamic require had crept in and the
closed world needed an `--include` to cover it.

It is the same shape as `tetris`, and the opposite of `gtop`, which loads
every widget through `require('./widgets/' + name)` and needs a screenful of
flags.

## Sizes

Measured on Linux x86_64, Release:

| artifact | size |
| --- | --- |
| runtime dependencies | 2.5 MB, 3 packages |
| build-only dependencies | 26 MB (TypeScript and `@types/node`) |
| `dist-cjs` | 228 KB, 38 files |
| `dist/ditz2.hbb` | 540 KB, 140 modules, 431 edges |
| `dist/dz` | 12.5 MB |

The compiler dwarfs everything it compiles, and none of it ships: `dist/dz`
needs neither TypeScript, nor `node_modules`, nor `dist-cjs`, nor
hermes-node. `run.sh` proves that by moving all three out of the way before
running the binary.

On Linux the executable is not fully self-contained -- `ldd` shows ICU 74,
`libstdc++` and `libgcc_s`. That is a property of the feature, not of this
example; see the single-file executables section in the top-level
`CLAUDE.md`.
