# imgui-demo

Dear ImGui's own demo window (`ImGui::ShowDemoWindow()`), drawn from a
JavaScript program by the `hermes-node:imgui` addon in a `hermes-node-ui`
window: 1280x800, opened with `highDpi: true`, docking on. Mouse, wheel,
keyboard and text input, the clipboard and the mouse cursor all reach
ImGui through the addon. The tests exercise each of those paths on the
addon's own test scenes (`addons/imgui/README.md`); on the demo window
itself they check that it draws and that a wheel event over it reaches
ImGui.

It has been run on Linux (GL backend, under Xvfb, framebuffer scale 1)
and on an Apple-silicon Mac (Metal, a 2x display), by the tests. Nobody
has looked at the window on the Mac.

`demo.js` is the whole program: one frame callback that calls
`imgui.newFrame()`, `imgui.showDemoWindow()` and `imgui.render()`, and
schedules the next.

## Running it

It needs a build configured with `-DHERMES_NODE_ENABLE_UI=ON`, which builds
both `hermes-node-ui` and the addon beside it. From the project root:

```sh
<build dir>/bin/hermes-node-ui examples/imgui-demo/demo.js
```

It runs until its window is closed. There is nothing to install: the
example has no npm dependencies, and so no `package.json`.

## Not a check

This example has no `run.sh`, so `check-hermes-node-examples` does not run
it. `test/ui-imgui-demo.js`, part of the default lit suite in a build with
the UI on, makes the same three calls through the test harness, in a
window opened without `highDpi`; it does not run this file.
