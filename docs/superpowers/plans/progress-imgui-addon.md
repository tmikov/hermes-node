# Implementation Progress

Tracks progress on `docs/superpowers/plans/2026-11-27-imgui-addon-part-a.md`
(implementation plan) and its companion design doc
`docs/superpowers/specs/2026-11-27-imgui-addon-design.md`.

## Status

| Task | Description | Status |
|------|-------------|--------|
| Task 1 | Vendor Dear ImGui | done |

## Mutations

Each test must be shown able to fail: the change made, and what it did.

| Task | Test | Mutation | Result |
|------|------|----------|--------|
| 1 | `test/imgui-pin.js` | README tag changed to `v1.92.0-docking` | `README WRONG README=v1.92.0-docking HEADER=v1.93.0-docking`, test failed |

## Notes

### Task 1

- Vendored from a fresh `--depth 1` clone of tag `v1.93.0-docking`; the
  twelve files verified byte-identical with `diff`.
