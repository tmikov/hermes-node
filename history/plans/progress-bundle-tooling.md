# Implementation Progress

Tracks progress on `history/plans/2026-08-15-bundle-tooling-plan.md`
(implementation plan) and its companion design doc
`history/plans/2026-08-15-bundle-tooling-design.md`.

## Status

| Step | Description | Status |
|------|-------------|--------|
| Task 1 | Reader inspection API (`openForInspection`, header/edge accessors) | done |
| Task 2 | `--verbose` for `--build-bundle` | done |
| Task 3 | `--bundle=<file> --dump` | done |
| Task 4 | `--bundle=<file> --extract-module=<identity> --out=<file>` | done |
| Task 5 | `--dump-bytecode=<file>` | done |
| Task 6 | Flag conflicts, docs, progress file | done |

All six tasks are complete. Each of Tasks 1 through 5 went through at least
one review round; the per-task ledger, including every deferred minor, is in
`.superpowers/sdd/2026-08-15-bundle-tooling-plan/progress.md`.

## What shipped

Four diagnostic flags, none of them on the run path, and none of them
changing what `--build-bundle` produces:

- `--build-bundle=<f> --verbose` narrates configuration (entry, absolute
  output path, the generation tag with the version/architecture/bytecode
  format/optimization it folds, and the optimization setting), discovery
  (each module with the specifier that pulled it in, `known` for an
  already-discovered target, `skip` for an unpackageable one), compilation
  (source and bytecode bytes, their ratio, and timing per module), and a
  summary of the finished container: modules by kind, edges and distinct
  specifiers, string table entries and bytes, payload and bytecode bytes,
  the largest single module, the total file size, and total compile time.
  All of it to stderr. Verified byte-for-byte that the container is
  identical with and without the flag.
- `--bundle=<f> --dump` prints the header, module table, edge table and
  section sizes to stdout, with `--verbose` adding per-module in/out edge
  counts. Edges print in stored order, which is the order the run path
  searches: if the sort is ever wrong, this is where it shows.
- `--bundle=<f> --extract-module=<identity> --out=<file>` writes one
  module's payload verbatim, through a temp file renamed into place. An
  unknown identity lists up to three of the container's own identities
  within an edit distance of a third of the given one.
- `--dump-bytecode=<f>` disassembles a raw bytecode file or a compile cache
  entry, using Hermes's own `BytecodeDisassembler`, with `--verbose` adding
  source locations -- `; <file>:<line>:<col>` per instruction, where debug
  info allows. It never prints source text.

Two structural properties the design asked for, both verified by
observation rather than assumed:

- The three read-only verbs run before any runtime exists. `runToolVerb()`
  in `tools/hermes-node/hermes-node.cpp` dispatches them from `main()`
  ahead of `runHermesNode`; under gdb, `--dump` reaches neither
  `runHermesNode` nor `uv_loop_init`.
- `hermesNodeBundle` and the new `hermesNodeBundleTools` link no Hermes VM
  library, read from the generated `build.ninja`, so `BundleFormatTest` and
  `BundleToolsTest` still run with no runtime. Only
  `hermesNodeBytecodeDump` links `hermesvm_a`, because the disassembler
  lives there. It does include `bundle_format.h` for `kBundleMagic` -- so a
  container pointed at it is named rather than reported as a bad bytecode
  magic -- which is a header of constants and costs no link dependency.

## Task 6: the flag matrix

`checkToolOptions()` in `tools/hermes-node/hermes-node.cpp` implements every
row of the design's "Flag surface" table. All of it sits after the parse
loop, next to the pre-existing `--optimize`/`--inspect` and
`--bundle`/`--inspect` refusals, so no rule depends on the order the flags
were typed in and `--dump --bundle=x` and `--bundle=x --dump` are the same
invocation. Every message names both flags involved.

Three things the matrix fixed rather than merely documented, all routed here
by earlier reviews:

- **`--dump-bytecode` was checked last in `runToolVerb()`**, so it silently
  lost to `--dump` and `--extract-module` and was accepted alongside every
  other flag. Any two verbs together are now an error, so the order of the
  branches in `runToolVerb()` carries no meaning at all -- at most one can
  be reached.
- **`--dump-bytecode=` with an empty value** reported
  `error: : No such file or directory`, a diagnostic with neither a filename
  nor a flag in it. It now says `--dump-bytecode requires a file path.`
  `--out=` had the same hole, reported from further away still
  (`error: failed to rename ... to : No such file or directory`), and gets
  the same treatment. `ToolOptions::out` became a `std::optional` to make it
  possible: an empty string could not distinguish "not given" from "given
  empty", and the matrix asks both questions.
- **A verb combined with `--inspect` was explained in terms of the
  debugger.** `--bundle=x --dump --inspect` hit the pre-existing
  `--bundle`/`--inspect` refusal and was told that bundled bytecode lacks
  debug info, which is true and irrelevant: `--dump` never runs the program.
  The verb check now comes first and says the actual reason -- the verb
  describes a file rather than running one, so there would be nothing to
  inspect.

`--verbose` is now refused outside its three consumers rather than accepted
and ignored, which is what the design's table asks for.

`--out` was left generic, with no verb prefix. Only one verb writes a file,
and renaming a shipped flag to pre-empt a hypothetical second one is not
worth it (ruled during Task 6 dispatch).

`test/bundle-tool-errors.js` has one case per table row plus the two
malformed-value cases, the two-verb case in both flag orders, and a
positive control that a valid `--bundle --dump` still works. Every case
asserts its own diagnostic text: a check matching a bare `error:` passes
when the binary fails for an unrelated reason, which is a failure mode this
branch hit repeatedly.

## Documentation

- `README.md` gains a "Looking inside a bundle" subsection under "AOT
  bundles" with the four commands, and its copy of the `--help` output was
  stale again -- it predated `--verbose` and all three verbs. It was
  re-synced by diffing against the binary's actual output, not against the
  plan text, and now matches it exactly.
- `CLAUDE.md` gains a "Bundle tooling" subsection under "AOT Bundles": the
  flags, the pre-runtime dispatch, `checkToolOptions()`, inspection mode,
  and why the two new library targets are separate. Its bundle test list
  also gained `bundle-require.js`, which had been missing since the AOT
  work.

The non-obvious property both documents state plainly: `--dump` and
`--extract-module` read a container the running binary would refuse to
execute. A generation mismatch is printed as a `MISMATCH` line rather than
enforced, since a container built by another version is exactly the one
worth looking at. Structural validation is unchanged in inspection mode and
a format-version mismatch stays fatal -- the tables are laid out by the
format version, and a reader guessing at a different layout would print
confident nonsense.

## Final whole-branch review

The last gate found one destructive bug and one shipped-narrower-than-designed
feature; both are fixed, plus four minors.

- **Extracting onto the container destroyed it, silently.** `--extract-module`
  never compared `--out` with `--bundle`, and the write is a rename, so
  `--out=app.hbb` replaced the container with one module's payload, exited 0,
  and said nothing -- the reader was holding the old inode through its
  mapping, so nothing downstream noticed. `extractModule()` now refuses when
  `--out` and the bundle are the same file, compared by `(st_dev, st_ino)` so
  that `./app.hbb`, a path through a symlinked directory, a symlink to the
  container, and a second hard link are all caught. Refused before anything
  is read or written.
- **`--verbose`'s configuration and summary blocks were a subset of the
  design's.** Now implemented: `output:` is the absolute path (was `out:`,
  raw as given); the generation tag carries the version, architecture,
  bytecode version and optimization level folded into it
  (`bundleGenerationDescription()`, built beside the tag from the same four
  fields so the two cannot drift); each compile line carries the
  bytecode-to-source ratio; and the summary reports modules by kind, distinct
  specifiers, string table entries and bytes, payload bytes, the largest
  single module, and the total file size. `summary()` moved after
  `serialize()` -- three of those lines describe the laid-out container --
  and reads its section sizes back out of the finished container so this
  summary and a later `--dump` cannot disagree.
- Labels in the configuration and summary blocks are padded to a fixed
  column, as the design's samples show. The discovery block is flat instead:
  `require`, `known` and `skip` start in column 0 like `discover`, with only
  their verbs padded to a common width. The design's samples indent them,
  and that indent would be a lie -- a require is reported before the module
  it discovers (pass A resolves and skips, pass B reports and discovers), so
  every require line after a file's first would sit under a `discover` line
  belonging to some other file's target. The flat form claims no hierarchy,
  which is the only true thing available without buffering the walk. Nor are
  the `->` columns aligned to each other or indices zero-padded to the final
  module count, for the same reason: the point of streaming the walk is that
  a long build says what it is doing while it does it.
- `BundleReader::payloadOffset()` was dead -- it existed only because the
  plan's interface list named it -- and is deleted.
- A malformed container reported from `--dump`/`--extract-module` now names
  the file. The reader's own messages never name it (one reader also serves
  the run path, where the container is the program), which made the same
  class of failure name the file or not depending on which layer caught it:
  `MappedFile`'s errors and every `--dump-bytecode` message already did.
- Two coverage gaps closed: `test/bundle-dump-bytecode.js` now exercises all
  four cache-header rejection reasons (the zero-bytecode and
  shorter-than-the-header cases were untested), and
  `test/bundle-tool-no-runtime.js` pins the "no runtime is booted" invariant
  by a proxy -- a verb given `--compile-cache=<dir>` never creates the
  directory, while running the same container does. Only a one-off gdb
  observation covered that before.

## Concerns

- The identity vocabulary differs between the discovery lines and the
  compile lines, so an identity read off a `--verbose` build cannot be
  pasted straight into `--extract-module`. `discover` and `require` print
  absolute paths; `compile`, `largest:` and the container itself use
  root-relative identities. This is forced by ordering: the root is
  `commonAncestor(paths)` and is not known until the walk has finished, so
  the discovery lines cannot be expressed in terms of it. Fixing it would
  mean buffering the whole walk and losing streaming output on long builds,
  which is the property the narration exists for. Recorded rather than
  fixed. `--dump` prints the identities `--extract-module` wants.
- `--extract-module=` with an empty value is still answered by the
  extraction code as `no module '' in <file>` rather than by the flag
  matrix. It names the miss and exits non-zero, so it is not the silent
  hole `--dump-bytecode=` and `--out=` were, but it is the one malformed
  value of the three that does not name the flag that carried it.
- A positional script argument alongside a verb is still ignored silently:
  `hermes-node --dump-bytecode=f cli.js` disassembles `f` and never
  mentions `cli.js`. It is the same shape as the `--verbose` rule (a flag
  promising something that will never happen), but the design's table does
  not list it and no rule was invented for it here.
- Nothing in the tooling reads the environment, by design, so there is no
  way to ask for a dump from a program that is already running. That is
  what the design intends, but it does mean a bundle that fails at run time
  has to be inspected as a second, separate invocation.
