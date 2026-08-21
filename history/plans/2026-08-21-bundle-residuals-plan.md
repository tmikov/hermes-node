# Bundle Residuals Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use
> superpowers:subagent-driven-development (recommended) or
> superpowers:executing-plans to implement this plan task-by-task. Steps use
> checkbox (`- [ ]`) syntax for tracking.

**Goal:** Clear the fourteen findings left open after the closed-world bundle
round, from a `require.resolve()` that fails silently at run time down to a
duplicated temp-tree fixture.

**Architecture:** Six tasks, ordered by what depends on what. Task 1 changes
what the producer packages, so it lands first and everything else is written
against the new reality rather than reworked around it. Task 6's
duplicate-identity rejection lands last, after Task 5 has pinned the dedup it
would otherwise turn from a slightly larger container into a bundle that
refuses to open.

**Tech Stack:** C++17, CMake + Ninja, GTest (`unittests/`), LLVM lit
(`test/`), Hermes Node-API, JavaScript (`libjs/`).

Origin: the residuals walkthrough recorded in
`.superpowers/sdd/2026-08-19-closed-world-bundle-plan/progress.md`, which
carries the decision and the reasoning for each item. Closed-world design:
`history/plans/2026-08-19-closed-world-bundle-design.md`. Subsystem progress:
`history/plans/progress-aot-bundle.md` (append there at the end; do not start
a new progress file).

## Global Constraints

- New files carry `Copyright (c) Tzvetan Mikov.` -- NOT Meta Platforms.
- Commit messages ASCII only, no emojis.
- Before every commit: `./utils/format.sh -f`, then
  `cmake --build cmake-build-asan --target check-hermes-node`. Both clean.
- Never modify anything under `libjs-node/`. `libjs/` (no suffix) is ours.
- Never check in anything under `examples/*/node_modules/`.
- Do not weaken a test to get it green. Task 1 deliberately changes what is
  packaged, so module and edge counts in `test/bundle-verbose.js` and
  `test/bundle-dump.js` legitimately move; the new value must be computed
  from what the code now produces and justified in the report. If you cannot
  explain why a number changed, it is a defect.
- `hermesNodeBundle` must not link the Hermes VM. `BundleFormatTest`,
  `BundleResolveTest`, `BundleFileSourceTest` and `BundleToolsTest` run with
  no runtime and must keep doing so.
- A lit FileCheck prefix must NOT end in the letters `RUN` -- lit matches its
  `RUN:` keyword anywhere in a line. Use a suffix such as `EXEC`.
- Run one lit test with (paths must be absolute):
  `python3 cmake-build-asan/bin/hermes-lit -v $(pwd)/test/<name>.js --param hermes_node=$(pwd)/cmake-build-asan/bin/hermes-node --param hermes=$(pwd)/cmake-build-asan/bin/hermes --param FileCheck=$(pwd)/cmake-build-asan/bin/FileCheck --param not=$(pwd)/cmake-build-asan/bin/not --param source_dir=$(pwd) --param test_exec_root=$(pwd)/cmake-build-asan/test`

---

### Task 1: A literal require.resolve() is a discovery edge

Today the scanner treats `require.resolve` as a property read, so it yields
no edge and no warning: a `require.resolve('./x')` whose target nothing else
requires is a hard run-time failure with no build-time signal at all. That is
the failure this whole subsystem set out to eliminate, reachable through a
different syntax.

**Files:**
- Modify: `lib/bundle/require_scanner.cpp`
- Modify: `include/hermes/node-compat/bundle/require_scanner.h` (doc only)
- Test: `unittests/RequireScannerTest.cpp`, `test/bundle-include.js`

**Interfaces:**
- Consumes: the existing `RequireVisitor`, `scanRequires(source, enableTS,
  out, error, gaps)`.
- Produces: no signature change. `out` simply gains the specifiers named by
  literal `require.resolve()` calls, in the same first-seen order.

- [ ] **Step 1: Write the failing unit test**

In `unittests/RequireScannerTest.cpp`:

```cpp
TEST(RequireScannerTest, RecordsLiteralRequireResolveTargets) {
  // A resolve is as statically visible as a require, and its target has to
  // be in the container or the call throws at run time with nothing said at
  // build time.
  EXPECT_EQ(
      scan("require.resolve('./data.json');"),
      (std::vector<std::string>{"./data.json"}));
  // Deduplicated against a require() of the same specifier, like any other.
  EXPECT_EQ(
      scan("require('./a'); require.resolve('./a');"),
      (std::vector<std::string>{"./a"}));
  // A computed argument is still invisible, and still not an escape.
  EXPECT_TRUE(scan("require.resolve(name);").empty());
  // Not our require, so not our edge.
  EXPECT_TRUE(scan("function f(require) { require.resolve('./x'); }").empty());
  // Other properties of require load nothing and contribute nothing.
  EXPECT_TRUE(scan("require.cache; require.main; require.extensions;").empty());
}
```

- [ ] **Step 2: Run it and watch it fail**

```bash
cmake --build cmake-build-asan --target RequireScannerTest && \
  cmake-build-asan/unittests/RequireScannerTest --gtest_filter='*RequireResolve*'
```
Expected: FAIL -- the specifier list is empty.

- [ ] **Step 3: Collect the specifier**

In `RequireVisitor`, a call whose callee is a MemberExpression naming
`resolve` on the module's `require` now contributes its literal argument.
The existing `visit(MemberExpressionNode)` must keep calling `accountFor` on
the object, so `require.resolve` still is not an escape.

Reuse the literal extraction already in `collect()` rather than writing a
second copy of the string-literal / single-quasi-template logic -- factor it
into a helper both call, so the two can never disagree about what counts as
a literal.

- [ ] **Step 4: Write the failing lit test**

Append to `test/bundle-include.js`:

```js
// A literal require.resolve() names a real dependency: its target is
// packaged like any other edge, so the call answers from the container with
// the tree deleted. Before this it resolved nothing and threw.
// RUN: rm -rf %t.rr && mkdir -p %t.rr
// RUN: echo '{ "v": 3 }' > %t.rr/data.json
// RUN: echo "const p = require.resolve('./data.json'); console.log('RR', require(p).v);" > %t.rr/cli.js
// RUN: %hermes-node --build-bundle=%t.rr/app.hbb %t.rr/cli.js
// RUN: rm -f %t.rr/cli.js %t.rr/data.json
// RUN: %hermes-node --bundle=%t.rr/app.hbb | %FileCheck --check-prefix=RREXEC %s
// RREXEC: RR 3
```

- [ ] **Step 5: Run the whole suite and settle the counts**

```bash
cmake --build cmake-build-asan --target check-hermes-node
```
Any fixture that calls `require.resolve` on something not otherwise required
now packages one more module. Update the affected expected counts in
`test/bundle-verbose.js` / `test/bundle-dump.js` to the new correct value and
record in your report, per fixture, why the number moved.

- [ ] **Step 6: Re-run the examples, which is where the size risk lives**

```bash
cmake --build cmake-build-release --target hermes-node
./examples/run-examples.sh cmake-build-release
```
Expected: PASS. Report the before/after container sizes for
`examples/babel-parser`'s three bundles and `examples/yargs-cli`. A large
jump is worth stating plainly; a resolve does not imply a load, so this
change can package files the program never opens.

- [ ] **Step 7: Commit**

```bash
./utils/format.sh -f
git add lib/bundle include/hermes/node-compat/bundle unittests test
git commit -m "bundle: a literal require.resolve() is a discovery edge"
```

---

### Task 2: Stop a failed probe from widening the bundle root

**Files:**
- Modify: `lib/bundle/bundle_build.cpp`
- Modify: `include/hermes/node-compat/bundle/bundle_format.h`
- Test: `test/bundle-resolution-inputs.js`

**Interfaces:**
- Produces: `constexpr uint32_t kResolveOnly = 0;` in `bundle_format.h`,
  beside `kRequirable`.

- [ ] **Step 1: Write the failing test**

Append to `test/bundle-resolution-inputs.js`:

```js
// A package.json read while probing a specifier that never resolved is not
// a reason to move the bundle root. Here `foo` resolves to nothing (its
// package.json main points at a file that does not exist), and it sits
// OUTSIDE the directory every packaged module shares -- so before this it
// pulled the root up a level and changed every identity in the container.
// RUN: rm -rf %t.wide && mkdir -p %t.wide/app %t.wide/node_modules/foo
// RUN: echo '{ "main": "nope.js" }' > %t.wide/node_modules/foo/package.json
// RUN: echo "module.exports = { v: 1 };" > %t.wide/app/dep.js
// RUN: echo "try { require('foo'); } catch (e) {} console.log('W', require('./dep').v);" > %t.wide/app/cli.js
// RUN: %hermes-node --build-bundle=%t.wide/app/app.hbb %t.wide/app/cli.js 2>&1 | %FileCheck --check-prefix=WIDE %s
// WIDE: bundle root: {{.*}}/app
//
// And the identities stay relative to that root.
// RUN: %hermes-node --bundle=%t.wide/app/app.hbb --dump | %FileCheck --check-prefix=WIDEDUMP %s
// WIDEDUMP: cli.js
```

- [ ] **Step 2: Run it and watch it fail**

Expected: FAIL -- the root is the parent of `app/`, and identities read
`app/cli.js`.

- [ ] **Step 3: Compute the root before filtering**

In `buildBundle`, move `commonAncestor(paths)` to before the
`readPackageJsonPaths()` loop, and in that loop skip any recorded path not
under the computed root:

```cpp
  // The root is the deepest directory every packaged MODULE shares.
  // Resolution inputs do not get a vote: a package.json read while probing a
  // specifier that failed can sit anywhere the walk looked, and letting it
  // widen the root would move every identity in the container. Dropping one
  // outside the root costs nothing either, because BundleFileSource answers
  // nothing outside the root, so the consumer could never have read it.
  std::string root = commonAncestor(paths);
```

Keep the `bundle root:` line printing where it does today, so its position in
the output does not move.

- [ ] **Step 4: Name the flag**

Add `constexpr uint32_t kResolveOnly = 0;` to `bundle_format.h` with a
comment saying it is the absence of `kRequirable`, and use it at the
`info.flags = 0;` site in `bundle_build.cpp`.

- [ ] **Step 5: Run the tests and commit**

```bash
cmake --build cmake-build-asan --target check-hermes-node
./utils/format.sh -f
git add lib/bundle include/hermes/node-compat/bundle test
git commit -m "bundle: a failed probe no longer widens the bundle root"
```

---

### Task 3: Say what the loader actually did

**Files:**
- Modify: `libjs/bundle-loader.js`
- Modify: `lib/bundle/bundle_run.cpp`
- Modify: `CLAUDE.md`
- Test: `test/bundle-scanner.js`

- [ ] **Step 1: Write the failing test**

`test/bundle-scanner.js`'s MISS case currently asserts only the miss line.
Extend it so the log distinguishes all three outcomes -- an edge-table hit, a
container-resolve hit, and a miss:

```js
// MISS: [bundle] edge: ./dep from cli.js -> dep.js
// MISS: [bundle] resolve: {{.*}} from cli.js
// MISS: [bundle] miss: ./dyn from cli.js
```

Build the fixture so all three occur in one run: a literal `require('./dep')`
(edge), a computed require of something packaged (resolve), and a computed
require of something absent (miss).

- [ ] **Step 2: Run it and watch it fail**

Expected: FAIL -- only the miss line is emitted.

- [ ] **Step 3: Log all three**

In `libjs/bundle-loader.js`, generalise `logMiss` into one helper taking the
outcome, and call it on the edge-table hit and the container-resolve hit as
well as the miss. Keep the `HERMES_NODE_DEBUG_NATIVE=BUNDLE` gate. The exact
shape, which the Step 1 test asserts:

```
[bundle] edge: <request> from <importer> -> <identity>
[bundle] resolve: <request> from <importer> -> <identity>
[bundle] miss: <request> from <importer>
```

The miss line keeps its current wording exactly, so an existing reader's
expectations still hold.

**Builtins and embedded modules stay silent.** They are forwarded before any
container lookup happens, so they have no outcome to report, and
`test/bundle-scanner.js` asserts today -- with
`--implicit-check-not="miss: path"` -- that a builtin produces no line at
all. A log line for every `require('path')` would drown the log in exactly
the entries that are never interesting.

- [ ] **Step 4: Guard __bundleLoad itself**

In `lib/bundle/bundle_run.cpp`'s `bundleLoadCallback`, refuse an identity
whose `kRequirable` flag is clear, with a thrown error. The two current
callers already check, so this is the invariant holding where the bytes are
handed out rather than only at its callers. Add a lit case: from inside a
bundled module, `globalThis.__bundleLoad('<a packaged package.json>')` throws.

- [ ] **Step 5: Update the doc**

`CLAUDE.md`'s bullet describing `HERMES_NODE_DEBUG_NATIVE=BUNDLE` should say
it now names the outcome of every bundled require, not only the misses.

- [ ] **Step 6: Run the tests and commit**

```bash
cmake --build cmake-build-asan --target check-hermes-node
./utils/format.sh -f
git add libjs lib/bundle CLAUDE.md test
git commit -m "bundle: log every require outcome, and guard __bundleLoad"
```

---

### Task 4: One stripRoot, and the hygiene backlog

**Files:**
- Modify: `include/hermes/node-compat/bundle/file_source.h`
- Modify: `lib/bundle/bundle_file_source.cpp`
- Modify: `lib/bundle/bundle_run.cpp`
- Modify: `lib/bundle/disk_file_source.cpp`
- Modify: `include/hermes/node-compat/bundle/bundle_resolve.h`
- Modify: `unittests/BundleFileSourceTest.cpp`

**Interfaces:**
- Produces: `std::optional<std::string_view> BundleFileSource::identityFor(std::string_view path) const;`
  -- the public form of the private `stripRoot`.

- [ ] **Step 1: Delete the second copy**

`identityUnderRoot()` in `bundle_run.cpp` is a second implementation of
`stripRoot`, carrying a comment that the two must agree byte for byte
including the root-is-`"/"` case. Two copies of a rule that must not drift is
the shape this subsystem's whole design exists to avoid. Expose
`identityFor()` on `BundleFileSource`, call it from `bundle_run.cpp`, delete
the copy and the comment. The existing `BundleFileSourceTest` containment
cases already pin the behaviour; add one asserting `identityFor` directly.

- [ ] **Step 2: The three performance items**

`DiskFileSource`: dedup recorded paths through an `std::unordered_set`
alongside the vector that preserves order, rather than a linear scan.
`BundleFileSource`: build the sorted identity index lazily on first query
instead of in the constructor, so a bundle whose edge table answers
everything never pays for it (`std::optional` around the vector; the
constructor keeps only the reader and the root). `stripRoot`: precompute the
`root + "/"` prefix once and take a `std::string_view`, so a query allocates
nothing.

- [ ] **Step 3: The five documentation and hygiene items**

(a) Comment at `stripRoot`/`identityFor`'s declaration that the returned view
points into the caller's argument, not into the object -- the shape that
already caused one dangling-view bug. (b) State the "at most one trailing
slash" invariant at `joinNormalized` in `bundle_resolve.cpp` and in the
`FileSource` interface contract, noting the two backends honour it by
different mechanisms. (c) Forward-declare `BundleReader` in `file_source.h`
instead of including `bundle_reader.h`, and include `<cstdint>` for the
`uint32_t` it uses. (d) `buildContainerFileSource` in
`unittests/BundleFileSourceTest.cpp` must fail cleanly rather than
dereferencing an empty optional after `EXPECT_TRUE`. (e) `bundle_resolve.h`'s
doc pointer should point at the declaration carrying the algorithm, not at
the forwarding stub.

- [ ] **Step 4: Run the tests and commit**

```bash
cmake --build cmake-build-asan --target check-hermes-node
./utils/format.sh -f
git add include lib unittests
git commit -m "bundle: one stripRoot, lazy index, and the hygiene backlog"
```

---

### Task 5: The tests the reviews asked for

**Files:**
- Modify: `unittests/BundleFileSourceTest.cpp`
- Modify: `test/bundle-escapes.js`, `test/bundle-container-resolve.js`,
  `test/bundle-include.js`

- [ ] **Step 1: Widen the agreement matrix**

`AgreesWithTheDiskBackend` is the guard against a specifier resolving one way
at build time and another at run time. Add the shapes where a one-sided
change actually diverges: a file beside a directory of the same name
(`lib.js` next to `lib/`), trailing-slash specifiers (`./lib/`, `dep/`),
`.ts` and `.json` extension probes, a nested `node_modules`
(`node_modules/dep/node_modules/inner`), an identity that is a string prefix
of another (`util.js` beside `utils.js`), and an absolute specifier inside
the root.

- [ ] **Step 2: Put the shipping shape under test**

Today the test's container mirrors the tree exactly. A real container never
does -- `.node`, `.mjs` and assets are skipped -- so a divergence that only
appears when the container is MISSING something the disk has cannot be seen.
Add a second fixture whose container deliberately omits files the tree holds,
and assert both halves: the backends agree wherever the container has the
file, and a skipped file is a clean miss rather than a neighbour picked up by
the extension probe (an `addon.node` beside an `addon.js` is the case that
would catch it).

- [ ] **Step 3: Pin the non-writable guard directly**

`globalThis.__closeDiskModuleLoading` is non-writable and non-configurable,
and nothing tests it any more: the case that did was rewritten when `-r`
became a refusal. Test the property itself from inside a bundled module --
assignment and `delete` both fail, and the disk escape still throws -- rather
than through an attack that no longer exists.

- [ ] **Step 4: Cover the _resolveFilename wrapper's own branches**

The wrapper is what makes `createRequire().resolve()` answer from the
container, and was the most invasive edit of the closed-world round. Its
`options.paths` branch and its embedded-`ws` passthrough have no test of
their own. Add: a `createRequire()`'d `resolve` with a `paths` entry inside
the bundle root answering from the container, and a `createRequire()`'d
`resolve('ws')` answering `node:ws` without walking the disk.

- [ ] **Step 5: Pin --include dedup, in both directions**

Assert the container holds exactly one record when `--include` names the same
file twice, and when it names something the entry's own graph already
reaches. Task 6 turns a regression here into a container that will not open,
so these want to exist first.

- [ ] **Step 5b: Two items deferred from earlier tasks in this plan**

Task 1 added `require?.resolve(...)` support -- two predicate overloads and
a dispatch branch -- with no test of its own; only the plain
`require.resolve` form and the pre-existing `require?.(...)` direct-call
form are covered. Add a scanner unit case for the optional-chain form.

Task 3's new `resolve:` log line is asserted as
`[bundle] resolve: {{.*}} from cli.js`, which pins neither the request text
nor the `-> <identity>` suffix, where the sibling `edge:` and `miss:` lines
pin both. Tighten it to match them. This looseness came from the plan's own
Step 1 sketch, not from the implementer.

- [ ] **Step 6: Run the tests and commit**

```bash
cmake --build cmake-build-asan --target check-hermes-node
./utils/format.sh -f
git add unittests test
git commit -m "bundle: test the shapes the reviews asked for"
```

---

### Task 6: Reject a malformed container, and share the fixture

**Files:**
- Modify: `lib/bundle/bundle_reader.cpp`
- Create: `unittests/TempTree.h`
- Modify: `unittests/BundleFileSourceTest.cpp`, `unittests/BundleResolveTest.cpp`,
  `unittests/BundleToolsTest.cpp`, `unittests/CompileCacheTest.cpp`,
  `unittests/CompileCacheRunTest.cpp`
- Test: `unittests/BundleFormatTest.cpp`

- [ ] **Step 1: Write the failing test**

In `unittests/BundleFormatTest.cpp`, extend the identity-validation case with
an empty path segment, and add a duplicate-identity case:

```cpp
TEST(BundleFormatTest, RejectsAnEmptyPathSegment) {
  for (const char *identity : {"a/", "a//b", "/a"}) {
    SCOPED_TRACE(identity);
    BundleWriter w;
    w.setEntry(w.addModule(identity, ModuleKind::kJavaScript, kRequirable, "A"));
    auto bytes = w.serialize(bundleGenerationTag());
    std::string error;
    EXPECT_FALSE(
        BundleReader::open(bytes.data(), bytes.size(), &error).has_value());
  }
}

TEST(BundleFormatTest, RejectsADuplicateIdentity) {
  // One record per identity is what lets byIdentity and BundleFileSource
  // agree about what a container holds.
  BundleWriter w;
  w.setEntry(w.addModule("a.js", ModuleKind::kJavaScript, kRequirable, "A"));
  w.addModule("a.js", ModuleKind::kJavaScript, kRequirable, "B");
  auto bytes = w.serialize(bundleGenerationTag());
  std::string error;
  EXPECT_FALSE(
      BundleReader::open(bytes.data(), bytes.size(), &error).has_value());
}
```

- [ ] **Step 2: Run it and watch it fail**

```bash
cmake --build cmake-build-asan --target BundleFormatTest && \
  cmake-build-asan/unittests/BundleFormatTest --gtest_filter='*Rejects*'
```

- [ ] **Step 3: Reject both**

Extend `isValidIdentity` in `bundle_reader.cpp` to reject an empty segment,
and add a duplicate check over the module table in `openImpl` -- the
identities are already being walked there for validation, so collect them
into a `std::unordered_set` and fail on a repeat, with
`"hermes-node bundle: duplicate module identity"`.

- [ ] **Step 4: Extract the shared fixture**

`unittests/TempTree.h` holds the one `mkdtemp` + write + recursive-remove
helper the five test files each copy today. Move the implementation there,
have all five use it, and delete the "mirrors the others" comments. Two of
the five are compile-cache tests unrelated to bundles; their behaviour must
not change.

- [ ] **Step 4b: Two test-hardening items deferred from Task 5**

`AgreesWithTheDiskBackend` asserts that the two backends return the SAME
answer, and both returning `nullopt` satisfies that -- agreement with no
content. Task 5 added seven specifiers to its matrix and verified by hand
that each resolves to a real file, but nothing in the test machine-checks
it, so a future fixture edit that stops a specifier resolving would silently
turn a real case into a vacuous one. Add a positive check that each
specifier expected to resolve actually does, so the matrix cannot decay into
asserting nothing. Leave the specifiers that are meant to miss (`missing`,
an outside-the-root absolute path) asserting a miss explicitly.

Second: the `resolve:` log line tightened in Task 5 reads
`[bundle] resolve: ./extra from cli.js -> extra.js` with no end anchor, so
it would substring-match a wrong resolution to `extra.json`. Add a `{{$}}`
anchor. The sibling `edge:` and `miss:` lines are unanchored by existing
convention, so anchor those two as well rather than leaving the file
inconsistent -- this is the same bug class that made `{{.*}}/foo` match
`.../foo/lib` earlier in this plan.

- [ ] **Step 5: Run the tests and commit**

```bash
cmake --build cmake-build-asan --target check-hermes-node
./utils/format.sh -f
git add lib/bundle unittests
git commit -m "bundle: reject a malformed container, and share the temp-tree fixture"
```

- [ ] **Step 6: Close the loop in the subsystem progress file**

Append to `history/plans/progress-aot-bundle.md`: what each of the fourteen
items changed, the container-size effect of Task 1 measured on the examples,
and the two items deliberately left (the `probeFile` guard, unreachable today
and therefore unpinned; the `mod.path` pin, whose collision is out of reach
now that `relativeResolveCache` is only written on the embedded path).

---

## Notes for the executor

- Task 1 is the only one that changes what a container holds. If a count
  moves anywhere else, that is a defect, not a new expectation.
- Task 6 depends on Task 5 step 5: land the `--include` dedup tests before
  the reader starts rejecting duplicates, or a regression surfaces as an
  unopenable bundle instead of a clear test failure.
- The invariant that has held through every round of this subsystem: a
  container is byte-for-byte identical with and without `--verbose`.
  `test/bundle-verbose.js` has a `cmp` for it. It must still hold.
