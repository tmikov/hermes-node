# Implementation Progress

Tracks progress on `docs/superpowers/plans/2026-11-27-imgui-addon-part-a.md`
(implementation plan) and its companion design doc
`docs/superpowers/specs/2026-11-27-imgui-addon-design.md`.

## Status

| Task | Description | Status |
|------|-------------|--------|
| Task 1 | Vendor Dear ImGui | done |
| Task 2 | Extract loadNativeAddon() from process.dlopen | done |
| Task 3 | Load a named addon; the addon skeleton | done |
| Task 4 | The frame protocol, and a cleared target | in progress (Step 0 done) |

## Mutations

Each test must be shown able to fail: the change made, and what it did.

| Task | Test | Mutation | Result |
|------|------|----------|--------|
| 1 | `test/imgui-pin.js` | README tag changed to `v1.92.0-docking` | `README WRONG README=v1.92.0-docking HEADER=v1.93.0-docking`, test failed |
| 2 | the 14 dlopen/addon tests (`test-native-addon.js`, `bundle-natives.js`, ...) | `loadNativeAddon` looks up `napi_register_module_v1_x` instead | 6 of 14 failed (`test-native-addon`, `test-native-addon-pkg`, `test-cli-eval-require`, `bundle-natives`, `bundle-natives-errors`, `build-exe-natives`); restored |
| 3 | `test/imgui-missing.js` | `stat()` check in `named_addons.cpp` replaced by `if (false)` | the missing file reached dlopen: `MISSING ERR_DLOPEN_FAILED path=true`, test failed; restored |
| 3 | `test/ui-imgui-feature.js`, `test/imgui-load.js` | `registerNamedAddons()` call in `main.cpp` disabled | `ui-imgui-feature.js` FAIL (`agree binary=off lit=off` fails ON), `imgui-load.js` UNSUPPORTED; restored |
| 3 | `test/imgui-symbols.js` | the three visibility properties removed from `addons/imgui/CMakeLists.txt` (before the export list existed; with it, this alone no longer fails) | `EXPORTS bad: 2562 unexpected (2506 ImGui)`, test failed; restored |
| 3 | `test/imgui-load.js` | init trace line emitted twice | ERR run failed on its `-NOT` line; restored |
| 3 | `test/imgui-symbols.js` | with a temporary `std::vector<std::string>` in the init: export list kept | PASS, 2 exports |
| 3 | `test/imgui-symbols.js` | same `std::vector` probe, `--version-script` option dropped | `EXPORTS bad: 16 unexpected`, test failed; restored |
| 3 | `test/imgui-symbols.js` | export list and the three visibility properties both dropped | `EXPORTS bad: 2562 unexpected (2506 ImGui)`, test failed; restored |
| 4 | `test/ui-present.js` | give-up branch of `maybeCapture()` sets `publicationId` to 0 again | `REBLIT matched=false framesObserved=240 presenting=WRONG got=0 want=1`, test failed; restored |

## Notes

### Task 1

- Vendored from a fresh `--depth 1` clone of tag `v1.93.0-docking`; the
  twelve files verified byte-identical with `diff`.

### Task 3

- `HNUI_CHECK_VERSION(env)` is the first statement of the addon's
  `NAPI_MODULE_INIT()` (checked by reading, Step 7(c)). No test reaches a
  mismatched host with this addon: the addon and the host are built from
  the same `hnui.h` in one tree, and the mismatched-addon case is
  `test/ui-version-check.js`'s, against `hnui_bad_version_addon`.
- libstdc++ declares namespace `std` with default visibility, so a
  `std::` template instantiated in the addon (a `std::vector<char>` in the
  first draft of `trace`) is exported as a weak symbol despite
  `-fvisibility=hidden`; `test/imgui-symbols.js` caught ten of them. Ruled:
  an explicit export list on top of the visibility properties
  (`addons/imgui/exports.map` as an ELF version script,
  `exports.darwin.txt` as ld64's `-exported_symbols_list`, both a
  `LINK_DEPENDS`), so the addon's code may use std containers freely. The
  macOS half has not been linked anywhere yet.
- `test/imgui-missing.js` hard-links the `hermes-node-ui` binary into
  `%t.dir` (copying it, about 190 MB under ASAN, only when the link fails);
  `/proc/self/exe` reports the link's own path, so the executable's
  directory is `%t.dir`. A symlink would not do: it is realpath'd back to
  `bin/`.
- The UI-off configuration (`cmake-build-asan-noui`) still configures and
  builds `hermes-node`.
