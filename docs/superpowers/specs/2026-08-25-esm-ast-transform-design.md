# Design: ESM as an AST transformation

**Status:** Draft for review, 2026-08-25.

This settles how `import`/`export` come to work in hermes-node: not by
porting Node's ESM loader, but by lowering a module's AST to CommonJS
inside the compile pipeline, using the Hermes parser and semantic
resolver as an AST plugin.

Everything measured below was measured against `cmake-build-asan` on
2026-08-25, not recalled.

## The problem

hermes-node cannot load an ES module. `hermes-node app.mjs` fails, and
`require()` of an ESM-only package fails, which is an increasing share
of npm.

The obvious route -- port Node's `lib/internal/modules/esm/*` -- is
closed. That tree is already vendored here (`loader.js`,
`module_job.js`, `translators.js`, `module_map.js`), and all of it sits
on `internalBinding('module_wrap')`, whose `ModuleWrap` is a wrapper
around V8's `SourceTextModule`: a parsed module that can be linked and
evaluated in separate phases. Hermes has no such object and no
equivalent. That is why `libjs/shims/internal/modules/esm/utils.js`
exists at all, and why its header says, in as many words, *"Since we
don't support ESM, this shim provides the subset needed by the CJS
loader."*

## What already exists, and it is more than expected

Resolution is not the gap. Node's real CJS loader
(`libjs-node/internal/modules/cjs/loader.js`, 2074 lines) and Node's
real ESM resolver (`libjs-node/internal/modules/esm/resolve.js`, 1052
lines) are both vendored and both live. Measured:

```
require('expkg')            -> CJS-CONDITION                  condition matching
require('expkg/sub')        -> SUB                            subpath exports
require('expkg/lib/cjs.js') -> ERR_PACKAGE_PATH_NOT_EXPORTED   encapsulation
```

`Module._resolveFilename(request, parent, isMain, options)` accepts
`options.conditions` (`cjs/loader.js:1337-1341`), and
`getCjsConditions()` / `getDefaultConditionsSet()` are wired
(`libjs/shims/internal/modules/helpers.js:166`,
`libjs/shims/internal/modules/esm/utils.js`).

So `exports` maps, conditions and encapsulation all work today. The
missing piece is precisely `ModuleWrap`, and this design replaces it
with a compile-time transformation.

## The decision

Lower ESM to CommonJS as an AST-to-AST transformation running inside
the Hermes compile pipeline, between parse and semantic resolution.

The alternative -- a real ESM loader in JavaScript, with separate link
and evaluate phases over a module registry -- is more faithful and much
larger, and its central benefit (asynchronous evaluation, and therefore
top-level await) is explicitly out of scope. Reusing the CJS loader
means module caching, cycle handling, resolution and `exports` maps are
already done and already tested.

Hermes's *own* `import`/`export` support is not a route either. It
exists (`ESTreeIRGen-stmt.cpp:1382`) but only under `setUseCJSModules`,
and lowers `import` to `require(source)` plus property loads: snapshot
bindings, no live bindings, `export class` an explicit "unsupported"
error, and the Metro `(this, exports, require, module)` signature
assumed throughout. It is the Metro bundler's module system, not ES
modules.

## What the Hermes front end gives, and the one thing it refuses

| Feature | Measured behaviour |
|---|---|
| `import` / `export` declarations | parsed |
| `import.meta` | parsed as `MetaPropertyNode`; rejected by *sema* in compile mode only |
| `import(...)` | parsed to `ImportExpressionNode` (`JSParserImpl.cpp:3508`); IRGen has no case, dies with "Invalid expression encountered" (`ESTreeIRGen-expr.cpp:234`) |
| top-level `await` | **not parsed at all** -- `parseProgram()` passes `Param{}`, so `paramAwait_` stays false (`JSParserImpl.cpp:355`) |

The first three are all rewritable by a transform that runs before
compile-mode resolution, so none of them needs a Hermes change.

Top-level await does, and is **out of scope**. The parse fails before a
plugin ever receives an AST, so nothing a transform can do reaches it;
and a module body lowered to something `require()` calls synchronously
cannot host an `await` regardless. Node's own answer for the same
situation is `ERR_REQUIRE_ASYNC_MODULE`. The cost we accept is the
diagnostic: a top-level `await` reports Hermes's raw `';' expected`,
which reads as a hermes-node bug rather than an unsupported feature.
Rewording it is worth doing and is not in this round.

## The plugin hook

Three changes in Hermes (n-api branch), each small.

**1. A transform callback on the compile entry.**
`BCProviderFromSrc::create` (`lib/BCGen/HBC/BCProviderFromSrc.cpp:91`)
runs the whole pipeline:

```
parse -> transformASTForCompilation -> sema::resolveAST -> generateIRFromESTree -> generateBytecodeModule
```

An optional transform parameter runs after `parser.parse()` and before
the existing `transformASTForCompilation`:

```cpp
if (astTransform) {
  {
    sema::SemContext analysisSemCtx{*context};
    if (!sema::resolveASTForParser(*context, analysisSemCtx, *parsed))
      return {nullptr, getErrorString()};
    parsed = cast_or_null<ProgramNode>(astTransform(*context, analysisSemCtx, *parsed));
    if (!parsed)
      return {nullptr, getErrorString()};
    sema::clearSemaResolution(*parsed);
  }  // analysisSemCtx dies here, after every pointer into it is nulled
}
```

`resolveASTForParser` (`Sema/SemResolve.h:295`) rather than
`resolveAST`, deliberately. `resolveAST` with `compile_ = true` mutates
the AST -- arrow concise body to block+return
(`SemanticResolver.cpp:254`), nested try/catch/finally (`:795`),
`$SHBuiltin` to `SHBuiltinNode` (`:1162`), `export default function` to
a `FunctionExpression` (`:1530`) -- and errors on features Hermes parses
but will not compile. `resolveASTForParser` does neither, so the plugin
receives the AST as written. Those mutations happen to be idempotent
under a second pass, but nothing should depend on that.

The contract: the transform may not retain anything reachable from the
analysis `SemContext` past its own return.

Three compile sites in this repo funnel through one function --
`compileAndRunCallback` (`lib/module-loader/module_loader.cpp`),
`compileFunctionForCJSLoaderCb` (`lib/bindings/node_contextify.cpp`)
and the bundle producer (`lib/bundle/bundle_build.cpp`) -- and each
calls `hermes_compile_to_bytecode`. A hook there therefore covers
scripts, `require()` and `--build-bundle` together. The AST-facing entry
cannot be C ABI, since the callback takes `Context &` and
`ESTree::ProgramNode *`, so a C++-only
`hermes_compile_to_bytecode_with_transform` sits beside the existing C
one. That is not a new kind of coupling: `lib/bundle/require_scanner.cpp`
already includes `hermes/Parser/JSParser.h` and `hermes/Sema/SemResolve.h`
directly and links `hermesvm_a`.

**2. `sema::clearSemaResolution(ESTree::Node *)`.** Resolution results
live in exactly three places on the AST
(`hermes/include/hermes/AST/ESTree.h`):

- `ScopeDecorationBase::scope_` (:266) -- `setScope` asserts not-already-set
- `FunctionLikeDecoration::semInfo_` and `strictness` (:287)
- `IdentifierDecoration::decl_`, `declState_`, `unresolvable_` (:451)

Everything else -- `Decl`, `LexicalScope`, `FunctionInfo`, the two side
`DenseMap`s -- lives in `SemContext` and is discarded by constructing a
fresh one. The helper is generated from `ESTree.def` with
`if constexpr (std::is_base_of_v<ScopeDecorationBase, T>)` so it covers
every node kind and cannot fall behind one added later. A copy in this
repo would rot silently the first time Hermes adds a node kind carrying
a scope, which is why it belongs in Hermes.

`lowerAST` (`lib/Sema/ASTLowering.cpp`) runs only in Flow-typed mode, so
untyped JavaScript has no type-directed lowering to undo.

**3. Gate the import-in-module-mode error on `compile_`.**
`SemanticResolver::visit(ImportDeclarationNode *)`
(`SemanticResolver.cpp:874`) reports *"'import' statement requires
module mode"* whenever `!astContext_.getUseCJSModules()`, unconditionally
-- so even the analysis pass rejects an `import`. Gating it on `compile_`
is the minimal fix and is safe: by the compile-mode pass the transform
has removed every import and export node, so that path is never reached.

The alternative, `setUseCJSModules(true)`, would enable Hermes's Metro
module semantics throughout IRGen for no benefit and is rejected.

## What the transform emits

New library `lib/esm/`, structured like `lib/bundle/`: it links the
Hermes front end, and nothing else in the tree needs to.

A module's `ProgramNode` becomes a single `FunctionExpression` built
**structurally**, not textually. Building it structurally rather than
prepending wrapper text has a second benefit: source positions are
exact, where the textual CJS wrapper has always offset the column of
everything on line 1.

### The wrapper parameters are hygienic names

In real ESM, none of `require`, `exports`, `module`, `__filename` or
`__dirname` are bound -- a `.mjs` in Node reports `require is not
defined` and `__dirname is not defined`. So the transform names every
wrapper parameter something it invents (`__esmReq$`, `__esmExp$`, ...)
rather than reusing the CommonJS names, and **binds the wrapper's
`require` to a name it owns**. Every helper call the transform emits
goes through that binding, never through the identifier `require`, and
that must hold for calls emitted deep inside nested functions as much as
for the ones at the top of the body.

Three things fall out of this at once, and the first is a real bug
avoided rather than a tidiness argument:

- `const require = createRequire(import.meta.url)` is one of the most
  common lines in real ESM code, and it shadows a wrapper parameter
  named `require` for the whole module scope. Helper calls emitted at
  the top of the body would hit its TDZ and every such module would
  throw before running a line. A name the user cannot write cannot be
  shadowed.
- That same `const require` then works, because the user's `require` is
  genuinely theirs.
- Bare `require`, `module.exports` and `__dirname` in a `.mjs` correctly
  become `ReferenceError`, matching Node, instead of silently working
  and letting a package ship code that runs only here.

### The body

```js
// 1. declared exports materialized, so a module in a cycle sees the names
Object.defineProperty(__esmExp$, "__esModule", { value: true });
__esmExp$.a = __esmExp$.b = void 0;

// 2. hoisted function exports, so a cycle can call them before the body runs
__esmExp$.f = f;

// 3. one load per distinct source
var _t1 = __esmReq$.esmImport("mod1.js", ["a", "b"]);
var _t1d = _t1 && _t1.__esModule ? _t1 : { default: _t1 };
```

Materializing the declared export names in step 1 is what makes the
missing-export check accurate: `'b' in _t1` then reflects what the
module *declares*, not what it has so far *assigned*, which is also what
makes the check correct mid-cycle when `require` hands back a
partially-populated `exports`.

Step 2 exists because ES modules hoist function declarations across a
cycle: a module can call an imported function before the exporting
module's body has run. The declaration itself is already hoisted by
JavaScript, so assigning it at the top of the body is enough.

`_t1d` is Babel's `_interopRequireDefault`. `import d from 'mod'` means
`module.exports` when `mod` is CommonJS and `.default` when it is ESM,
and which one it is cannot be known at transform time -- only the
`__esModule` marker distinguishes them at run time. This is the line
that makes `import d from 'lodash'` work against the real npm CommonJS
ecosystem, so it is load-bearing rather than ceremony.

### Reference rewriting

Driven by the analysis `SemContext`, by `Decl` and never by name -- the
same discipline `require_scanner.cpp` uses to identify `require` by
binding rather than by spelling, which is what makes it correct in the
presence of shadowing.

| Source | Emitted |
|---|---|
| read of an import binding | `_t1.a` |
| read of a default import | `_t1d.default` |
| **assignment** to an import binding | transform-time error, `Cannot assign to import 'a'` |
| **write** to an exported local | `__esmExp$.x = (x = v)`; reads stay local |
| `import.meta` | member access on a local from `__esmReq$.esmMeta()` |
| `import(x)` | `__esmReq$.esmDynamicImport(x)`, a Promise of the namespace |
| `export * from 'm'` | helper copying own enumerable keys except `default` |
| `export {x} from 'm'` | `__esmExp$.x = _tm.x` -- a snapshot, see below |

Rewriting reads of an import to a member access is what makes bindings
live **without getters**: every read is a fresh property load off the
exports object. On the export side the mirror image would be rewriting
reads of an exported local to `__esmExp$.x`, but writes are the only side
that has to be observable, so writes are synced and reads stay local
loads. That keeps the common path a plain local variable.

`export {x} from 'm'` is the one place the no-getters model gives
something up: the re-export is a snapshot rather than live. Making it
live is the only thing in this design that would require
`defineProperty` getters, and it is not worth them.

## The runtime helpers hang off `require`

`makeRequireFunction` lives in **our** shim
(`libjs/shims/internal/modules/helpers.js:45`), not in vendored Node, and
every construction site goes through it: `cjs/loader.js:1748`
(`_compile`), `:1942` (`createRequire`) and `libjs/bundle-loader.js:381`.
So attaching the helpers there costs one edit to a file we own, and the
disk world and the bundle's closed world both inherit them.

`makeRequireFunction(mod)` already closes over `mod`, so a helper has
the parent module without being handed it -- which is why the emitted
code above passes a specifier and a name list and nothing else.

The properties are non-enumerable, so `Object.keys(require)` is
unchanged and nothing feature-detecting `require` sees them.

`esmImport` resolves through
`Module._resolveFilename(spec, parent, false, {conditions: <import set>})`,
loads by the resolved absolute path, then checks the requested names
against the returned object and throws naming both modules when one is
missing.

Against a **CommonJS** target that check runs over the real `exports`
object, which is both more permissive and more accurate than Node: Node
statically lexes a CommonJS file with `cjs-module-lexer` to guess its
named exports, and guesses wrong on anything it cannot see
syntactically. We are looking at the object itself. The residual case is
a CommonJS module that assigns an export *after* its body returns, which
would be reported missing here; it is rare enough to accept and specific
enough to recognise in a bug report.

**The condition set is the point.** Lowering `import` to a bare
`require` would resolve under the `require` condition, and a package
whose `exports` has only an `import` key -- the standard shape of an
ESM-only package, which is the thing this design exists to run -- would
fail with `ERR_PACKAGE_PATH_NOT_EXPORTED`.

A global (`globalThis.__esm`, alongside the existing
`globalThis.__loadUserScript` and `__closeDiskModuleLoading`) was
considered and rejected once `require` was seen to work: it adds global
surface, carries a collision risk, and would still have needed the
parent module passed explicitly at every call site.

## Deciding that a file is ESM

Node's rule, unchanged: `.mjs` always, `.cjs` never, `.js` by the
nearest `package.json` `type`. `package_json_reader.js` and the `.mjs`
branch at `cjs/loader.js:1863` already have what is needed; what changes
is that `Module._extensions['.mjs']` compiles as ESM instead of raising
`ERR_REQUIRE_ESM`.

The decision is made in JavaScript and passed to
`compileFunctionForCJSLoaderCb` as a flag, which routes to the
transform-carrying compile entry and skips the textual CJS wrapper --
the transform builds the wrapper itself.

`require()` of an ESM module therefore works, and so does `import` of a
CommonJS one. That is more permissive than Node, which gates
`require(esm)`; the restriction exists there to keep an asynchronous
module out of a synchronous call, and we have no asynchronous modules,
so reimplementing it would buy nothing.

The plugin runs **only** on files the loader has already decided are
ESM. The consequence is deliberate and worth stating: `import()` in a
`.cjs` or a CommonJS `.js` keeps failing with Hermes's "Invalid
expression encountered", as it does today. Catching it there would mean
inspecting every compiled module, which needs either a per-module AST
walk on the hot compile path or a fourth Hermes change (a
`sawModuleSyntax` bit set by the lexer).

## Compile cache: a correctness bug to close

Entries are keyed by the module's absolute path and validated by a
CRC32 and byte length of the **source**. A `.js` file's kind depends on
a `package.json` that may be several directories up -- change `"type"`
from `commonjs` to `module` and the source CRC is unchanged, so the
cache serves CommonJS bytecode for a file that is now ESM. The module
kind has to be folded into the cache key.

## Bundle producer

`require_scanner.cpp` wraps every source in the CommonJS wrapper before
parsing, deliberately: a module body is a function body, so a top-level
`return` is only legal wrapped, and the wrapper's `require` parameter is
what lets the scanner identify `require` by binding. But a module body
inside a function makes `import` a syntax error, so ESM files need a
path that parses unwrapped and follows `ImportDeclaration`,
`ExportAllDeclaration` and `ExportNamedDeclaration` sources into the
same deduplicated specifier list.

A literal `import(x)` becomes a discovery edge exactly as
`require.resolve` is one; a computed `import(x)` joins the existing
"computed require()" warning, for the identical reason -- both reach the
run-time loader by the same route.

The producer's compile step needs the same kind decision the loader
makes, since it calls `hermes_compile_to_bytecode` directly
(`bundle_build.cpp:1372`) and must route an ESM file through the
transform-carrying entry. The kind is already derivable there -- the
producer resolves and reads `package.json` files during discovery, and
keeps the ones its resolution consulted.

The **run** layer needs nothing at all, and neither does the container
format. Lowering happens at compile time, so a bundle holds ordinary
lowered-CommonJS bytecode: no `kESModule` flag in the module record, no
format version bump, and `--bundle` never learns what ESM is.

### The risk this creates, stated plainly

`bundle_resolve.cpp` does not consult `package.json` `exports`; CLAUDE.md
already names that as the most likely source of a build-versus-run
resolution mismatch. ESM makes it worse in a specific way: ESM
resolution requires file extensions and selects the `import` condition,
and the producer's resolver does neither, while the run-time path
(through `Module._resolveFilename` with the import condition set) does
both. So a specifier resolving one way at build time and another at run
time is materially more likely under ESM than under CommonJS.

Teaching `resolveSpecifier` an ESM mode is real work and is scoped as
its own round. Until it is done, `--build-bundle` over ESM-heavy input
is not trustworthy, and that should be said rather than discovered.

## Divergences from specified ESM

All four follow from `require` being synchronous, and all four are
accepted:

- **No top-level await**, as above.
- **A cyclic read before the target has evaluated yields `undefined`**,
  where ESM throws a TDZ `ReferenceError`. The value is wrong in the
  same direction CommonJS is already wrong.
- **`import()` resolves on a microtask around a synchronous load.** The
  Promise is real; the loading is not actually asynchronous.
- **`export {x} from 'm'` is a snapshot**, not a live binding.

And one deliberate divergence that is not a consequence of anything:
missing-export errors fire **at the point the import is reached**, not
before any module evaluates. Real ESM links the whole graph first, so it
reports the error before a single side effect happens. Getting that
ordering needs a load phase separate from an evaluate phase, which is
exactly what `require()` fuses -- it is a structural cost, not a parsing
one, since every top-level import is loaded either way.

## Testing

- **Lit PASS-check tests** for observable semantics: live bindings,
  cycles, default and namespace interop, `export *`, missing-export
  errors, `import()`, assignment-to-import rejection, and the
  `createRequire` shadowing case that motivated hygienic parameters.
- **`EsmTransformTest`** (GTest) over the transform itself, AST in and
  AST out, VM-free the way `BundleFormatTest` is.
- **A real ESM-only npm package under `examples/`** as the end-to-end
  proof, in the shape `examples/tetris` proves `--build-exe`: a
  third-party package, a one-line entry, and a `run.sh` that checks the
  result rather than asserting it.

## Not doing

- Top-level await, and therefore asynchronous module evaluation.
- A pre-evaluation link phase, and therefore spec-ordered link errors.
- `import()` in CommonJS files.
- Live re-exports (`export {x} from 'm'`).
- An ESM mode for `bundle_resolve.cpp` -- its own round, and named above
  as the outstanding risk to `--build-bundle`.
- Node's `require(esm)` restrictions, which guard against a hazard this
  design does not have.
