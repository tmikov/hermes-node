---
id: 01a0838f-9fa2-73fc-b1fd-4ebb97f8cecf
title: The napi public headers cannot be included from C
type: task
status: open
resolution: null
component: hermes
assignee: null
created: 2026-09-09T00:27:02.946Z
creator: Tzvetan Mikov <tmikov@gmail.com>
---

Split out of 01a07a4e-6e53, which bundled it with an unrelated compile-cache
scope decision. The finding is also wider than that issue recorded -- see the
correction below.

`hermes/API/napi/hermes_napi.h` and `hermes_napi_compile.h` declare structs
and then use the bare tag as a type:

    struct hermes_bytecode_flags { ... };
    ... const hermes_bytecode_flags *flags ...     // hermes_napi.h
    struct hermes_compile_flags { ... };
    ... const hermes_compile_flags *flags ...      // hermes_napi_compile.h

C requires either `struct hermes_bytecode_flags *` or a typedef. As written,
these headers are C++-only.

**Correction to how this was first recorded.** The original issue said "none
of these headers wrap their declarations in `extern "C"`". That is wrong:
`hermes_napi.h` does. What it does NOT do is make its text valid C, and the
distinction is the interesting part -- `extern "C"` controls name mangling so
a C program can LINK to these functions, and says nothing about whether a C
program can COMPILE the header that declares them. So the header set is
half-prepared for C consumption: the linkage half is done, the syntax half is
not, and nobody has noticed because nothing in-tree includes them from C.

Found while folding the Wasm cache API into `hermes_napi.h` (a fold done
because a per-function public header did not match that directory, where
`hermesNapi` has 21 sources and one public header). The new declarations
follow the existing convention rather than diverging from their neighbours;
fixing one declaration in isolation would make that file internally
inconsistent without making anything C-consumable.

The decision is directory-wide and should be made once: either add typedefs
(or `struct` keywords) plus `extern "C"` across the whole napi header set and
add a compile-as-C smoke test so it cannot rot, or state in the headers that
they are C++-only and drop the half-measure. Doing it per-header as each is
touched is how the set got into this state.

Note this is upstream-facing: these headers live in the `hermes` submodule,
so a change here travels with the branch rather than staying in
hermes-node-compat.

## Log

- 2026-09-09T00:27:02.946Z  Tzvetan Mikov <tmikov@gmail.com>  created
