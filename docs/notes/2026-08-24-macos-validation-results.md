# macOS: making --build-exe work -- results

Answers the handoff in `2026-08-24-macos-validation-handoff.md`.

## Verdict

**It works, and `--build-exe` itself needed no changes at all.** All ten checks
pass. `--build-exe` produced a working standalone executable on the first
attempt, it still ran with the container and the whole source tree deleted, and
`codesign` reports `flags=0x20002(adhoc,linker-signed)` with
`codesign --verify` answering `valid on disk / satisfies its Designated
Requirement` -- area A confirmed on a real artifact, not on a relinked
`hermes-node`. `make-kit.py`'s `libtool -static` branch, which had never
executed anywhere, merged 40 archives correctly on its first run and preserved
`MH_SUBSECTIONS_VIA_SYMBOLS` on every member that had it (area B). The
universal build works and `check-hermes-node` on the two-slice tree -- exactly
what `release.yml` runs -- **exits 0** (area C). Both trees now report the
Linux numbers exactly: 285 unit tests, 180 JS tests, 0 unsupported, 0 failures.

Eight commits, **all of them tests or test infrastructure, none of them
product code**. Two were genuine macOS portability bugs in the feature's own
tests, one was a wrong assertion about `--build-exe` output on a universal
binary that would have failed every macOS release, and five were pre-existing
problems in unrelated tests that this machine happened to expose -- including
a race that had been failing roughly half of local runs, and a watcher test
that **hung the release gate for 33 minutes** instead of failing.

One product-level bug was found along the way: an uncaught exception thrown
from a timer, immediate or tick callback exited **0** and never reached
`process.on('uncaughtException')`, so an asynchronously-failing test looked
green. It is the reason the watcher bug presented as a hang. Found during this
work and fixed afterwards, on request; the fix sits at the bottom of the
branch, ahead of every macOS commit and depending on none of them -- see "The
timer callback fix" at the end. Nothing about it is macOS-specific; the behaviour was identical on Linux,
which is why that section carries the longest `LINUX-AFFECTING` note in this
report. Its I/O half (a throw in an `fs` or `net` callback) is still
outstanding and deliberately so.

The one thing worth deciding before shipping: **on a universal binary `ld64`
signs the arm64 slice and leaves the x86_64 slice unsigned.** That is the
toolchain's behaviour and not ours -- CMake's own universal `hermes-node` and a
two-line `int main(){}` do exactly the same -- but it means "the linker signs
it for us" is an arm64-only benefit, and a universal artifact still needs a
signing step if the whole file is ever meant to verify. Details under C9.

## Environment

| | |
|---|---|
| arch | `arm64` (Apple Silicon) |
| macOS | 26.6.1 |
| clang | Apple clang version 21.0.0 (clang-2100.0.123.102) |
| ld | `@(#)PROGRAM:ld PROJECT:ld-1266.8` |
| default build | **single-arch arm64** (`cmake-build-release`); a separate `cmake-build-universal` was configured for C9 |
| submodule | clean, and matching the recorded gitlink `9aaccbe5`. The divergence the handoff warned about did not recur, and no `git submodule` command was run. |

## What I changed

Eight commits, listed below, all of them `testfix:`. Nothing under
`lib/build-exe/`, `utils/make-kit.py`, `tools/hermes-node/bundle_main.cpp` or
`lib/bundle/` was touched -- the feature under validation is byte-for-byte as
it arrived.

The timer fix that came out of this work is **not** among them. It is three
commits -- two `bugfix:` and the CLAUDE.md section describing them -- and they
sit at the **bottom** of the branch, beneath every macOS commit, so they can
be taken on their own. See "The timer callback fix" at the end.

Commits are named here by subject rather than by hash: the branch has been
reordered once already and hashes do not survive that.

### 1. `testfix: macOS has no /bin/false`

`test/build-exe-errors.js` used `/bin/false` nine times as a stand-in for a
compiler that rejects its input. macOS has only `/usr/bin/false`, so
`posix_spawnp` reported "No such file or directory" where the test expected an
exit status. RUN lines now use a new `%false` substitution; the CHECK lines
cannot take substitutions -- FileCheck reads them literally -- so they match
the basename with a pattern.

> **LINUX-AFFECTING:** `test/lit.cfg` and `test/build-exe-errors.js` are
> shared. Re-verify `build-exe-errors.js` on Linux, where `%false` resolves to
> `/bin/false` and the CHECK patterns should match exactly what they matched
> before.

### 2. `testfix: re-sign before reading a patched payload`

`build-exe.js` patches a byte inside the finished executable to corrupt the
embedded payload, then expects the program to report
`not a hermes-node bundle (bad magic)`. On Apple Silicon it got `Killed: 9`:
patching invalidates the ad-hoc signature `ld64` applied, and the kernel
SIGKILLs a Mach-O whose pages no longer match its CodeDirectory, before
`main()` runs. A new `%resign` substitution restores the signature
(`codesign -f -s -` on Darwin, `true` elsewhere).

Nothing was wrong with the product: re-signing by hand produced the expected
error verbatim, which is the evidence the embedded run path is correct here.
Worth recording rather than only working around -- **on macOS a corrupted
payload is caught by the system before it reaches us**, and our magic check is
the second line of defence.

> **LINUX-AFFECTING:** `test/lit.cfg` and `test/build-exe.js` are shared.
> `%resign` expands to `true ` off Darwin so the added RUN line is a no-op;
> re-verify `build-exe.js` still passes.

### 3. `testfix: chdir reports the resolved path`

`NodeProcessTest.ChdirWorks` compared `process.cwd()` after `chdir('/tmp')`
against the literal `"/tmp"`. `/tmp` is a symlink to `/private/tmp` here and
`getcwd()` resolves symlinks, so it returned `/private/tmp`. Now compares
against `std::filesystem::canonical("/tmp")`.

> **LINUX-AFFECTING:** `unittests/NodeProcessTest.cpp` is shared, but
> `canonical("/tmp")` is the identity where `/tmp` is a real directory, so
> Linux should be unchanged. Re-verify `NodeProcessTest.ChdirWorks`.

### 4. `testfix: PIPE takes the shorter of its two spellings`

`test-net-server-listen-path.js` failed with `EADDRINUSE`. The cause was
length, not occupancy: `common.PIPE` is built with `path.relative`, which -- when
the working directory is far from the repo -- walks to the root and back and
comes out **longer** than the absolute path it exists to shorten. Measured 105
bytes against macOS's 104-byte `sun_path`. `bind()` truncated, both socket
names collapsed onto the same string, and the collision surfaced as
`EADDRINUSE`. Now takes whichever spelling is shorter.

> **LINUX-AFFECTING:** `test/node-tests/common/index.js` is shared. Where the
> relative spelling is shorter -- the ordinary case -- this picks it and `PIPE`
> is byte-identical to before. Re-verify `test-net-server-listen-path.js` and
> `test-net-pipe-connect-errors.js`.

### 5. `testfix: accept the c-ares shapes macOS actually produces`

macOS states in `/etc/resolv.conf` itself that the file "is not consulted for
DNS hostname resolution". c-ares reads exactly that file, so every query in
`test-dns-resolve.js` fails with `ECONNREFUSED` where it succeeds on Linux.

That exposed a real mismatch. `node_cares_wrap.cpp` follows Node in passing a
c-ares error code **string** on failure and `0` on success -- deliberately, so
the JS layer raises a `DNSException` rather than a `UVException` -- but the
test asserted `typeof status === 'number'`, which only holds on the success
path Linux always takes. **The binding is right and the assertion was wrong.**
The acceptable-error list also gained `ECONNREFUSED`.

> **LINUX-AFFECTING:** `test/test-dns-resolve.js` is shared. Both changes
> widen what is accepted and reject nothing that passed before.

### 6. `testfix: a universal binary carries one payload per slice`

**This is the one that would have failed every macOS release.**
`build-exe.js` asserted the payload marker appears exactly once in the
executable. A universal Mach-O carries a complete payload in *each* slice, so
the count is 2 for the `x86_64;arm64` build `release.yml` produces, and
`check-hermes-node` on the universal tree failed while everything it was
testing worked correctly. The count now comes from `lipo -archs`, falling back
to 1 where lipo does not exist, and every occurrence is patched so whichever
slice runs finds its own payload corrupted.

Also strips both counts before comparing: macOS `wc` pads its output where
Linux `wc` does not, and unquoted word splitting had been hiding that.

> **LINUX-AFFECTING:** `test/build-exe.js` is shared. Where lipo does not
> exist the count falls back to 1 and the patch loop runs once, which is what
> the single line did before. Re-verify on Linux -- the shell here is longer
> than what it replaced and it is the part I cannot run.

### 7. `testfix: fsp.access stops racing fsp.chmod for a file`

`test-fs-async-verify.js` Test 38 read the file Test 36 creates. Nothing
orders the two async chains, so whenever `access()` got there first the test
failed with `ENOENT` on a file that was about to exist. It was the only
filename in that file shared between two tests. Twelve consecutive passes
after the change, against five failures in six runs before it.

> **LINUX-AFFECTING:** `test/test-fs-async-verify.js` is shared, and the race
> was never macOS-specific -- only the scheduling that exposed it. Linux was
> likelier winning the race than immune to it.

### 8. `testfix: an unclosed FSEvent hung the run, not failed it`

**Found only after everything else was green**, by running the gate with its
real parallel runner instead of `-j1`. `check-hermes-node` on the universal
tree sat at 90% for 33 minutes with no output; the stuck process was
`test-fs-event-wrap.js`.

Tests 9 and 10 closed the watcher only from `onchange`. A started FSEvent is
ref'd, so when the event did not arrive the handle held the event loop open
and the process waited for ever. The trigger is macOS coalescing FSEvents with
a latency: standalone the event lands well inside the old fixed 500 ms window,
but under the load of a real suite run it does not -- measured, **4 of 16
concurrent copies hung**. Both tests now close from whichever path arrives
first and poll for the event against a 10 s deadline, so a genuinely missed
event fails instead of hanging, and a normal run finishes as soon as the event
lands rather than always paying 500 ms.

32 consecutive passes across two rounds of 16 concurrent copies, and three
full parallel suite rounds at 5-7 s each, against the 33-minute hang.

> **LINUX-AFFECTING:** `test/test-fs-event-wrap.js` is shared. Nothing here is
> macOS-specific except the latency that exposed it -- inotify is prompt, so
> Linux was very likely always inside the old window. Re-verify the test
> passes, ideally under a parallel run.

## C1 -- the kit cuts

All five files present:

```
hermes-node-bundle-main.o           4976
kit.manifest                         917
kit.stamp                              0
libhermes-node-kit.a            27746160
libhermesNapi.a                   225976
```

`make-kit: 40 archives merged`, 977 archive members. The manifest, verbatim:

```
# hermes-node kit manifest -- generated by utils/make-kit.py, do not edit
version: 0.0.1-135-g2aea1d8
cc: /usr/bin/clang++
driverflag: -Wall
driverflag: -Wextra
driverflag: -Wno-unused-parameter
driverflag: -Wwrite-strings
driverflag: -Wcast-qual
driverflag: -Wno-invalid-offsetof
driverflag: -Wmissing-field-initializers
driverflag: -Wno-deprecated-copy
driverflag: -Wno-noexcept-type
driverflag: -Wnon-virtual-dtor
driverflag: -Wdelete-non-virtual-dtor
driverflag: -ffp-contract=on
driverflag: -Wno-range-loop-analysis
driverflag: -O3
driverflag: -DNDEBUG
driverflag: -arch
driverflag: arm64
driverflag: -Wl,-search_paths_first
driverflag: -Wl,-headerpad_max_install_names
driverflag: -Wl,-export_dynamic
driverflag: -Wl,-dead_strip
linkarg: -force_load
linkarg: {kit}/libhermesNapi.a
linkarg: {kit}/libhermes-node-kit.a
linkarg: -lresolv
linkarg: -lpthread
linkarg: -lm
linkarg: -framework
linkarg: CoreFoundation
```

The `linkarg` lines do end with the macOS system libraries, `-lresolv`
included -- the flag the spike's hand-written list had missing, which is the
manifest earning its keep.

**Atom granularity survived the merge.** Rather than spot-checking one member
I walked all 977: **973 of the 976 object members carry
`MH_SUBSECTIONS_VIA_SYMBOLS` (`0x2000`)**. The three that do not are
boost::context's hand-written assembly -- `make_apple.S.o`, `jump_apple.S.o`,
`ontop_apple.S.o` -- and they do not carry it in the original
`libboost_context.a` either (checked: `flags 0x00000000` there too). So the
merge changed nothing; it is faithful, not lossy. `libtool -static` behaved as
the design assumed on its first execution anywhere.

## C2 -- an executable builds and runs

```
wrote myapp (10793128 bytes)
$ ./myapp one two
PASS 42 one,two
$ rm app.hbb app.js && ./myapp three
PASS 42 three
```

The second run is the feature and it works: the container is not read at run
time. No `.hnexe.*` temporaries were left behind. At 10.79 MB it is smaller
than the ~12.7 MB Linux figure, which is the folding and dead-stripping the
archive kit exists to preserve.

## C3 -- the signature

```
$ codesign -dvvv ./myapp
Executable=/private/tmp/mv/myapp
Identifier=myapp
Format=Mach-O thin (arm64)
CodeDirectory v=20400 size=83774 flags=0x20002(adhoc,linker-signed) hashes=2615+0 location=embedded
Hash type=sha256 size=32
CandidateCDHash sha256=064aac0766733dabef6cc3f5b3ac83356da87539
CandidateCDHashFull sha256=064aac0766733dabef6cc3f5b3ac83356da87539db8a148e799376466fb410fe
Hash choices=sha256
CMSDigest=064aac0766733dabef6cc3f5b3ac83356da87539db8a148e799376466fb410fe
CMSDigestType=2
CDHash=064aac0766733dabef6cc3f5b3ac83356da87539
Signature=adhoc
Info.plist=not bound
TeamIdentifier=not set
Sealed Resources=none
Internal requirements=none

$ codesign --verify --verbose ./myapp
./myapp: valid on disk
./myapp: satisfies its Designated Requirement
```

Control (`cmake-build-release/bin/hermes-node`): same
`flags=0x20002(adhoc,linker-signed)`, same `Signature=adhoc`, also valid.

**Area A is settled on a real artifact.** No signing step of ours, and no
`codesign -s -` was needed.

## C4 -- the payload object is Mach-O shaped

`--verbose` printed exactly:

```
	.section __DATA,__const
	.p2align 4
	.globl _hermesNodeBundleStart
_hermesNodeBundleStart:
	.incbin "/private/tmp/mv/app.hbb"
	.globl _hermesNodeBundleEnd
_hermesNodeBundleEnd:
```

Mach-O section, leading underscores, and **zero** occurrences of
`.note.GNU-stack`. Alignment:

```
_hermesNodeBundleStart  0x10085d640  16-byte aligned: True
_hermesNodeBundleEnd    0x10085d898
```

`End - Start` = `0x258` = 600 bytes = the container's size exactly.

## C5 -- unit tests, both object formats

`BuildExeTest`: **11 tests, 11 passed**, including both `PayloadAssemblyElf`
and `PayloadAssemblyMachO`, plus
`PayloadAssemblyAlignsAndBracketsTheBytesInBothFormats` and
`PayloadAssemblyDefaultsToTheHostFormat`. Replacing the `#ifdef` with a
parameter paid off exactly as intended: the ELF branch is the foreign one here
and it is still asserted.

## C6 -- the full suite

Both suites, both trees, after the fixes:

| | unit | JS | unsupported | failures |
|---|---|---|---|---|
| `cmake-build-release` | 285 | 180 | 0 | **0** |
| `cmake-build-universal` | 285 | 180 | 0 | **0** |

`cmake --build <tree> --target check-hermes-node` **exits 0** on both, run at
`HEAD` with the target's own 16-thread runner (not `-j1`), after the kit
re-cut itself for the new version. This matches the handoff's Linux numbers
exactly (285 unit, 180 JS, 0 unsupported). Three further parallel rounds of
the JS suite on the universal tree: 180 passes each, 5-7 s each.

**Run the gate the way CI runs it.** Most of my iteration used `-j1`, which is
deterministic and easy to read -- and which hid the worst bug in this report
entirely. `test-fs-event-wrap.js` only hangs under parallel load, so the suite
looked green for hours before the real runner found it. See change 8.

The three `REQUIRES: linker-available` tests -- `build-exe.js`,
`build-exe-escapes.js` and `build-exe-natives.js` -- did **not** report
UNSUPPORTED. All three ran and passed, so the `kit_dir` param took and the
numbers above mean what they say. (`build-exe-errors.js` is not among the
three: it drives most of its cases through a hand-written fake kit and needs
no linker.) The single
`Unsupported` seen in early runs was `bundle-yargs.js` awaiting
`examples-installed`; it disappeared once C10's `npm install` had run.

Before the fixes the first honest run of this suite was: 180 tests, 4 failures
(`build-exe.js`, `build-exe-errors.js`, `test-net-server-listen-path.js`,
`test-dns-resolve.js`) plus the intermittent `test-fs-async-verify.js`, and one
unit failure (`NodeProcessTest.ChdirWorks`).

## C7 -- a native addon beside the executable

Build printed one `native:` line and two `note:` lines, as specified:

```
wrote dist/app (10793120 bytes)
native: hello_addon.node (from hello_addon.node)
note: this executable requires 1 native addon alongside it; ship them together.
note: they must sit beside dist/app, not beside the container.
```

With the sidecar beside the executable: `ADDON world`, exit 0. With it moved
away:

```
Error: Cannot find module 'hello_addon.node'
  This bundle records a native addon, but its file is not beside the bundle.
  Expected: /private/tmp/c7/dist/hello_addon.node
  Native addons ship alongside the container; copy it there.
    at missingSidecar (libjs/bundle-loader.js:233:26)
```

`MODULE_NOT_FOUND` naming the file to ship, **not** `ERR_DLOPEN_FAILED`, which
is the branch an optional-dependency probe needs.

## C8 -- dependencies

```
$ otool -L /tmp/c7/dist/app
	/usr/lib/libresolv.9.dylib
	/usr/lib/libSystem.B.dylib
	/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation
	/usr/lib/libc++.1.dylib
```

**Identical** to `hermes-node`'s, diffed. Four OS-provided libraries, no ICU,
as the spike predicted. 145 `_napi_` dynamic exports on both. The produced
executable is 135,608 bytes smaller than `hermes-node` -- the CLI parser and
tool verbs dead-stripped, the same effect the Linux side measured at 154,032.

## C9 -- the universal build

**`make-kit.py` printed its multi-arch warning** and exited 0, as designed:

```
make-kit: WARNING: this link line names 2 architectures (arm64, x86_64).
make-kit: A universal kit is UNVERIFIED: no two-slice kit has ever been cut
...
make-kit: 40 archives merged into .../cmake-build-universal/kit/libhermes-node-kit.a
```

**The kit is genuinely fat, and the documented empty-slice trap did not
occur.** Every kit file is two-slice, and both slices are real:

| | x86_64 | arm64 |
|---|---|---|
| `libhermes-node-kit.a` slice size | 29,831,464 | 27,541,816 |
| symbols in that slice | 66,298 | 97,507 |
| `libhermesNapi.a` symbols | 841 | 1,379 |
| `hermes-node-bundle-main.o` symbols | 29 | 36 |

Nothing resembling the 216-byte zero-symbol slice the warning describes.

**`--build-exe` produces a working universal executable.** 22,338,520 bytes,
`x86_64 arm64`, slices of 11,601,984 and 10,705,880. The arm64 slice runs
natively (`UNIVERSAL PASS 42 arm64`) and the x86_64 slice runs under Rosetta
(`arch -x86_64 ./uapp` -> `UNIVERSAL PASS 42 x64`).

**Both slices are NOT signed, and that is `ld64`, not us:**

```
arm64    flags=0x20002(adhoc,linker-signed)   valid on disk
x86_64   code object is not signed at all
```

Before concluding anything I checked the controls, as section 7 asks:

| built by | arm64 slice | x86_64 slice |
|---|---|---|
| our `--build-exe` | `adhoc,linker-signed` | not signed |
| CMake's own universal `hermes-node` | `adhoc,linker-signed` | not signed |
| `clang -arch arm64 -arch x86_64` over `int main(void){return 0;}` | `adhoc,linker-signed` | not signed |
| `clang -arch x86_64` alone | -- | not signed |

So no flag of ours is suppressing anything: **`ld64` ad-hoc signs arm64 output
and does not sign x86_64 output.** Apple made ad-hoc signing mandatory for
arm64 and never did for x86_64. Nothing is broken -- the x86_64 slice runs
fine, because macOS only enforces signatures on native arm64 code -- but see
"Still broken / not done" for what it means for a release.

**The release gate passes.** `cmake --build cmake-build-universal --target
check-hermes-node`, which is exactly what `release.yml` runs, **exits 0**: 285
unit tests, 180 JS tests, no failures. It did not before commit `4a7975b`; that
commit is the difference between a macOS release being cuttable and not.

## C10 -- an example, end to end

Both examples installed, bundled, linked and ran.

| | container | executable | signature |
|---|---|---|---|
| `yargs-cli` | 272.1 KB | 11,073,864 | `adhoc,linker-signed` |
| `hermes-parser-ast` | 234.7 KB + 2.0 MB sidecar | 11,040,832 | `adhoc,linker-signed` |

`./yargs-cli/dist/greet hello --name World` -> `Hello, World.`, and `--help`
prints the command table.

`hermes-parser-ast` is the interesting one -- a real native addon reached only
through computed requires. The build printed its one `native:` line and two
`note:` lines, and the produced executable parsed a real file:

```
$ ./hermes-parser-ast/dist/ast /tmp/parseme.js > /tmp/ast-exe.json
$ hermes-node hermes-parser-ast/ast.js /tmp/parseme.js > /tmp/ast-plain.json
$ cmp /tmp/ast-exe.json /tmp/ast-plain.json
BYTE-IDENTICAL to the unbundled run
```

## Still broken / not done

0. ~~**An uncaught exception in a timer callback exits 0.**~~ **FIXED** after
   this report was first written, on request -- see "The timer callback fix"
   at the end. The description below is what was found; the scheduling half
   (timers, immediates, ticks) now matches Node, and the I/O half does not
   yet.

   ```
   $ cat throwtimer.js
   setTimeout(function () { throw new Error('BOOM'); }, 10);

   $ hermes-node throwtimer.js        $ node throwtimer.js
   Error: BOOM                        ...
       at anonymous (...)             throw new Error('BOOM');
       at listOnTimeout (...)               ^
   exit=0                             exit=1
   ```

   The error is printed and the status is success. The two neighbouring cases
   are both correct -- a synchronous top-level `throw` exits 1, and an
   unhandled promise rejection exits 1 -- so it is specifically the
   timer/async-callback path that reports the failure and then does not act on
   it.

   Why it matters beyond tidiness: **a test that fails asynchronously exits
   0 and looks green.** This suite is insulated because its tests are
   `FileCheck`ed for a `PASS` line that a thrown assertion never prints, but
   anything checking only exit status is not. It is the same class of problem
   as the `process.exitCode` bug `CLAUDE.md` records ("a failing run looked
   green"), and it is what turned change 8's missing `close()` into a silent
   33-minute hang rather than an assertion failure.

   Not fixed from here deliberately: this is core, shared, cross-platform
   behaviour with nothing macOS about it, and section 6 of the handoff is
   explicit that a change I cannot reason about on Linux is one to stop and
   ask about rather than make.

1. **The x86_64 slice of a universal executable is unsigned.** Established
   above as `ld64`'s behaviour, reproduced on CMake's own binary and on a
   two-line C program, so it is not a `--build-exe` defect and it blocks
   nothing today. But it qualifies the design's headline claim: **"the linker
   signs it for us" is true for arm64 and false for x86_64.** If a macOS
   release artifact is ever meant to pass `codesign --verify` as a whole file,
   or to be Developer-ID signed and notarized, it needs an explicit signing
   step for the fat binary. Worth deciding deliberately rather than
   discovering at notarization. I did not add one: the handoff is explicit
   that reaching for `codesign` before understanding the cause is the wrong
   move, and having understood the cause, adding a signing step is a design
   decision rather than a fix.

2. **The x86_64 slice was only exercised under Rosetta**, on this Apple
   Silicon machine. No real Intel hardware was involved. It runs and prints
   the right answer, but "works on Intel" is not something this session can
   claim.

3. **`clang-format` version drift.** The local `clang-format 21.1.2` wants to
   reflow parts of `unittests/NodeProcessTest.cpp` that I did not touch,
   which suggests the Linux side formats with a different version. I committed
   only my own hunk (verified clang-format-clean in isolation) and left the
   churn out, since committing it would only invite Linux's `format.sh` to
   flip it back. Somebody should pin the version.

4. **`common.PIPE` diverges from upstream Node and I did not fix it.**
   Upstream joins the directory and the socket name with `path.join`; this
   port concatenates them, so the socket is a *sibling* of the tmpdir rather
   than a file inside it, and `tmpdir.refresh()` never cleans it up. Fixing
   that moves the socket on Linux too, it was not needed to make the test
   pass, and it is not a change to make from a machine that cannot run the
   Linux suite. Flagged for whoever can.

5. **`--verify-natives` and the tool verbs were exercised only through the
   suite**, not driven by hand against a produced executable. The suite covers
   them and passes; I did not add anything beyond it.

## Anything that surprised you

- **How little was wrong.** Every one of the three risk areas came out clean
  on first contact: the signature, the never-executed `libtool` branch, and
  the universal kit. The `-arch` forwarding fix that the whole-branch review
  made blind, without a Mac, is correct -- the assemble step produced a fat
  payload object in one invocation and no per-arch fallback was needed.

- **The tamper test was measuring the OS.** `Killed: 9` where a diagnostic was
  expected looks like the feature failing, and is the opposite: the ad-hoc
  signature `ld64` gives us is real enough that the kernel enforces it. The
  same property that makes linking better than injecting is what broke the
  test.

- **Five of the eight fixes had nothing to do with `--build-exe`.** `/tmp`
  symlinks, `sun_path` length, c-ares on a platform that does not use
  `resolv.conf`, a test race, and a watcher hang -- all pre-existing, all
  invisible on Linux. A first port to a new platform mostly finds assumptions,
  not bugs.

- **`-j1` hid the worst bug for hours.** I ran almost everything serially
  because it is easier to read, and reported the suite green on that basis
  more than once. `test-fs-event-wrap.js` hangs only under parallel load, so
  the first honest look at what CI actually runs found a 33-minute hang in a
  suite I had already called clean. Serial runs are not a cheap approximation
  of the real gate.

- **A hang, not a failure, is what a missing `close()` buys you.** The test
  had a perfectly good assertion waiting to fire; it never got the chance,
  because the ref'd handle kept the loop alive and, per finding 0, the
  exception would not have set the exit status anyway. Two independent
  weaknesses had to line up, and both are cheap to fix.

- **`wc` pads its output on macOS and not on Linux**, and unquoted word
  splitting had been concealing that in a shell comparison. My first attempt
  at the universal payload-count fix failed for precisely this reason.

- **The relative path that is longer than the absolute one.** `common.PIPE`
  exists to keep socket paths short and was making this one 105 bytes against
  a 104-byte limit. The failure then presented as `EADDRINUSE`, which names
  the wrong problem entirely.

## Anything you would change but did not, and why

- **A `codesign -s -` step for universal output.** It is one line and it would
  make the fat binary verify. I did not add it because it is a design
  decision, not a repair -- see "Still broken" 1 -- and because the person who
  owns the "we link rather than inject" argument should be the one to decide
  how much of it survives contact with x86_64.

- **Teaching `make-kit.py` to check its own slices.** Its warning tells the
  reader to verify each slice by symbol count; the script could simply do
  that itself and fail, turning an unverified kit into an impossible one. I
  left it alone: the warning's own docstring argues deliberately for warning
  over failing, since the release build is the only thing that cuts a
  universal kit and failing there would block releases to report something
  that -- as this session establishes -- is fine.

- **Reformatting `NodeProcessTest.cpp`.** See "Still broken" 3.

- **Fixing `common.PIPE`'s missing separator.** See "Still broken" 4.

- **Asserting the SIGKILL in `build-exe.js`.** Tempting, since it is a real
  and pleasant security property, but it tests Apple's kernel rather than our
  code. It is documented in the test's comment instead.

- **Making an uncaught exception in a timer exit 1.** See "Still broken" 0.
  It is a few lines in shared code and it is the right behaviour, but it
  changes what every asynchronously-throwing program on every platform does
  with its exit status, and some test somewhere is probably relying on the
  current answer without knowing it. That is a change to make with the Linux
  suite in reach.

- **Adding a lit timeout.** `hermes-lit` will wait for ever, which is how a
  single hung test cost 33 minutes and would cost a CI job its whole budget.
  A per-test timeout would have turned that into a clear failure in seconds.
  Out of scope here, and it is a suite-wide policy decision rather than a
  macOS fix, but it is the change that would most improve the next debugging
  session.

## The timer callback fix

Found during the validation above and fixed afterwards, on request. It is the
one product change in this branch, and it is deliberately **independent of
the macOS work**: the two `bugfix:` commits and the CLAUDE.md section that
documents them are the first three on the branch, they touch no file any
macOS commit touches, and nothing above them is needed to build or test them.
They can be taken on their own.

### What was actually wrong

Wider than reported. The report described the exit status; two more symptoms
shared the same line, and the bug covered four scheduling surfaces:

| thrown from | before: exit | before: listener runs | node |
|---|---|---|---|
| `setTimeout` | 0 | no | 1 |
| `setInterval` | 0 | no | 1 |
| `setImmediate` | 0 | no | 1 |
| `process.nextTick` | 0 | no | 1 |
| a rejected promise | 1 | -- | 1 |

So `process.on('uncaughtException')` never fired for any of them: the native
callback printed the error itself and returned, and JavaScript was never
asked. And because timers all run off **one** libuv handle, the early return
skipped the code that re-arms it -- every timer still pending after a throw
silently stopped firing, which is why a handled exception lost the rest of the
program too.

### The shape of the fix

Node splits this deliberately: C++ asks `process._fatalException(err)` and
obeys the answer, because only JavaScript knows which listeners exist. That
split is reproduced rather than invented.

- `triggerUncaughtException()` in `lib/bindings/node_errors.{h,cpp}` -- one
  copy, mirroring `node::errors::TriggerUncaughtException`. It returns true
  when a listener took the error and **does not return** otherwise, so no
  caller carries an unhandled path of its own.
- `process._fatalException` did not exist here at all. Node installs it from
  `internal/bootstrap/node.js`, which this runtime does not run, so it is now
  built in `libjs/process-events.js` from what this runtime has. It was
  already being called by `internal/modules/run_main.js`, where it would have
  thrown `TypeError`.
- Three call sites converted: `onTimerFired` and `onCheckImmediate`
  (`node_timers.cpp`) and the tick drain (`hermes_node_runtime.cpp`, both
  places). The timer site also re-arms its handle on the handled path.

Every case now matches node v24.13.1, including a listener seeing the error,
the program continuing, and a later timer still firing.

### It immediately caught a real one

`test-repl-features.js` went red -- and it was **already failing**. Its Test 1
asserted a `'... '` continuation prompt from a `terminal:false` REPL session;
no such prompt is written, our output is byte-identical to Node's
(`"> | 3\n> "`), and running the whole file under `node` fails the same
assertion with the same message. It threw from inside a tick, so it had been
printed and discarded while the file went on to print `PASS`.

That is the bug demonstrating itself: the first thing the fix did was expose a
test that had been lying. Corrected in its own commit, before the fix, so
every commit is green.

### What is deliberately not covered

The same swallow-and-continue is in roughly ten I/O bindings. Measured:

```
fs.readFile callback throws   -> exit 0   (node: 1)
fs.stat callback throws       -> exit 0   (node: 1)
net listen callback throws    -> HANGS    (node: 1)
```

`triggerUncaughtException` is written to be their fix too and each site is a
few lines. They are left alone because resuming means something different for
each -- whether the stream keeps reading, whether the handle closes -- and
because that is a second change with its own testing burden, on a machine that
cannot run the Linux suite. The `net` hang in particular is worth its own
look.

### Verification

`check-hermes-node` exits 0 on both trees, single-arch and universal: 285 unit
tests, 181 JS tests (the 180 above plus
`test/test-uncaught-exception-async.js`, whose thirteen cases were each
checked against node v24.13.1).

Independence checked rather than asserted. With only the three bugfix commits
applied and no macOS work present, `test-uncaught-exception-async.js` and
`test-repl-features.js` both pass, and the rest of the suite fails exactly
what it fails at the branch point -- `build-exe.js`, `build-exe-errors.js`,
`test-net-server-listen-path.js` and `test-dns-resolve.js`, all macOS
environment issues that the `testfix:` commits above deal with, plus the
`test-fs-async-verify.js` flake (6 of 8 runs there, and roughly the same at
the branch point). Nothing the fix introduces, and nothing it needs.

The new test spawns thirteen subprocesses and asserts on their exit status,
which is the shape a flake likes, so it was run five times in isolation as
well: five passes. Worth repeating after any change near the event loop --
this is the file that would report such a change as a status regression.
