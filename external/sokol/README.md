# sokol

Single-file C headers for windowing (`sokol_app.h`) and a cross-backend 3D
API wrapper (`sokol_gfx.h`). The UI compositor is built on them -- see
`docs/superpowers/specs/2026-09-18-ui-compositor-design.md`.

- Upstream: https://github.com/floooh/sokol
- License: zlib/libpng (`external/sokol/sokol/LICENSE`)
- Copyright: 2018 Andre Weissflog
- **Pinned at commit `c0db757`** (2026-09-13)

`sokol/` is untouched upstream source, per this repo's `external/$lib/$lib`
convention; this directory holds only the wrapper CMake and this file.

## Why only five files

Upstream is a collection of independent single-file headers, not a library
with a build. Vendoring the whole repository would bring in a dozen headers
nothing here includes (audio, fetch, time, debug UI, ...), each of which
would then be a file a future reader has to check for local edits. So the
vendored set is exactly what the compositor compiles against, and grows by
one file at a time as a task needs one.

| File | Why it is here |
|---|---|
| `sokol_gfx.h` | The 3D API wrapper. Compiled **twice** (see below). |
| `sokol_app.h` | Window and event loop; the main thread's. |
| `sokol_glue.h` | `sapp_*` -> `sg_*` translation for the presenting instance. |
| `sokol_log.h` | Reference logger. Used as the *shape* of ours, not called directly -- ours terminates on a panic item (spec: "Failure splits in two"). |
| `LICENSE` | Required by the license itself. |

## Re-syncing to a new pin

```bash
for f in LICENSE sokol_app.h sokol_gfx.h sokol_glue.h sokol_log.h; do
  curl -sS -o external/sokol/sokol/$f \
    https://raw.githubusercontent.com/floooh/sokol/<new-sha>/$f
done
```

Then, in this order:

1. Update the pin above, and `HNUI_SOKOL_COMMIT` wherever it is baked into
   the C API (spec: "Version check").
2. Re-run the rename generator's consumers: `sgp_rename.h` is generated from
   `sokol_gfx.h` at build time (`utils/gen-sokol-rename.py`), so a symbol
   added or removed upstream changes it with no edit here --
   `test/sokol-rename.js` asserts the count did not go *down* past the
   design's measured floor.
3. Rebuild. `lib/compositor-gpu/gl_backend.cpp` carries `static_assert`s
   tying `hnui_gpu_view` to Sokol's `sg_view` and the `hnui_target::
   color_format` range to `sg_pixel_format`; a bump that changes either
   fails the build rather than silently shipping an ABI struct that no
   longer matches (RULING 45).

## Digests as vendored

Checked in at these SHA-256 sums. Anything else means the tree was edited,
which the "untouched upstream" convention forbids -- carry a patch beside
this file instead (the design requires one, for a Metal command-buffer
accessor; it does not exist yet, and neither does the patch mechanism).

```
72f9367756dba68918b8955092c14503934681862eee33cc16495a02f40025ef  LICENSE
edc40eb40e16a6d79017c27de9d3e5fed5480a1f843618d3befebe172fc6c6e4  sokol_app.h
94719c2c6cf2b64ef2f60b14868c392b26c547de75e45ab3e3a57afbf0c799a1  sokol_gfx.h
6207d08f7ea5da43154f980c7fe314429eafb75922c6b41108ce2936f8240753  sokol_glue.h
a8d55de21694410740e752ea8e57c4103d5951d20339ba9712e24232adef9a42  sokol_log.h
```

## Two copies of `sokol_gfx`

`sokol_gfx` keeps one instance per process (`static _sg_state_t _sg`) and
every call must come from the thread owning that instance's GPU context.
The compositor needs two: the JS thread draws, the main thread presents. So
the implementation is compiled twice, the second time behind a generated
header of `#define sg_x sgp_x` lines. Nothing about that lives in this
directory -- it is entirely in `lib/compositor-gpu/` -- but it is the reason
`sokol_gfx.h` is included by more than one translation unit here, which
otherwise looks like a mistake.
