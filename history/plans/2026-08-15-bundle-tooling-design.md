# Design: Bundle Tooling

**Status:** Draft for review, 2026-08-15.

## Goal

Make a bundle inspectable. Today `--build-bundle` prints one line and the
container is opaque afterwards: answering "what went in, and why" means
running `strings` over the file. This adds four capabilities, all of them
diagnostic and none of them on the run path.

1. `--verbose`, so the producer says what it is doing while it does it.
2. A dump of a container's tables.
3. Extraction of one module's payload to a file.
4. Disassembly of a standalone bytecode file.

`hermes-node --build-bundle` is a compiler. Diagnostics come from explicit
flags, never from the environment. `HERMES_NODE_DEBUG_NATIVE=BUNDLE` stays
what it is -- a runtime channel for the consumer's fallback log -- and gains
nothing here.

## Flag surface

`--bundle=<file>` names the container. The verb is separate and defaults to
"run it", which is what `--bundle` does today.

```
hermes-node --bundle=app.hbb                    # run (unchanged)
hermes-node --bundle=app.hbb --dump             # print the container's tables
hermes-node --bundle=app.hbb --extract-module=lib/util.js --out=util.hbc
hermes-node --dump-bytecode=util.hbc            # disassemble a bytecode file
hermes-node --build-bundle=app.hbb --verbose ./cli.js
```

Rules, each a hard error with a message naming the flags involved:

| Situation | Behavior |
| --- | --- |
| `--dump` or `--extract-module` without `--bundle` | Error |
| `--dump` together with `--extract-module` | Error, two verbs |
| `--extract-module` without `--out` | Error |
| `--out` without `--extract-module` | Error |
| `--verbose` without `--build-bundle`, `--dump`, or `--dump-bytecode` | Error |
| `--dump-bytecode` with `--bundle` or `--build-bundle` | Error |
| `--dump`, `--extract-module`, `--dump-bytecode` with `--inspect` | Error |

`--dump` and `--extract-module` never execute the bundled program, so the
existing `--bundle` + `--inspect` refusal is not weakened by them; they are
refused alongside it for one reason only, that an inspector session with no
program to inspect is a mistake worth naming.

`--verbose` is a boolean. No levels: the three consumers each have one
useful extra tier and nothing beyond it.

## Inspection mode in the reader

`BundleReader::open` hard-errors on a generation mismatch, which is right
for running and wrong for inspecting -- a container built by another version
is exactly the one worth looking at. `--dump` and `--extract-module`
therefore open in inspection mode: structural validation is unchanged (magic,
format version, every offset, length, and index), but the generation tag is
reported rather than enforced.

This is a new entry point, not a flag on the existing one, so no caller can
reach it by accident:

```cpp
/// Opens without enforcing the generation tag. For inspection tools only:
/// bytecode from a mismatched generation must never be executed.
static std::optional<BundleReader> openForInspection(
    const uint8_t *data, size_t size, std::string *error);
```

`open()` keeps its signature and its hard error. A mismatch found in
inspection mode is printed as a line in the dump, next to the tag the
running binary would require.

Format version mismatch stays fatal in both modes. The tables are laid out
by the format version; a reader that guessed at a different layout would
print confident nonsense.

## 1. `--verbose` for `--build-bundle`

Three phases, to stderr so a dump piped to a file stays clean.

**Configuration**, before any work:

```
entry:      /home/t/app/greet.js
output:     /home/t/app/app.hbb
generation: 0x8f2a1c04  (hermes-node 0.1.0, x86_64, bytecode 96, optimized)
optimize:   on
```

**Discovery**, one line per module in discovery order, with each resolved
specifier under it. This is where provenance lives: the question the current
producer cannot answer is "why is this file in my bundle", and the answer is
the chain of importers printed here.

```
discover  [  0] greet.js
  require 'yargs/yargs'         -> node_modules/yargs/yargs
  require 'yargs/helpers'       -> node_modules/yargs/helpers/index.js
discover  [  1] node_modules/yargs/yargs
  require './build/index.cjs'   -> node_modules/yargs/build/index.cjs
  known   './lib/util'          -> [3]
  skip    'bufferutil'          (.node is not packageable)
```

`known` marks a specifier resolving to an already-discovered module, which
is what makes cycles and shared dependencies visible. `skip` carries the
same reason string the warning already prints, so `--verbose` is a superset
of default output rather than a different view of it.

**Compilation**, one line per JavaScript module:

```
compile   [  0] greet.js                  4812 src -> 12480 bc  (2.6x)   18.4 ms
```

**Summary**, always last:

```
build root: /home/t/app
modules:    21  (19 js, 2 json)
edges:      26  (18 distinct specifiers)
strings:    47 entries, 3912 bytes
payload:    248320 bytes
largest:    node_modules/yargs/build/index.cjs  96412 bytes
total:      261224 bytes
compile:    412.7 ms
```

Without `--verbose` the producer's output is exactly what it is today: the
warnings, the errors, and `bundle root:`.

## 2. `--bundle=<file> --dump`

Human-readable text on stdout. Never bytecode bytes.

```
bundle: app.hbb   format v1  generation 0x8f2a1c04
entry:  [0] greet.js

MODULES (21)
  idx  kind  bytes    identity
    0  js     12480   greet.js
    1  js      3120   node_modules/yargs/yargs
    2  json      412   node_modules/yargs/package.json

EDGES (26)
  greet.js            'yargs/yargs'    -> [1]
  greet.js            'yargs/helpers'  -> [4]

SECTIONS
  strings   3912 B    modules    672 B
  edges      312 B    payload  248320 B
total 261224 bytes
```

Edges print in table order, which is the sorted order the runtime binary
searches: by importer index, then by specifier bytes. Printing them as
stored rather than regrouped is deliberate -- if the sort is ever wrong,
this dump is where it shows.

A generation mismatch adds one line under the header:

```
generation: 0x8f2a1c04  MISMATCH (this binary requires 0x1b70e93a)
```

`--verbose` adds, per module, the count of edges pointing at it and the
count leaving it, which identifies both the widely shared dependencies and
the leaves.

## 3. `--bundle=<file> --extract-module=<identity> --out=<path>`

Writes one module's payload verbatim: serialized Hermes bytecode for a
JavaScript module, UTF-8 text for a JSON one. No header is added, so the
output of a JavaScript extraction is a bytecode file that `--dump-bytecode`
reads directly.

The identity is the one `--dump` prints, matched exactly. An unknown
identity is an error listing the closest few by edit distance, since these
are long paths and a typo in one is the expected failure.

`--out` is required and is never inferred from the identity: writing a file
the user did not name is how a tool overwrites something it should not.

Extraction opens in inspection mode. Getting bytecode out of a container the
current binary refuses to run is a reason to have the feature, not a reason
to withhold it.

## 4. `--dump-bytecode=<file>`

Disassembles a standalone bytecode file, using Hermes's own disassembler:
`BCProviderFromBuffer::createBCProviderFromBuffer` then
`hbc::BytecodeDisassembler`, the same path `tools/hbcdump` takes.

Default output is the bytecode header, the section ranges via
`BytecodeSectionWalker::printSectionRanges`, and a pretty disassembly with
function IDs and virtual offsets:

```cpp
DisassemblyOptions::Pretty | DisassemblyOptions::IncludeFunctionIds |
DisassemblyOptions::IncludeVirtualOffsets
```

`--verbose` adds `IncludeSource`, which emits a `; <file>:<line>:<column>`
comment for each instruction where debug info allows. It is the source
*location*, not the source text: a bytecode file does not carry the text.

Two input shapes are accepted. A raw bytecode file is the normal case. A
compile-cache entry -- which is a `kCompileCacheHeaderSize` header followed
by the bytecode -- is detected by its magic and the header skipped, because
the cache directory is full of these and telling a user to hexdump past a
header would be a poor answer.

As shipped, "anything else fails with the message Hermes itself produces for
a bad bytecode buffer" holds for a file this binary does not recognize, and
two cases were added on top of it, both approved in review. A file carrying
the cache magic but failing one of the cache header's own checks (a header
version this binary does not read, a bytecode size the file cannot hold, a
size of zero, or a file shorter than the header) is reported against the
cache header, because such a file is never valid bytecode either and
Hermes's message would blame a bytecode magic that was never the problem. A
bundle container is named as one, with a note pointing at the two verbs that
do read a container. Both are more specific than Hermes's own diagnosis
because this binary wrote both formats and can say so.

Alignment: `BCProviderFromBuffer` requires an aligned buffer, which
`llvh::MemoryBuffer::getFile` provides for a whole file. A cache entry read
past its header is not aligned, so that path copies the bytecode into an
aligned buffer rather than pointing into the mapping.

## Where the tool verbs run

`--dump`, `--extract-module`, and `--dump-bytecode` need no JavaScript
runtime, no event loop, and no `napi_env`. They are handled in `main()`
before `runHermesNode` is called, and return directly.

This is not only faster. Bootstrapping a runtime in order to describe a file
would mean a diagnostic tool could fail for reasons that have nothing to do
with the file being diagnosed, which is the opposite of what a diagnostic
tool is for.

`--build-bundle` is unaffected: it compiles, so it needs the `napi_env` and
stays where it is, at step 13 of the bootstrap sequence.

## Library structure

The existing split is that `hermesNodeBundle` (format only) links no Hermes
VM, which is what lets `BundleFormatTest` run with no runtime, while
`hermesNodeBundleRun` links `hermesNapi` but neither the parser nor the
compiler.

Two new targets rather than one, because the three verbs do not have the
same dependencies:

- `hermesNodeBundleTools` -- `--dump` and `--extract-module`. Both only read
  the container's tables and copy bytes, so this links `hermesNodeBundle`
  and nothing else. Keeping it VM-free means its logic is unit-testable with
  no runtime, exactly as the format layer already is.
- `hermesNodeBytecodeDump` -- `--dump-bytecode`. The disassembler lives in
  the VM libraries, so this one links `hermesvm_a`. It reads a bytecode
  file, and the fact that one can be extracted from a container is not its
  concern. As shipped it does include `bundle_format.h`, for `kBundleMagic`
  alone, so that a container pointed at it is named rather than reported as
  a bad bytecode magic; the header is a handful of constants and costs no
  link dependency, so the target still links `hermesvm_a` and nothing else
  of ours.

Folding both into a single target would give dump and extract a VM
dependency neither needs, for no gain.

`--verbose` is producer-side and adds nothing new: it goes in
`hermesNodeBundleBuild`, which already links what it needs.

## Testing

Lit, one file per feature, each asserting the specific text that identifies
the behavior rather than that something was printed:

- `test/bundle-verbose.js` -- build a fixture with a shared dependency and a
  cycle; assert a `known` line for the second reference and a `skip` line
  for a `.node`; assert the summary's module and edge counts; assert that
  without `--verbose` none of it appears.
- `test/bundle-dump.js` -- build, dump, check header, a module row, an edge
  row, and the totals. Then a bundle whose generation is deliberately
  altered, checking the `MISMATCH` line and a zero exit.
- `test/bundle-extract.js` -- extract a JavaScript module and feed it
  straight to `--dump-bytecode`; extract a JSON module and compare bytes
  with the original file. An unknown identity errors and suggests.
- `test/bundle-dump-bytecode.js` -- disassemble a compiled fixture, assert a
  known function name and a section-range line; the same for a compile-cache
  entry; a non-bytecode file errors cleanly.
- `test/bundle-tool-errors.js` -- every row of the flag-surface table.

Unit tests extend `BundleFormatTest`: `openForInspection` accepts a
generation mismatch, still rejects bad magic, a bad format version, and each
truncation case.

## Out of scope

- JSON output. Text only; add a format flag if something needs to consume it.
- Extracting all modules at once, and extracting by index rather than
  identity.
- Disassembling straight out of a container without the extract step.
- Editing a container. These tools are strictly read-only.
- Any change to what `--build-bundle` produces. Byte-for-byte identical
  containers before and after this work, `--verbose` or not.
