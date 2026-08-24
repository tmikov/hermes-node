# Make `--build-exe` work on macOS

- **Branch:** `work-mac`
- **Opened:** 2026-08-24
- **Audience:** a fresh Claude Code session on macOS, with no prior context
- **Deliverable:** the feature **working** on this machine, fixes committed,
  plus a report at `history/plans/2026-08-24-macos-validation-results.md`

This is development, not a survey. `hermes-node --build-exe` was built
entirely on Linux and **no line of it has ever run on macOS**. Your job is to
make it work here and commit the fixes -- not to report what is broken and
stop.

The person who wrote the feature has no Mac. That asymmetry shapes
everything below: you are the only one who can see these failures, and they
are the only one who can check that your fixes did not break Linux. Section 6
is about keeping that division honest, and it matters more than any single
task.

---

## 1. The feature, in one paragraph

`hermes-node --build-exe=<out> <bundle.hbb>` produces a standalone executable
from an AOT bytecode bundle. It does not inject a blob into a prebuilt binary
the way Node's SEA, Deno and Bun do -- it **links a new binary**: the
container becomes an object file via `.incbin`, and that object is linked
against a "kit" (a merged static archive plus `kit.manifest` recording
CMake's real link line, cut by `utils/make-kit.py` intercepting it). Read the
`## Single-File Executables` section of `CLAUDE.md` first; it is short and it
is the authority.

Prior work on this machine: `history/plans/2026-08-23-macos-partial-link-spike.md`
and its `-results.md`, beside this file. **Read the results file** -- it
established that partial-linking works here and that `ld64` ad-hoc signs, and
it is the baseline you are building on.

## 2. The three areas that carry the risk

**A. Does `ld64` ad-hoc sign a payload-carrying executable?** This is the
entire justification for linking rather than injecting. The spike checked a
relinked `hermes-node`; nobody has checked an artifact `--build-exe` actually
produced. Expected: `flags=0x20002(adhoc,linker-signed)`, `Signature=adhoc`,
and `codesign --verify` reporting `valid on disk`.

**B. `make-kit.py`'s `libtool -static` branch has never executed anywhere.**
The spike predates the script; every kit ever cut used the Linux `ar -M`
path. It must merge ~40 archives into one and leave
`MH_SUBSECTIONS_VIA_SYMBOLS` intact on every member -- that flag is why the
kit is an archive rather than an `ld -r` object, and losing it costs ~12% of
every produced binary.

**C. The universal build decides whether a macOS release can be cut at all.**
`.github/workflows/release.yml` configures `-DCMAKE_OSX_ARCHITECTURES=
"x86_64;arm64"` and then runs `check-hermes-node`, which depends on the kit
-- so **the release pipeline cuts a two-slice kit**. A whole-branch review
found this would have failed deterministically (the payload object was
assembled host-arch only) and fixed it by forwarding the kit's driver flags
to the assemble step. No Mac has seen that fix.

## 3. Setup

Use a **Release** build. ASAN distorts every size number and its kit is
~755 MB.

```bash
cd <repo root>
git branch --show-current          # expect: work-mac
cmake -B cmake-build-release -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build cmake-build-release --target hermes-node hermes-node-kit
cmake --build cmake-build-release --target FileCheck not hermes hello_addon.node
```

`hermes-node-kit` is `EXCLUDE_FROM_ALL` and must be named explicitly. The
four extra targets are what the test suite needs and the main target does not
pull in.

**The `hermes/` submodule.** Last time on this machine it was on branch
`n-api` at a commit that had **diverged** from the recorded gitlink and would
not compile (`hermes_napi_host::ref_loop` missing). If you hit that, **stop
and ask the user before running any `git submodule` command** -- the local
Hermes checkout is often intentionally ahead of the gitlink and updating it
destroys work. The previous session was given explicit permission before it
acted. This is one of the few hard stops.

## 4. The sequence

Work through section 5's checks in order. For each: run it, and **if it
fails, fix it and re-run**. When everything passes, re-run the whole sequence
from the top once more -- a later fix can invalidate an earlier pass.

Commit as you go, one commit per fix, rather than one commit at the end.

## 5. The checks

Linux numbers to compare against: **285 unit tests, 180 JS tests, 0
unsupported**; a produced executable ~12.7 MB; 145 `napi_` dynamic exports on
both `hermes-node` and a produced app.

**C1 -- the kit cuts.** `ls cmake-build-release/kit/` should hold
`libhermes-node-kit.a`, `libhermesNapi.a`, `hermes-node-bundle-main.o`,
`kit.manifest`, `kit.stamp`. Paste the manifest into the report verbatim; its
`linkarg` lines should end with the macOS system libraries. Then confirm the
merge preserved atom granularity: extract a member and check `otool -h` shows
`MH_SUBSECTIONS_VIA_SYMBOLS` (flag `0x2000`). If the members lost it, the
merge behaved like a link and the kit is wrong.

**C2 -- an executable builds and runs.**
```bash
cd /tmp && mkdir -p mv && cd mv
echo "console.log('PASS', 40 + 2, process.argv.slice(2).join(','));" > app.js
<repo>/cmake-build-release/bin/hermes-node --build-bundle=app.hbb app.js
<repo>/cmake-build-release/bin/hermes-node --build-exe=myapp \
    --kit=<repo>/cmake-build-release/kit app.hbb
./myapp one two
rm app.hbb app.js && ./myapp three     # MUST still work
```
The second run is the whole feature: the container is not read at run time.

**C3 -- the signature.** `codesign -dvvv ./myapp` and
`codesign --verify --verbose ./myapp`, plus the same on
`cmake-build-release/bin/hermes-node` as a control. Paste verbatim. If it
reports `code object is not signed at all`, that is a major finding -- see
section 7 before reaching for `codesign -s -`.

**C4 -- the payload object is Mach-O shaped.** `--build-exe ... --verbose`
prints the generated assembly; it must use `.section __DATA,__const`, leading
underscores on the symbols, and **no** `.note.GNU-stack` (ELF-only). Then
`nm myapp | grep -i hermesNodeBundle` -- the start symbol's address must be
16-byte aligned. `openEmbeddedBundle` enforces that and refuses to run
otherwise, so a misalignment surfaces as a startup error rather than
corruption.

**C5 -- unit tests, both object formats.** `BuildExeTest` has
`PayloadAssemblyElf` and `PayloadAssemblyMachO`. On Linux the Mach-O one is
the foreign case; here the ELF one is. Both must pass on both hosts -- that
was the point of replacing an `#ifdef` with a parameter.

**C6 -- the full suite.**
```bash
R=$(pwd)
python3 cmake-build-release/bin/hermes-lit -q $R/test \
  --param hermes_node=$R/cmake-build-release/bin/hermes-node \
  --param hermes=$R/cmake-build-release/bin/hermes \
  --param FileCheck=$R/cmake-build-release/bin/FileCheck \
  --param not=$R/cmake-build-release/bin/not \
  --param hello_addon=$R/cmake-build-release/hello_addon.node \
  --param kit_dir=$R/cmake-build-release/kit \
  --param source_dir=$R --param test_exec_root=/tmp/lit-macos
cmake --build cmake-build-release --target check-hermes-node
```
Three JS tests carry `REQUIRES: linker-available`. If they report
UNSUPPORTED the `kit_dir` param did not take, and every number below it is
meaningless -- say so rather than reporting a pass.

**C7 -- a native addon beside the executable.** Bundle a script that
`require`s `./hello_addon.node`, link it, copy the addon beside the
executable, run it. Then move the sidecar away: expect `MODULE_NOT_FOUND`
naming the file to ship, **not** `ERR_DLOPEN_FAILED`. The build should print
one `native:` line and two `note:` lines.

**C8 -- dependencies.** `otool -L` on the produced executable and on
`hermes-node`. The spike saw four OS-provided dylibs and no ICU; the two
lists should match, since a produced executable should need nothing
`hermes-node` does not.

**C9 -- the universal build.** Use a **separate** build directory so C1-C8
stay intact.
```bash
cmake -B cmake-build-universal -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
      -DCMAKE_OSX_ARCHITECTURES="x86_64;arm64"
cmake --build cmake-build-universal --target hermes-node hermes-node-kit
```
Then, in order: did `make-kit.py` print its multi-arch **warning** (it should;
a nonzero exit there would be a defect)? Does `libtool` produce a genuinely
**fat** archive -- `lipo -detailed_info`, and watch for the documented trap of
a slice that exists but is nearly empty with zero symbols while `lipo -info`
lists it happily? Does `--build-exe` produce a working universal executable,
with **both slices signed** (`lipo -thin <arch>` then `codesign -dvvv`)? And
finally the one that blocks releases:
```bash
cmake --build cmake-build-universal --target check-hermes-node
```
That is exactly what `release.yml` runs.

**C10 -- an example, end to end.** Once the above passes:
```bash
cmake --build cmake-build-release --target hermes-parser-napi
cd examples && ./yargs-cli/build-bundle.sh && ./hermes-parser-ast/build-bundle.sh
```
then `--build-exe` each `.hbb` into its own `dist/` and run it. The
`hermes-parser-ast` one exercises a real native addon through a computed
require. Report sizes and output. `examples/*/README.md` say what each does.

## 6. How to fix things -- read this before changing any code

**The Linux side must not regress, and you cannot test it.** That is the
central constraint. So:

- **Prefer a platform-conditional fix** over changing shared behaviour. If
  the Apple branch of something needs to differ, make it differ in the Apple
  branch.
- **When you must change shared code** -- `utils/make-kit.py`,
  `lib/build-exe/build_exe.cpp`, anything in `lib/bundle/` -- say so
  **prominently in the commit message**, in a line beginning
  `LINUX-AFFECTING:` followed by what a Linux maintainer should re-verify.
  Those lines are how the Linux side knows what to re-run. A shared change
  with no such line is worse than no fix.
- Re-run `cmake --build cmake-build-asan --target check-hermes-node` is not
  available to you in a useful sense (no ASAN Linux here); the Release suite
  in C6 is your gate.

**Conventions:**
- `./utils/format.sh -f` before every commit that touches C++.
- ASCII-only commit messages, no emojis. `Copyright (c) Tzvetan Mikov.` on
  any new file (**not** Meta Platforms).
- Never `git add hermes`, never `git submodule update`. **Never push.**
- Do not weaken a test to make something pass. If a test encodes
  Linux-specific behaviour that is genuinely wrong for macOS, change the
  test *and say so under `LINUX-AFFECTING:`*, with the reasoning.

**Decide these yourself, do not ask:** tool flags (`libtool` options,
`lipo` invocations), error-message wording, how to structure a
platform-conditional branch, which of two equivalent fixes to take, test
additions.

**Stop and ask about these:** anything touching the `hermes/` submodule;
anything that would change what a *bundle container* contains or how it is
laid out (that is a format question and Linux shares the format); adding a
new dependency; and any case where the only fix you can see would make Linux
behave differently in a way you cannot reason about confidently.

## 7. Failure modes worth anticipating

Not predictions of what will break -- places where a plausible failure has a
non-obvious right answer.

- **`libtool -static` over ~40 archives.** Likely wrinkles: warnings about
  archives with no symbols (`-no_warning_for_no_symbols` silences them), an
  argument list too long (libtool takes `-filelist`), or duplicate member
  names across archives. Duplicate *symbols* across members are expected and
  fine -- the merged archive is never `-force_load`ed, which is exactly why
  `libhermesNapi.a` is kept out of it. Do not "fix" that by merging it in.
- **Fat archive merging.** If `libtool` will not merge already-fat archives,
  the fallback shape is per-arch merge then `lipo -create`. Prefer whichever
  produces a genuinely fat archive with non-empty slices.
- **The assemble step with two `-arch` flags.** It forwards the kit's whole
  driver-flag list precisely so this works; if clang will not emit a fat `.o`
  in one invocation, per-arch assemble plus `lipo -create` is the fallback.
- **If `codesign` reports the executable unsigned**, do not immediately add a
  `codesign -s -` step. First establish *why* -- `ld64` signs arm64 output by
  default, so an unsigned result suggests a flag we pass is suppressing it,
  and finding that flag is a better fix than re-signing afterwards. If
  re-signing genuinely is the answer, that is a real finding and a design
  change: record it prominently, because it removes the main argument for
  linking over injecting.
- **Paths with spaces.** macOS build paths often have them. The producer
  rejects `"`, `\`, newline and carriage return in a container path
  deliberately (GAS resolves C escapes inside `.incbin`), but a space must
  work.

## 8. The report

`history/plans/2026-08-24-macos-validation-results.md`:

```markdown
# macOS: making --build-exe work -- results

## Verdict
<One paragraph, leading with the answer. Does it work on macOS now? Are
produced executables ad-hoc linker-signed? Does the universal build -- the
one release.yml does -- pass check-hermes-node?>

## Environment
arch / macOS / clang / ld / default single-arch or universal

## What I changed
<One entry per commit: what was broken, what you did, and whether it can
affect Linux. Repeat every LINUX-AFFECTING line here.>

## C1..C10
<Result per check, with the numbers and the verbatim output where asked.>

## Still broken / not done
## Anything that surprised you
## Anything you would change but did not, and why
```
