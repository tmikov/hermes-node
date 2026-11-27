# imgui-demo

Dear ImGui's own demo window (`ImGui::ShowDemoWindow()`), drawn from a
JavaScript program by the `hermes-node:imgui` addon in a `hermes-node-ui`
window: 1280x800, high-DPI, docking on. Every widget in it is live --
mouse, keyboard, text input, the clipboard and the mouse cursor all go
through the addon.

`demo.js` is the whole program: one frame callback that calls
`imgui.newFrame()`, `imgui.showDemoWindow()` and `imgui.render()`, and
schedules the next.

## Running it

It needs a build configured with `-DHERMES_NODE_ENABLE_UI=ON`, which builds
both `hermes-node-ui` and the addon beside it (`cmake-build-asan` is
configured that way). From the project root:

```sh
<build dir>/bin/hermes-node-ui examples/imgui-demo/demo.js
```

It runs until its window is closed. There is nothing to install: the
example has no npm dependencies, and so no `package.json`.

## Not a check

This example has no `run.sh`, so `check-hermes-node-examples` does not run
it; that target runs against a Release build, which has the UI off. The
same three calls are exercised under test by `test/ui-imgui-demo.js`, part
of the default lit suite in a build with the UI on.
