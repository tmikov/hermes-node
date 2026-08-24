# macOS partial-link spike -- results

Answers the spike in `2026-08-23-macos-partial-link-spike.md`.

## Verdict

**It works, and it answers the signing question the way we hoped.** The whole
44-input closure partial-links with `clang++ -r -nostdlib` in 0.15 s, and the
final link from that single object produces a binary that is functionally
indistinguishable from the one CMake builds: identical exported-symbol set
(2719 symbols, 145 of them `_napi_`), identical `otool -L` dependency set,
identical lit failure set, and a working `.node` addon `dlopen`. Crucially,
`ld64` stamps the result `CodeDirectory ... flags=0x20002(adhoc,linker-signed)`
and `codesign --verify` says `valid on disk / satisfies its Designated
Requirement` -- **the exact CodeDirectory sui hand-writes in Rust, for free**.
It runs on Apple Silicon with no `codesign -s -` step and no re-signing. I also
went one step past the task list and linked a real `.hbb` in as an `.incbin`
object: the payload-carrying binary is still `adhoc,linker-signed`, still
verifies, and still runs. **On macOS the entire injection-and-re-signing tax
that Node SEA, Deno and Bun all pay simply does not arise if we link instead of
inject.** The one caveat found is a footgun rather than a blocker, and it is in
T7: a missing architecture makes `ld -r` emit an *empty* slice and exit 0.

Two things qualify the recipe, both below. The `-r` binary is 3.9% larger than
CMake's because `ld -r` drops `MH_SUBSECTIONS_VIA_SYMBOLS` and the final link
can then neither fold identical code nor dead-strip; merging the archives with
`libtool -static` instead of partial-linking them keeps the flag and lands
within 8 bytes of the original, so **the kit should be a merged archive, not a
pre-linked object**.

## Environment

| | |
|---|---|
| arch | `arm64` (Apple Silicon) |
| macOS | 26.6.1 |
| clang | Apple clang version 21.0.0 (clang-2100.0.123.102), target `arm64-apple-darwin25.6.0` |
| ld | `@(#)PROGRAM:ld PROJECT:ld-1266.8`, BUILD 16:03:08 Mar 6 2026 |
| build | `cmake-build-release`, Release, Ninja, `/usr/bin/clang++` |
| branch | `work-mac` |

### Build note (not part of the spike, but it blocked it)

The `hermes` submodule checkout was on branch `n-api` at `664be0ea5`, which has
**diverged** from the recorded gitlink `9aaccbe5` (not merely ahead -- the
recorded commit is not an ancestor of it). `664be0ea5` lacks
`hermes_napi_host::ref_loop` / `unref_loop`, so `lib/event-loop/uv_event_loop.cpp`
failed to compile:

```
uv_event_loop.cpp:329:14: error: no member named 'ref_loop' in 'hermes_napi_host'
uv_event_loop.cpp:330:14: error: no member named 'unref_loop' in 'hermes_napi_host'
```

On the user's explicit instruction the submodule was checked out to the recorded
gitlink (`git submodule update --checkout hermes` -> `9aaccbe5`), after which the
build succeeded. The `n-api` branch ref still points at `664be0ea5`; the tree was
clean, so nothing was lost. **This is left checked out at the gitlink** -- a
follow-up session on this machine may want `git -C hermes checkout n-api`.

Two test binaries also had to be built (`ninja FileCheck not hermes`); they are
not dependencies of the `hermes-node` target. See T6.

## T0 build shape

```
$ lipo -info cmake-build-release/bin/hermes-node
Non-fat file: cmake-build-release/bin/hermes-node is architecture: arm64

$ ls -la cmake-build-release/bin/hermes-node
-rwxr-xr-x  1 tmikov  staff  11915960 Aug 23 09:37 cmake-build-release/bin/hermes-node
```

**Single-arch arm64**, 11,915,960 bytes. Not universal, so T7 is a mechanism
question only.

## T1 raw link line

```
/usr/bin/clang++ -Wall -Wextra -Wno-unused-parameter -Wwrite-strings -Wcast-qual
-Wno-invalid-offsetof -Wmissing-field-initializers -Wno-deprecated-copy
-Wno-noexcept-type -Wnon-virtual-dtor -Wdelete-non-virtual-dtor -ffp-contract=on
-Wno-range-loop-analysis -O3 -DNDEBUG -arch arm64 -Wl,-search_paths_first
-Wl,-headerpad_max_install_names -Wl,-export_dynamic
tools/hermes-node/CMakeFiles/hermes-node.dir/hermes-node.cpp.o -o bin/hermes-node
lib/runtime/libhermesNodeRuntime.a lib/bundle/libhermesNodeBundleTools.a
lib/bytecode-dump/libhermesNodeBytecodeDump.a
-force_load hermes/API/napi/libhermesNapi.a
lib/embedded-modules/libhermesNodeEmbeddedModules.a
lib/event-loop/libhermesNodeEventLoop.a
lib/binding-registry/libhermesNodeBindingRegistry.a
lib/bindings/libhermesNodeBindings.a external/simdutf/libsimdutf.a
external/ada/libada.a external/cares/cares/src/lib/libcares.a -lresolv
external/llhttp/libllhttp_a.a external/brotli/libbrotli_a.a
external/zstd/libzstd_a.a lib/module-loader/libhermesNodeModuleLoader.a
lib/compile-cache/libhermesNodeCompileCacheRun.a
lib/compile-cache/libhermesNodeCompileCache.a lib/process/libhermesNodeProcess.a
lib/inspector/libhermesNodeInspector.a external/libuv/libuv/libuv.a -lpthread -lm
lib/bundle/libhermesNodeBundleBuild.a lib/bundle/libhermesNodeBundleRun.a
hermes/API/napi/libhermesNapi.a hermes/API/napi/libhermesNapiCompile.a
hermes/lib/VM/libhermesVMRuntime.a hermes/API/hermes/libhermesapi.a
hermes/public/hermes/Public/libhermesPublic.a lib/bundle/libhermesNodeBundle.a
external/picohash/libpicohash.a external/zlib/libzlib_a.a
hermes/lib/libhermesvm_a.a hermes/jsi/libjsi.a hermes/lib/Parser/libhermesParser.a
hermes/lib/AST/libhermesAST.a hermes/lib/Support/libhermesSupport.a
hermes/external/boost/boost_1_86_0/libs/context/libboost_context.a
hermes/lib/Regex/libhermesRegex.a
hermes/lib/Platform/Unicode/libhermesPlatformUnicode.a -framework CoreFoundation
hermes/external/dtoa/libdtoa.a hermes/external/llvh/lib/Support/libLLVHSupport.a
hermes/external/llvh/lib/Demangle/libLLVHDemangle.a
```

Points worth extracting:

- `-force_load hermes/API/napi/libhermesNapi.a` is a **two-token pair**, exactly
  the spelling the spike's filter anticipates. There is exactly one of them.
- The link is already `-Wl,-export_dynamic`.
- Non-object arguments that must be re-added at final link: `-lresolv`,
  `-lpthread`, `-lm`, `-framework CoreFoundation`. Note **`-lresolv`**, which the
  spike's suggested T4 command line does not have; `-ldl` (which it does have)
  is absent here and is not needed on macOS.
- **No ICU anywhere**, as predicted -- `libhermesPlatformUnicode.a` plus
  `-framework CoreFoundation`.

## T2 filter (and any change you had to make)

**No change needed.** The filter as written in the spike matched reality:

```
$ wc -l < /tmp/inputs.txt
43
$ grep -c force_load /tmp/inputs.txt
1
```

43 lines = 42 inputs (1 `.o` + 41 `.a`) + the `-force_load` flag token itself.
`libhermesNapi.a` appears twice, once behind `-force_load` and once plainly, as
in the original line.

Tokens the filter dropped, verified by an inverse pass -- all of them either
compiler flags or the libraries listed above, nothing else:

```
-Wall -Wextra -Wno-unused-parameter -Wwrite-strings -Wcast-qual
-Wno-invalid-offsetof -Wmissing-field-initializers -Wno-deprecated-copy
-Wno-noexcept-type -Wnon-virtual-dtor -Wdelete-non-virtual-dtor -ffp-contract=on
-Wno-range-loop-analysis -O3 -DNDEBUG -arch arm64 -Wl,-search_paths_first
-Wl,-headerpad_max_install_names -Wl,-export_dynamic
-lresolv -lpthread -lm -framework CoreFoundation
```

I did add `-arch arm64` back to the `-r` invocation, since the host is arm64 and
being explicit costs nothing.

## T3 partial link -- success?, time, size

```
$ time clang++ -arch arm64 -r -nostdlib -o /tmp/hn-runtime.o $(cat /tmp/inputs.txt)
real	0m0.151s
exit=0
$ ls -la /tmp/hn-runtime.o
-rw-r--r--  1 tmikov  wheel  14127816 Aug 23 09:37 /tmp/hn-runtime.o
$ file /tmp/hn-runtime.o
/tmp/hn-runtime.o: Mach-O 64-bit object arm64
```

**Success, no warnings, no errors.** 0.151 s, 14,127,816 bytes, 3631 global
symbols. All 42 input paths verified to exist beforehand, so nothing was
silently skipped.

| | Linux (baseline) | macOS arm64 |
|---|---|---|
| inputs | 44 | 42 |
| `-r` time | 0.3 s | **0.15 s** |
| object size | 20.8 MB | **14.1 MB** |

`ld64` had no objection to `-r`, to `-nostdlib`, or to `-force_load` inside a
partial link.

## T4 final link, size, `codesign -dvvv` output verbatim, does it run

```
$ time clang++ -O3 -DNDEBUG -arch arm64 -Wl,-search_paths_first \
    -Wl,-headerpad_max_install_names -Wl,-export_dynamic \
    /tmp/hn-runtime.o -o /tmp/hn2 \
    -lresolv -lpthread -lm -framework CoreFoundation
real	0m0.070s
exit=0
```

| | size |
|---|---|
| `bin/hermes-node` (CMake) | 11,915,960 |
| `/tmp/hn2` (relinked) | 12,383,248 |

+467,288 bytes, +3.9%. Unlike Linux -- where the relinked binary came out the
same size as the original -- there is a small growth here. Chased down: it is
lost identical-code folding, see "Why the relinked binary is bigger" below,
which also gives a partial-link route that does not pay it.

### The headline measurement

```
$ codesign -dvvv /tmp/hn2
Executable=/private/tmp/hn2
Identifier=hn2
Format=Mach-O thin (arm64)
CodeDirectory v=20400 size=96092 flags=0x20002(adhoc,linker-signed) hashes=3000+0 location=embedded
Hash type=sha256 size=32
CandidateCDHash sha256=d0fa95c10d6a939f6564d5cb8f83f9b9fa351aa4
CandidateCDHashFull sha256=d0fa95c10d6a939f6564d5cb8f83f9b9fa351aa49d624f421a0046ecc3c20a23
Hash choices=sha256
CMSDigest=d0fa95c10d6a939f6564d5cb8f83f9b9fa351aa49d624f421a0046ecc3c20a23
CMSDigestType=2
CDHash=d0fa95c10d6a939f6564d5cb8f83f9b9fa351aa4
Signature=adhoc
Info.plist=not bound
TeamIdentifier=not set
Sealed Resources=none
Internal requirements=none

$ codesign --verify --verbose /tmp/hn2
/tmp/hn2: valid on disk
/tmp/hn2: satisfies its Designated Requirement
```

And the control, for comparison:

```
$ codesign -dvvv cmake-build-release/bin/hermes-node
Executable=/Users/tmikov/prog/hermes-node/cmake-build-release/bin/hermes-node
Identifier=hermes-node
Format=Mach-O thin (arm64)
CodeDirectory v=20400 size=92484 flags=0x20002(adhoc,linker-signed) hashes=2887+0 location=embedded
Hash type=sha256 size=32
CandidateCDHash sha256=dbcc46f33b26c5a1bbaa58c37688d88bfcc9319f
CandidateCDHashFull sha256=dbcc46f33b26c5a1bbaa58c37688d88bfcc9319ff0db4c230668f88ecd561e8a
Hash choices=sha256
CMSDigest=dbcc46f33b26c5a1bbaa58c37688d88bfcc9319ff0db4c230668f88ecd561e8a
CMSDigestType=2
CDHash=dbcc46f33b26c5a1bbaa58c37688d88bfcc9319f
Signature=adhoc
Info.plist=not bound
TeamIdentifier=not set
Sealed Resources=none
Internal requirements=none

$ codesign --verify --verbose cmake-build-release/bin/hermes-node
bin/hermes-node: valid on disk
bin/hermes-node: satisfies its Designated Requirement
```

**Same `flags=0x20002(adhoc,linker-signed)`, same `Signature=adhoc`, both
verify.** The relinked binary is signed no differently from the one CMake
produced. `Identifier` is derived from the output filename (`hn2` vs
`hermes-node`), which is the only difference and is under our control via
`-Wl,-install_name`-adjacent means or simply by naming the output.

### Does it run

```
$ /tmp/hn2 --version
hermes-node 0.0.1-100-g6b4131f
exit=0
$ /tmp/hn2 -e 'console.log("hello", 1+1)'
hello 2
exit=0
$ /tmp/hn2 -e 'console.log(eval("2*21"), new Function("return 40+2")())'
42 42
exit=0
```

No SIGKILL, no Gatekeeper complaint. `eval` and `new Function` work, confirming
the full (not lean) runtime is in there.

## T5 NAPI export counts (relinked vs original), addon load

```
$ nm -gU cmake-build-release/bin/hermes-node | grep -c ' _napi_'   ->  145
$ nm -gU /tmp/hn2                            | grep -c ' _napi_'   ->  145
$ nm -gU cmake-build-release/bin/hermes-node | wc -l               ->  2719
$ nm -gU /tmp/hn2                            | wc -l               ->  2719
```

**145 / 145**, matching the Linux baseline exactly. Better than the task asked
for: diffing the two sorted exported-symbol name lists produces **no output at
all** -- the exported symbol sets are identical, not merely equinumerous. The
`-force_load` pair survived the filter.

### Addon load

No `.node` existed in the release tree, so I built the fixture
(`ninja hello_addon.node`, 50,544 bytes) and loaded it from both binaries:

```
$ cat /tmp/addon.js
const a = require('.../cmake-build-release/hello_addon.node');
console.log('addon keys:', Object.keys(a).join(','));
console.log('call:', a.hello ? a.hello() : '(no hello)');

$ ./bin/hermes-node /tmp/addon.js        # control
addon keys:
call: world
exit=0

$ /tmp/hn2 /tmp/addon.js                 # relinked
addon keys:
call: world
exit=0
```

Byte-identical output. `dlopen` + `napi_register_module_v1` resolution against
the relinked binary's exported symbols works.

## T6 lit results, both binaries

The suite needed three binaries the `hermes-node` target does not pull in
(`FileCheck`, `not`, `hermes`) and the `hello_addon` lit param. Without them 116
of 174 tests fail *on both binaries* for harness reasons -- worth recording
because a first pass at this looks alarming and means nothing. With
`ninja FileCheck not hermes` and `--param hello_addon=...`, the real numbers:

```
python3 cmake-build-release/bin/hermes-lit -q -j1 $R/test \
  --param hermes_node=<binary> \
  --param hermes=$R/cmake-build-release/bin/hermes \
  --param FileCheck=$R/cmake-build-release/bin/FileCheck \
  --param not=$R/cmake-build-release/bin/not \
  --param hello_addon=$R/cmake-build-release/hello_addon.node \
  --param source_dir=$R --param test_exec_root=<out>
```

174 tests, 1 unsupported (`bundle-yargs.js`, `examples-installed` not set).

| binary | failures |
|---|---|
| `bin/hermes-node` (control) | `test-net-server-listen-path.js`, `test-dns-resolve.js`, `test-fs-async-verify.js` |
| `/tmp/hn2` (relinked) | `test-net-server-listen-path.js`, `test-dns-resolve.js` |

The only difference is `test-fs-async-verify.js`, and it is **flaky on both
binaries**, not a relink difference. Run six times each in isolation:

```
-- bin/hermes-node : FAIL pass pass FAIL pass pass
-- /tmp/hn2        : pass FAIL FAIL FAIL FAIL FAIL
```

So: **identical failure sets**, modulo one pre-existing flaky test. The two
stable failures are environmental (`test-dns-resolve.js` needs working DNS;
`test-net-server-listen-path.js` is a unix-socket path test) and reproduce on
the control. Best full run recorded: `Expected Passes: 170 / Unsupported: 1 /
Unexpected Failures: 3` for `/tmp/hn2`, where the third is the flake.

## T7 universal

The build tree is arm64-only (`lipo -info hermes/lib/libhermesvm_a.a` ->
`Non-fat file: ... arm64`), so this could only be answered as a mechanism
question. Per the spike I did **not** reconfigure the build.

**(a) Does one `-r` invocation accept two arches? Yes -- the mechanism works.**
Proven on a trivial case rather than on our closure:

```
$ printf 'int f(void){return 1;}\n' > /tmp/a.c
$ clang -arch arm64 -arch x86_64 -c /tmp/a.c -o /tmp/a-fat.o
$ clang -arch arm64 -arch x86_64 -r -nostdlib /tmp/a-fat.o -o /tmp/a-r.o
$ file /tmp/a-r.o
/tmp/a-r.o: Mach-O universal binary with 2 architectures:
  [x86_64:Mach-O 64-bit object x86_64] [arm64:Mach-O 64-bit object arm64]
```

`ld -r` is happy to produce a fat relocatable object. So (b)'s `lipo -create`
step is likely unnecessary; (a) should be the shipping path once a universal
build tree exists. **(b) is untested** as a whole, since there is no x86_64
slice here to lipo.

### The footgun -- and it is a serious one

Running (a) against the *real* inputs on this arm64-only tree **exits 0**:

```
$ clang++ -arch arm64 -arch x86_64 -r -nostdlib -o /tmp/fat.o $(cat /tmp/inputs.txt)
ld: warning: ignoring file lib/runtime/libhermesNodeRuntime.a, building for
    macOS-x86_64 but attempting to link with file built for macOS-arm64
   ... (one such warning per input) ...
true exit=0
```

Every single input was ignored for x86_64, and this is reported as a
**warning**, not an error. The result:

```
$ lipo -detailed_info /tmp/fat.o
architecture x86_64
    size 216            <- an empty slice
architecture arm64
    size 14127816
  arm64 syms:  3631
  x86_64 syms: 0
```

A 216-byte x86_64 slice with zero symbols, in a file that `lipo -info` cheerfully
reports as containing both architectures. The same happens for a standalone
`-arch x86_64 -r` (216 bytes, exit 0). The failure only surfaces at the *final*
link:

```
$ clang++ -arch x86_64 ... /tmp/x86_64.o -o /tmp/hn2-x86 ...
Undefined symbols for architecture x86_64:
  "_main", referenced from:
      <initial-undefines>
ld: symbol(s) not found for architecture x86_64
exit=1
```

**Whatever builds the shipped kit must not trust `-r`'s exit status.** It has to
assert per-slice that the object is non-trivial (symbol count, or size) --
otherwise a misconfigured build tree yields a "universal" kit whose second slice
is empty, and the only symptom is a confusing `_main` undefined at the customer's
final link.

## T8 otool -L

```
$ otool -L /tmp/hn2
/tmp/hn2:
	/usr/lib/libresolv.9.dylib (compatibility version 1.0.0, current version 1.0.0)
	/usr/lib/libSystem.B.dylib (compatibility version 1.0.0, current version 1356.0.0)
	/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation (compatibility version 150.0.0, current version 4424.1.255)
	/usr/lib/libc++.1.dylib (compatibility version 1.0.0, current version 2100.43.0)

$ otool -L cmake-build-release/bin/hermes-node
	(identical four lines)
```

**Identical dependency sets**, and this is much better than Linux. All four are
OS-provided and present on every supported macOS: there is no ICU dependency (as
predicted -- Hermes uses `PlatformUnicodeCF.cpp` on Apple), and `libc++` ships
with the OS, unlike the Linux build's `libstdc++` / `libgcc_s` / ICU 74. **A
macOS single-file executable produced this way is genuinely self-contained** in
a way the Linux one is not.

## Beyond the task list: linking an actual payload in

The spike stops at "can we relink". Since the point of relinking is to attach a
bundle, I checked the next step too -- it is cheap and it is the real question.
A `.hbb` built by the relinked binary, wrapped in an `.incbin` object, linked
alongside `hn-runtime.o`:

```
$ /tmp/hn2 --build-bundle=/tmp/payload.hbb /tmp/payload-src.js   # 512 bytes
$ cat /tmp/blob.s
	.section __DATA,__const
	.globl _hermes_node_bundle_blob
	.p2align 4
_hermes_node_bundle_blob:
	.incbin "/tmp/payload.hbb"
	.globl _hermes_node_bundle_blob_end
_hermes_node_bundle_blob_end:
$ clang -arch arm64 -c /tmp/blob.s -o /tmp/blob.o
$ clang++ -O3 -DNDEBUG -arch arm64 -Wl,-export_dynamic \
    /tmp/hn-runtime.o /tmp/blob.o -o /tmp/hn3 \
    -lresolv -lpthread -lm -framework CoreFoundation
link exit=0

$ codesign -dvvv /tmp/hn3
CodeDirectory v=20400 size=95964 flags=0x20002(adhoc,linker-signed) hashes=2996+0 location=embedded
Signature=adhoc
$ codesign --verify --verbose /tmp/hn3
/tmp/hn3: valid on disk
/tmp/hn3: satisfies its Designated Requirement

$ /tmp/hn3 -e 'console.log("payload-carrying binary runs", 1+1)'
payload-carrying binary runs 2

$ nm -g /tmp/hn3 | grep bundle_blob
000000010099c4b0 S _hermes_node_bundle_blob
000000010099c6b0 S _hermes_node_bundle_blob_end
```

The two symbols are 0x200 = 512 bytes apart, exactly the `.hbb` size. **The
payload-carrying binary is ad-hoc linker-signed, verifies, and runs.** That is
the end-to-end claim the spike set out to test, and it holds.

## Why the relinked binary is bigger -- and a better kit format

The +467,288 bytes (+3.9%) from T4 is **lost identical-code folding**, and
chasing it turned up a partial-link alternative that costs nothing at all.

### Where the bytes are

All of it is `__text`; every other section is flat or slightly smaller.

| section | orig | hn2 | delta |
|---|---|---|---|
| `__text` | 6,021,304 | 6,474,312 | **+453,008** |
| `__const` (TEXT) | 3,232,559 | 3,232,215 | -344 |
| `__cstring` | 195,846 | 195,850 | +4 |
| `__gcc_except_tab` | 32,792 | 31,024 | -1,768 |
| `__unwind_info` | 16,888 | 17,176 | +288 |
| `__DATA_CONST` / `__DATA` / `__LINKEDIT` | | | unchanged |

It is not alignment padding: the histogram of function start-address alignments
is essentially identical between the two. And it is not extra code -- grouping
`__text` symbols into address blocks, the 14,174 blocks common to both differ by
only 7,168 bytes in total. What differs is the **number** of blocks: 14,962 in
the original against 17,549 in the relinked binary.

### The mechanism

`ld64` folds identical function bodies, letting several symbols share one
address. The original binary does this; the relinked one does not:

```
$ nm -a bin/hermes-node | grep 'base64EncodeI[ch]E'
000000010026fc7c t ..._ZN6hermes2vm12base64EncodeIcE...   <- same address:
000000010026fc7c t ..._ZN6hermes2vm12base64EncodeIhE...      one copy, folded

$ nm -a /tmp/hn2 | grep 'base64EncodeI[ch]E'
0000000100276608 t ..._ZN6hermes2vm12base64EncodeIcE...   <- two addresses:
000000010027767c t ..._ZN6hermes2vm12base64EncodeIhE...      two copies
```

The `char` and `unsigned char` instantiations compile to identical code, so the
original link keeps one. The same story repeats across ~2,600 blocks:
`__insert_with_size` ABI-tag clones, `SemiNCAInfo::runDFS`, `__introsort`,
`_OUTLINED_FUNCTION_*` from the AArch64 machine outliner, `.cold.1` fragments.

The reason folding is lost is one header flag:

```
$ otool -h tools/hermes-node/CMakeFiles/hermes-node.dir/hermes-node.cpp.o | tail -1
 ... flags 0x00002000        <- MH_SUBSECTIONS_VIA_SYMBOLS
$ otool -h /tmp/hn-runtime.o | tail -1
 ... flags 0x00000000        <- gone
```

**`ld -r` drops `MH_SUBSECTIONS_VIA_SYMBOLS`.** Without it the final linker
cannot split `__text` into per-symbol atoms, so it can neither fold identical
code nor dead-strip. `ld -help` offers no option to preserve it.

### Two measurements that confirm it

Disabling folding on the *original* inputs reproduces the relinked size almost
exactly, and dead-stripping the `-r` object accomplishes nearly nothing:

| link | size | note |
|---|---|---|
| original inputs | 11,915,960 | baseline |
| original inputs `-no_deduplicate` | 12,385,960 | **within 2,712 bytes (0.02%) of hn2** |
| `/tmp/hn2` (from `-r` object) | 12,383,248 | |
| original inputs `-dead_strip` | 10,890,840 | -1,025,120 (-8.6%), still 145 napi exports, still runs |
| `hn-runtime.o` `-dead_strip` | 12,366,744 | -16,504 -- effectively nothing |

So the partial-link path forfeits **both** optimizations. Folding is the 3.9%
already measured; dead-stripping is a further 8.6% that the `-r` route cannot
reach at all. Against a dead-stripped, folded link the `-r` route costs
1,492,408 bytes, or 12%.

### The fix: merge archives instead of partial-linking

**Separate what is load-bearing here from what is packaging.** The finding is
negative and it is about `ld -r`: pre-linking the closure into one relocatable
object is the step that destroys atom granularity. Everything else is
distribution taste. Once you are not partial-linking, the closure simply has to
reach the customer's final link as separate object files, and shipping the 41
`.a` files exactly as CMake produces them, plus a linker response file, gets the
same binary -- that *is* the original link. Merging them buys one file instead
of 42 and hides the internal library structure. Nothing more. So the
recommendation below is not "merge for a technical benefit", it is "do not merge
in the way that loses information", and `libtool -static` is the merge that
does not.

It does not lose information because **it is not linking**. It concatenates
archive members into one archive: no symbol resolution, no atom coalescing. That
is exactly why `MH_SUBSECTIONS_VIA_SYMBOLS` survives on each member and the final
link behaves normally -- and also why duplicate symbols across the input archives
pass straight through, harmless until something `-force_load`s the result (see
the caveats). This produces a single-file kit just as `ld -r` does:

```bash
# everything from /tmp/inputs.txt except the -force_load token and the entry .o
libtool -static -o /tmp/hn-kit.a $(cat /tmp/kit-inputs.txt)      # 41 inputs, 28,146,032 bytes
clang++ -O3 -DNDEBUG -arch arm64 -Wl,-export_dynamic \
  tools/hermes-node/CMakeFiles/hermes-node.dir/hermes-node.cpp.o \
  -force_load hermes/API/napi/libhermesNapi.a /tmp/hn-kit.a -o /tmp/hn4 \
  -lresolv -lpthread -lm -framework CoreFoundation
```

| | size | napi / total exports | codesign | folding |
|---|---|---|---|---|
| `bin/hermes-node` | 11,915,960 | 145 / 2719 | adhoc, linker-signed | yes |
| `/tmp/hn2` (`ld -r`) | 12,383,248 | 145 / 2719 | adhoc, linker-signed | **no** |
| `/tmp/hn4` (`libtool`) | **11,915,952** | 145 / 2719 | adhoc, linker-signed | **yes** |

`hn4` is **8 bytes smaller than the binary CMake produced**, `otool -L` is
identical, the addon loads, `eval`/`new Function` work, and lit gives the same
two environmental failures and nothing else. Folding is confirmed restored --
the two `base64Encode` instantiations share an address again.

Four caveats for whoever builds the kit:

- **"8 bytes smaller" is not "identical".** Almost certainly a path-length or
  identifier difference, not something meaningful -- but it was not chased, and
  this is not a bit-for-bit equivalence claim.
- **Universal is untested for this route.** `libtool` can produce fat archives
  and `lipo` handles archives, but neither was tried; the tree is arm64-only.
  See T7.

- The kit archive is 28 MB against the 14 MB `-r` object, since it is
  pre-dedup, pre-strip. That is a **shipped-artifact** size, not a
  produced-binary size, and the produced binary is what matters.
- `-force_load` semantics do not survive naively. Force-loading the *merged*
  archive pulls members the real link never pulls and fails on duplicate
  symbols (`hermes::vm::matchTypeOfIs` appears in two `Operations.cpp.o`
  members, `llvh::DisplayGraph` in two `GraphWriter.cpp.o`). The entry `.o` and
  the one genuinely force-loaded archive (`libhermesNapi.a`) have to stay
  separate, as above.

None of this changes the spike's verdict -- both routes link, run and are
ad-hoc linker-signed. It changes which route the kit should use.

## Surprises / open questions

1. **`ld -r` silently produces empty slices for missing architectures** (T7).
   Exit 0, warnings only. This is the one thing here that can ship a broken
   artifact quietly, and the kit's build must assert against it explicitly.

2. **The relinked binary is 3.9% larger** -- cause found, see "Why the relinked
   binary is bigger" below. It is lost identical-code folding, and it points at
   a better kit format than `ld -r`.

3. **`-lresolv` is in the real link line but not in the spike's suggested T4
   command**, and `-ldl` (which the spike suggests) does not appear on macOS at
   all. Anything that derives the final link line must read it from the build,
   not from a hardcoded list.

4. **`Identifier=` in the CodeDirectory is derived from the output filename**
   (`hn2` here vs `hermes-node`). Harmless for ad-hoc signing, but if a shipped
   single-file executable is ever Developer-ID signed and notarized, the
   identifier becomes meaningful and should be set deliberately rather than
   inherited from whatever the user named their program.

5. **The lit suite needs `FileCheck`, `not`, `hermes` and the `hello_addon`
   param**, none of which the `hermes-node` target builds. A fresh Release tree
   plus a hand-written `hermes-lit` invocation reports 116/174 failures that are
   pure harness noise. Anyone repeating this should build those first and
   always run the control.

6. **`test-fs-async-verify.js` is flaky on this machine**, independent of the
   relink -- it failed 2/6 on the control and 5/6 on the relinked binary in
   isolated runs. It is not a partial-link finding, but it is a real pre-existing
   flake worth chasing separately.

7. **The `hermes` submodule was left at the recorded gitlink `9aaccbe5`**, not at
   the `n-api` branch tip `664be0ea5` it was on when this started. See the build
   note under Environment.

8. **The folding problem may be macOS-only, and nobody has checked.** The
   handoff doc reports the Linux `-r` binary came out at 13.3 MB, "same size as
   the real `hermes-node`". That is consistent with GNU ld not folding identical
   code by default (it is not the default there the way it is in `ld64`), in
   which case `ld -r` costs nothing on Linux and only macOS needs the archive
   form. **This is inference from the handoff's numbers, not a measurement** --
   nothing Linux-side was run in this spike. Worth ten minutes on a Linux box
   before deciding whether the kit format is per-platform or uniform. Uniform
   is probably still right on grounds of having one story rather than two.

9. **Universal is unproven end-to-end.** The mechanism works on a toy case, but
   nobody has yet partially linked a genuinely two-slice hermes-node closure.
   That needs a universal build tree, which was out of scope here.
