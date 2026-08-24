# macOS partial-link spike -- handoff

- **Branch:** `work-mac`
- **Opened:** 2026-08-23
- **Audience:** a fresh Claude Code session running on macOS, with no prior context
- **Deliverable:** fill in and commit `docs/notes/2026-08-23-macos-partial-link-results.md`
  (template at the bottom of this file)

This is a **spike**, not a feature. Its output is an answer, not code you keep.
Nothing here should change a single line of the project's source. If something
fails, that is a result -- record it exactly and move on. Do not fix, patch, or
work around anything.

---

## 1. Why this exists

hermes-node can already compile a program's whole `require()` graph to Hermes
bytecode and write it into one container file (`--build-bundle=app.hbb`), then
run that container with no source tree present (`--bundle=app.hbb`). See the
"AOT Bundles" section of `CLAUDE.md` for the format and its guarantees.

The next step is a **single executable**: one file that is the runtime and the
program together, with no `.hbb` beside it.

Everyone else in this space solves that the same way, and all of them pay the
same tax:

| Runtime | How the payload attaches | macOS cost |
|---|---|---|
| Node.js SEA | external `postject` tool injects a blob into a *copy* of the `node` binary (ELF note / Mach-O section / PE resource), flips a sentinel "fuse" byte | docs tell you to `codesign --remove-signature` first and re-sign after |
| Deno `compile` | `libsui` (`denoland/sui`) injects into a copy of `denort` | arm64: inserts an `LC_SEGMENT_64` before `__LINKEDIT`, shifts every downstream offset, then **hand-writes an ad-hoc signature in pure Rust** (SHA-256 page hashes, CodeDirectory with `flags=0x20002` = adhoc\|linkerSigned). x86_64: strip signature, append, patch `__LINKEDIT` sizes, shell out to `codesign -s -` |
| Bun `--compile` | appends a serialized module graph + `Offsets` + `"\n---- Bun! ----\n"` trailer, in a real per-platform section | **gave up on preserving the signature**; the user re-signs by hand |

The reason they all suffer is Apple TN2206: appending to a Mach-O is expressly
prohibited and verification fails, and on Apple Silicon a binary whose
signature does not verify is SIGKILLed rather than run.

**Our alternative is to not inject anything.** Instead of rewriting a prebuilt
binary, we *link a new one* with the system linker: the bundle becomes an
ordinary `.o` (via `.incbin`) and gets linked against a pre-linked object
containing all of hermes-node.

If that works, the entire macOS signing problem disappears -- because `ld64`
already ad-hoc signs the binaries it produces. Note that sui's hand-built
CodeDirectory carries the `linkerSigned` flag: **sui is reimplementing what the
linker does.** If we use the linker, we get it for free.

**That is the question you are here to answer.**

---

## 2. Design decisions already made (do not relitigate)

- The produced executable contains the **full** runtime (`hermesvm_a`), not the
  lean one (`hermesvmlean_a`). Behaviour is identical to `--bundle` today,
  including `eval` and `new Function`.
- Because the runtime is full, **the object closure is fixed** -- only the
  bundle varies between apps. That is what makes a single pre-linked
  relocatable object viable as the shipped "kit".
- Targets are Linux and macOS. Windows is deferred (we cannot test it).

---

## 3. What is already proven on Linux

Measured on `cmake-build-release`, x86_64, GNU ld. **These are your comparison
points.**

| Step | Result |
|---|---|
| `clang++ -r -nostdlib` over the full closure (44 inputs) | 0.3 s, **20.8 MB** object |
| final link from that object alone | 0.23 s, **13.3 MB** binary -- *same size as the real `hermes-node`* |
| `napi_` dynamic exports / total dynamic symbols | **145 / 3717** -- identical to the original |
| `.node` addon `dlopen` | works, same as original |
| full lit suite (174 tests) | **identical 8-failure set** to the original binary (those 8 are pre-existing in that stale build tree) |
| link drivers | works with `clang++`, and with plain `clang` **or `gcc`** plus explicit `-lstdc++` |

Method on Linux, for reference:

```bash
ninja -t commands bin/hermes-node | tail -1 | sed 's/^: && //; s/ && :$//' > orig-link.sh
tr ' ' '\n' < orig-link.sh | awk '/^-Wl,--(no-)?whole-archive$/ || /\.(o|a)$/' > static-inputs.txt
clang++ -r -nostdlib -o hn-runtime.o $(cat static-inputs.txt)
clang++ -O3 -DNDEBUG -rdynamic hn-runtime.o -o hn2 -lpthread -ldl -lrt -lm <icu .so paths>
```

The filter exists because **`ld -r` refuses shared libraries**
(`attempted static link of dynamic object`). On Linux the offender is system
ICU. On macOS it will be `-framework CoreFoundation` instead -- Hermes does not
use ICU on Apple at all (`hermes/CMakeLists.txt`: `if (APPLE OR EMSCRIPTEN ...)
set(ICU_FOUND 1)`, and `PlatformUnicodeCF.cpp` is used).

---

## 4. Setup

You need a **Release** build. Do not use an ASAN build -- it distorts every
size number and is far slower.

```bash
cd <repo root>
git branch --show-current          # expect: work-mac
cmake -B cmake-build-release -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build cmake-build-release --target hermes-node
```

**If `hermes/` is empty**, the submodule is not checked out. **Stop and ask the
user before running any `git submodule` command.** The local Hermes checkout is
often intentionally ahead of the recorded gitlink, and updating it destroys
work. This is a hard rule.

Record your environment in the results file: `uname -m`, `sw_vers -productVersion`,
`clang --version`, `ld -v` (first line).

---

## 5. Tasks

Work through these in order. Record the actual command output for each -- not a
summary, not a paraphrase. If a command fails, paste the error verbatim.

### T0. Is the build universal?

```bash
lipo -info cmake-build-release/bin/hermes-node
ls -la cmake-build-release/bin/hermes-node
```

Release CI ships `macos-universal`, but a local default build is probably
single-arch. **Record which.** If it *is* universal, note that
`clang -arch arm64 -arch x86_64 -r` may not work in one invocation, and see T7.

### T1. Capture the real link line

```bash
cd cmake-build-release
ninja -t commands bin/hermes-node | tail -1 | sed 's/^: && //; s/ && :$//' > /tmp/orig-link.sh
fold -w 150 /tmp/orig-link.sh
```

Paste the whole thing into the results file. It is the primary raw datum here:
it tells us what the macOS closure actually contains (frameworks, `-force_load`,
`-Wl,-export_dynamic`, arch flags).

### T2. Filter it

**Critical gotcha:** on macOS the whole-archive mechanism is `-force_load <path>`,
which is a **two-token pair**. A naive "keep everything ending in `.a`" filter
keeps the archive but drops the `-force_load` flag, silently losing every NAPI
symbol -- and the binary will still link and still run simple scripts. It will
only fail when a native addon tries to resolve `napi_*`. T5 is what catches this.

```bash
python3 - <<'PY' > /tmp/inputs.txt
import shlex
argv = shlex.split(open('/tmp/orig-link.sh').read())
keep, i = [], 1                      # argv[0] is the compiler driver
while i < len(argv):
    a = argv[i]
    if a == '-o':                                i += 2; continue
    if a in ('-force_load', '-Wl,-force_load'):  keep += [a, argv[i+1]]; i += 2; continue
    if a.endswith(('.o', '.a')):                 keep.append(a)
    i += 1
print('\n'.join(keep))
PY
wc -l < /tmp/inputs.txt
grep -c force_load /tmp/inputs.txt     # must be >= 1, else the filter missed it
```

If `-force_load` appears in `/tmp/orig-link.sh` in a spelling the script does
not match (e.g. `-Wl,-force_load,<path>` as a single comma-joined token),
**say so in the results and adjust the filter to match reality.** Record what
you changed.

### T3. Partial link

```bash
time clang++ -r -nostdlib -o /tmp/hn-runtime.o $(cat /tmp/inputs.txt)
echo "exit=$?"
ls -la /tmp/hn-runtime.o
```

Record: does it succeed, how long, how big. Compare to Linux's 0.3 s / 20.8 MB.

If it fails, the error text *is* the finding. Common candidates to note:
`ld64` rejecting `-r` with some input, a duplicate-symbol complaint, or an
objection to `-nostdlib`.

### T4. Final link, and the signature question

```bash
time clang++ -O3 -DNDEBUG -Wl,-export_dynamic /tmp/hn-runtime.o -o /tmp/hn2 \
  -framework CoreFoundation -lpthread -ldl -lm
echo "exit=$?"
ls -la /tmp/hn2      # compare to bin/hermes-node
```

Add back any other `-framework` or `-l` argument that appeared in
`/tmp/orig-link.sh` and is not in the command above.

**Then the headline measurement:**

```bash
codesign -dvvv /tmp/hn2 2>&1 | head -20
codesign --verify --verbose /tmp/hn2 2>&1 | head -10
# and for comparison, the binary CMake produced:
codesign -dvvv cmake-build-release/bin/hermes-node 2>&1 | head -20
```

Record **verbatim**. What we are looking for is whether the CodeDirectory
exists and whether its flags include `adhoc` and `linker-signed`. If
`codesign -dvvv` says `code object is not signed at all`, that is equally
important to know -- it would mean linking buys us nothing on this platform and
we need a `codesign -s -` step like everyone else.

Also note whether the binary simply *runs* -- on Apple Silicon an unsigned or
invalidly-signed binary is killed outright:

```bash
/tmp/hn2 --version
/tmp/hn2 -e 'console.log("hello", 1+1)'
/tmp/hn2 -e 'console.log(eval("2*21"), new Function("return 40+2")())'
```

### T5. NAPI exports and a real addon

This is what catches a broken `-force_load` filter.

```bash
nm -gU cmake-build-release/bin/hermes-node | grep -c ' _napi_'
nm -gU /tmp/hn2                          | grep -c ' _napi_'
nm -gU cmake-build-release/bin/hermes-node | wc -l
nm -gU /tmp/hn2                          | wc -l
```

The two `_napi_` counts **must match**. On Linux both were 145.

Then actually load an addon. There should be a `hello_addon.node` somewhere in
the build tree (`find cmake-build-release -name '*.node'`). If one exists:

```bash
cat > /tmp/addon.js <<'EOF'
const a = require('<absolute path to the .node file>');
console.log('addon keys:', Object.keys(a).join(','));
EOF
cmake-build-release/bin/hermes-node /tmp/addon.js    # control
/tmp/hn2 /tmp/addon.js                               # relinked
```

Both must behave identically. If no `.node` exists in the tree, say so; the
test suite in T6 covers addons anyway.

### T6. Full test suite, both binaries

**Run the control too.** A failure list is meaningless without it -- the build
tree may have pre-existing failures (on Linux it had 8).

```bash
R=$(pwd)
python3 cmake-build-release/bin/hermes-lit -q $R/test \
  --param hermes_node=/tmp/hn2 \
  --param hermes=$R/cmake-build-release/bin/hermes \
  --param FileCheck=$R/cmake-build-release/bin/FileCheck \
  --param not=$R/cmake-build-release/bin/not \
  --param source_dir=$R \
  --param test_exec_root=/tmp/lit-relink 2>&1 | tail -25

python3 cmake-build-release/bin/hermes-lit -q $R/test \
  --param hermes_node=$R/cmake-build-release/bin/hermes-node \
  --param hermes=$R/cmake-build-release/bin/hermes \
  --param FileCheck=$R/cmake-build-release/bin/FileCheck \
  --param not=$R/cmake-build-release/bin/not \
  --param source_dir=$R \
  --param test_exec_root=/tmp/lit-orig 2>&1 | tail -25
```

The finding we want is **"identical failure sets"** or a precise diff.

### T7. Universal binaries (only if T0 said universal, or if you can build one)

Release CI ships a universal macOS binary, so the shipped kit will eventually
have to be universal too. Establish which of these works:

```bash
# (a) does a single -r invocation accept two arches?
clang++ -arch arm64 -arch x86_64 -r -nostdlib -o /tmp/fat.o $(cat /tmp/inputs.txt)

# (b) if not: per-arch objects, then lipo
clang++ -arch arm64  -r -nostdlib -o /tmp/arm64.o  $(cat /tmp/inputs.txt)
clang++ -arch x86_64 -r -nostdlib -o /tmp/x86_64.o $(cat /tmp/inputs.txt)
lipo -create /tmp/arm64.o /tmp/x86_64.o -output /tmp/fat.o
lipo -info /tmp/fat.o
```

(b) will only work if the build tree actually contains both slices. If it does
not, just record that (a) failed/succeeded and note (b) as untested. **Do not
reconfigure the build to make this work** -- out of scope for the spike.

### T8. What does the result still depend on?

```bash
otool -L /tmp/hn2
```

On Linux the relinked binary still needed system ICU 74, `libstdc++` and
`libgcc_s`. Record the macOS equivalent -- this matters for how self-contained
a shipped executable really is.

---

## 6. Rules

- **Do not push.** Not to any remote, ever. Do not offer to.
- **Do not touch the `hermes` submodule.** Never `git add hermes`, never
  `git submodule update hermes`, without the user explicitly saying so.
- **Do not modify project source.** This spike changes nothing but the results
  file. If you find yourself editing a `CMakeLists.txt`, stop.
- **Commit only** `docs/notes/2026-08-23-macos-partial-link-results.md`
  (and this file if you correct an error in it).
- **Report honestly.** A negative result is the most valuable thing you can
  produce here. Do not soften it, do not work around it, do not "fix" it so the
  numbers look better. If `ld64` refuses `-r`, say so plainly and stop.
- Use `/tmp` freely for scratch; nothing there is a deliverable.

## 7. Results template

Create `docs/notes/2026-08-23-macos-partial-link-results.md` with this shape
and commit it.

```markdown
# macOS partial-link spike -- results

Answers the spike in `2026-08-23-macos-partial-link-spike.md`.

## Verdict

<One paragraph. Does linking work on macOS? Does ld64 ad-hoc sign the output?
Would we need a codesign step anyway? Lead with the answer.>

## Environment

- arch / macOS / clang / ld versions
- universal or single-arch build

## T0 build shape
## T1 raw link line
## T2 filter (and any change you had to make)
## T3 partial link -- success?, time, size
## T4 final link, size, `codesign -dvvv` output verbatim, does it run
## T5 NAPI export counts (relinked vs original), addon load
## T6 lit results, both binaries
## T7 universal
## T8 otool -L

## Surprises / open questions

<Anything that did not match the Linux baseline, or that a follow-up session
should know.>
```
