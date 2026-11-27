# imgui

Dear ImGui, an immediate-mode GUI library. The `hermes-node:imgui` addon is
built on it -- see `docs/superpowers/specs/2026-11-27-imgui-addon-design.md`.

- Upstream: https://github.com/ocornut/imgui
- License: MIT (`external/imgui/imgui/LICENSE.txt`)
- Copyright: 2014-2026 Omar Cornut
- **Pinned at tag `v1.93.0-docking`**

`imgui/` is untouched upstream source, per this repo's `external/$lib/$lib`
convention; this directory holds only the wrapper CMake and this file.
Configuration goes through a header of ours (`addons/imgui/imgui_config.h`,
named by `IMGUI_USER_CONFIG`), never through edits to `imconfig.h`.

## Why the docking branch

Docking is on (`ImGuiConfigFlags_DockingEnable`): a complex UI is the goal,
and dockable panels are most of what makes ImGui UIs look like applications.
Multi-viewport stays off. The pin is the newest `v<version>-docking` release
tag at the time the work started.

## Vendored files

The core sources and headers at the repository root, and nothing else.

| File | Why it is here |
|---|---|
| `imgui.cpp`, `imgui.h` | The core library and its public API. |
| `imgui_internal.h` | Included by the core sources. The addon does not include it. |
| `imgui_draw.cpp` | Draw lists, fonts, the built-in font atlas. |
| `imgui_tables.cpp` | Tables. |
| `imgui_widgets.cpp` | The standard widgets. |
| `imgui_demo.cpp` | The demo window, the addon's first deliverable. |
| `imconfig.h` | Included by `imgui.h`; left stock, ours overrides it through `IMGUI_USER_CONFIG`. |
| `imstb_rectpack.h`, `imstb_textedit.h`, `imstb_truetype.h` | The stb headers the core sources include. |
| `LICENSE.txt` | Required by the license itself. |

Not vendored: `backends/` (the addon has its own platform and renderer
halves on `hnui` and `sokol_gfx`), `examples/`, `misc/` and `docs/`.

## Re-syncing to a new pin

```bash
d=$(mktemp -d)
git clone --depth 1 --branch v<new>-docking https://github.com/ocornut/imgui $d/imgui
for f in imgui.cpp imgui.h imgui_demo.cpp imgui_draw.cpp imgui_internal.h \
         imgui_tables.cpp imgui_widgets.cpp imconfig.h imstb_rectpack.h \
         imstb_textedit.h imstb_truetype.h LICENSE.txt; do
  cp $d/imgui/$f external/imgui/imgui/$f
done
```

Then update the pinned-tag line above (`external/imgui/CMakeLists.txt` reads
`IMGUI_VERSION_PINNED` from it, and re-runs configure when it changes) and
run `test/imgui-pin.js`, which fails if the README's tag and the header's
`IMGUI_VERSION` disagree or the header is not the docking branch's. Check
that `git status` shows nothing outside `external/imgui/` and that the
addon still builds: renamed or removed API fails there, not here.
