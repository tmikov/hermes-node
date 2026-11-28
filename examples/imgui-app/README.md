# imgui-app

A docked, multi-panel application written in plain JavaScript against
the `hermes-node:imgui` binding, in a `hermes-node-ui` window: 1280x800,
opened with `highDpi: true`. Its panels are backed by Node's `fs`, `os`
and `process` APIs, so it shows the binding's calling convention used for
a real program rather than a demo, and the event loop doing Node I/O while
the UI keeps drawing.

```
┌ File  View  Help ──────────────────────────────────────────────┐
│┌ Files ─────────┐┌ Widgets ──────────────┐┌ Monitor ──────────┐│
││ ▸ addons/      ││ [Button] ☑ Check      ││ rss   ▃▃▃▄▄▄▄▅    ││
││ ▾ examples/    ││ Slider ━━●━━━  0.42   ││ load  ▁▂▃▅▆▇▆▅    ││
││    demo.js     ││ Text  [hello_____]    ││ frame 16.6 ms     ││
││ ▸ test/        ││ Combo [dark     ▾]    │└───────────────────┘│
│└────────────────┘│ Color ■  Table ▦      │┌ Canvas ───────────┐│
│┌ Viewer ────────┐│ Tabs | Tree | Popup   ││  animated shapes  ││
││ text of the    │└───────────────────────┘│                   ││
││ selected file  │┌ Log ──────────────────┐│                   ││
│└────────────────┘│ 12:01:03 opened a.js  │└───────────────────┘│
└──────────────────┴───────────────────────┴─────────────────────┘
```

## Running it

It needs a build configured with `-DHERMES_NODE_ENABLE_UI=ON`, which builds
both `hermes-node-ui` and the addon beside it. From the project root:

```sh
<build dir>/bin/hermes-node-ui examples/imgui-app/app.js [folder]
```

The Files panel is rooted at `folder`, or at the current directory. It
runs until its window is closed or File > Quit. There is nothing to
install: no npm dependencies, and so no `package.json`.

## The panels

- **Shell** (`app.js`). A main menu bar and a dock space over the window.
  *File*: Open Folder (a modal with a text field), Quit. *View*: a toggle
  per panel (the window's close button clears the same toggle), Reset
  Layout (the default view again: every panel reopened, in the default
  layout), Dark / Light, ImGui Demo, ImGui Metrics. *Help*: About (a modal
  showing `imgui.version` and `process.versions`).
- **Files** (`panels/files.js`). A directory tree read lazily: a directory
  is listed with `fs.promises.readdir` the first time it is opened, and
  shows "loading" until the promise settles. Directories first, then by
  name; at most 5000 entries per directory, then a "... N more" row.
  A directory is listed once and not read again: collapsing and expanding
  it shows the same listing, and only File > Open Folder starts over.
  Symbolic links are shown and not followed into. A FIFO, a socket or a
  device is labelled as such (`pipe (fifo)`) and a click on it opens
  nothing. A directory that cannot
  be read shows its error code in the tree and in the log. The visible
  tree is one flat list of rows, rebuilt only when a directory opens,
  closes or finishes loading, and drawn through the list clipper, so a
  frame costs the rows on screen whatever the tree holds (a directory of
  5000 entries open costs about 150 more binding calls a frame than the
  default view; see "Tested"). A name with a control character in it is shown with
  `?` in its place, so every row stays one line tall, and a name holding
  `##` (which ImGui takes as the end of a label) is drawn as plain text
  beside its node, so it is shown whole. Each level is indented by the
  text line height, not by ImGui's `style.IndentSpacing`, since the binding
  has no style accessor: a cosmetic difference from ImGui's own trees.
- **Viewer** (`panels/viewer.js`). The selected file's first 1 MiB, read
  with `fs.promises.open` and 64 KiB `FileHandle.read` calls at explicit
  positions, never `readFile`; the handle is closed in a `finally`. Valid
  UTF-8 without a NUL byte is shown as text (lines over 4096 characters
  cut, with a marker), anything else as a hex dump, both drawn through
  the list clipper so a long file costs only its visible lines. A file
  longer than 1 MiB says so, and a character the cut split is dropped
  rather than shown as a replacement character. Only a regular file is
  opened, decided by `fs.promises.stat` of the path first (which follows
  a symbolic link): anything else, such as a link to a FIFO, is shown as
  "not a regular file", since opening a FIFO with no writer or reading a
  terminal would hold a threadpool thread for good. A path replaced by a
  FIFO between that stat and the open is not caught, and its open blocks
  as before; closing that window would need the open itself not to block
  (`O_NONBLOCK`) and a stat of the opened handle. Above it, the file's
  `stat` fields. A request superseded by a newer one stores nothing.
- **Widgets** (`panels/widgets.js`). A gallery in tabs: Basic (buttons,
  checkbox, radio buttons, sliders, drags, text and number inputs, a
  combo, colour editors, a progress bar, a list box); Table (a frozen
  header row over `os.cpus()` and `process.versions`, drawn through the
  list clipper); Trees (tree nodes and collapsing headers); Popups (a
  context menu, a modal, tooltips).
- **Monitor** (`panels/monitor.js`). `process.memoryUsage()`,
  `os.loadavg()`, ImGui's frame rate and the JavaScript time per frame,
  sampled four times a second by an unref'd `setInterval` timer into
  `Float32Array` rings, drawn with `plotLines` and `plotHistogram`. The
  heap figures are plotted only when the runtime reports them: this one
  reports `heapUsed` and `heapTotal` as 0, so the panel says so in a line
  instead of drawing two flat plots.
- **Canvas** (`panels/canvas.js`). An animation drawn with
  `imgui.drawList` into a child window, which is what clips it: an orbiting
  circle with a trail, an inner circle orbiting the other way, a rotating
  hexagon, a Bezier curve whose control points swing. A speed slider, a
  pause checkbox and the centre circle's colour. Every position is a
  function of the frame callback's timestamp, so the speed does not depend
  on the frame rate. Dragging in the canvas moves the centre
  (`invisibleButton`, `isItemActive`, `getMouseDragDelta`).
- **Log** (`panels/log.js`). The application's own events (a file opened,
  a read error, a layout reset), the last 1000, newest at the bottom. It
  follows the bottom unless you have scrolled up, with a Clear button and
  a filter field. A message is made one line as it is logged (a control
  character in a path becomes `?`, by the same helper the file tree uses,
  `display-text.js`), since the list clipper gives every entry one line's
  height.

## What is Node's and what is ImGui's

Everything the program knows comes from Node's APIs: the tree from
`fs.promises.readdir`, the file from `fs.promises.open` and `FileHandle`,
the classification from `require('buffer').isUtf8`, the figures from
`process.memoryUsage()`, `os.loadavg()`, `os.cpus()` and
`process.versions`, the clock from the frame callback's timestamp and
`process.hrtime.bigint()`. ImGui only draws: windows, docking, widgets, the
draw list, the list clipper. The two meet in one rule: **a promise's
continuation stores its result in application state, and the frame only
draws the state it finds and never awaits.** The window keeps drawing
while a directory listing or a file read is in flight.

Values a widget edits live in the binding's reference arrays (a
one-element `Array` for a scalar: `const check = [true];
imgui.checkbox('Check', check)`), so each call reads like its C++
counterpart and ImGui's own documentation applies. A panel's `open` is
such an array too, which is how the window's close button and the View
menu share one flag.

## The default layout

`layout.js` holds ImGui's own settings text for the six panels docked as
in the sketch above: the dock space split into three columns (380, 518
and 378 wide at 1280x800), each split into two. It is loaded with
`imgui.loadIniSettingsFromMemory()` before the first frame, and by View >
Reset Layout before the next frame's `newFrame()` (the call is refused
inside a frame). The panels are docked in the first frame and tile from
the second: docking lays out what was visible in the frame before.

It was produced as part B's Task 8 found works, and only text produced
this way should ever be loaded: ImGui does not validate this format, and
a malformed string can hang it or abort the process. To regenerate it:

1. Write the layout by hand in the format ImGui's handlers parse
   (`WindowSettingsHandler_ReadLine` and `DockSettingsHandler_ReadLine` in
   `imgui.cpp`): a `[Window][<name>]` section per panel with
   `DockId=0x<node>,0`, then `[Docking][Data]` with a `DockSpace
   ID=0x08BD597D Pos=0,19 Size=1280,781 Split=X` line and one indented
   `DockNode ID=0x<node> Parent=0x<parent> SizeRef=<w>,<h>` line per node
   (`Split=X` or `Y` on a node that is split, `CentralNode=1` on exactly
   one leaf). 0x08BD597D is the ID `dockSpaceOverViewport()` gives a zero
   argument for the main viewport; node IDs are free. `Pos=0,19` is the
   work area under the 19-unit main menu bar.
2. Put it in `layout.js` and run a script that creates the application
   with `createApp({root, clock: 'fixed', driven: true})`, adopts it with
   `harness.adopt(app)` (`test/fixtures/imgui/harness.js`), runs a few
   frames, prints `imgui.__test.state().windows` (each panel must be
   `docked`) and finally `JSON.stringify(imgui.saveIniSettingsToMemory())`:

   ```sh
   HERMES_NODE_UI_TEST_HOOKS=1 python3 utils/with-display.py 100 \
     <build dir>/bin/hermes-node-ui <script> test/fixtures/imgui/app-tree
   ```
3. Paste the saved string into `layout.js`, less the
   `[Window][Debug##Default]` section (ImGui's fallback window) and the
   `LastUsed=` lines (the date of the run). Run the script again: the saved
   string must come back identical.

## Tested

`test/ui-imgui-app.js` drives the application in its window through
`createApp({root, clock: 'fixed', driven: true})`: with `driven` it
registers no frame callback, prints an `APP opened`, `APP dir` or
`APP error` line per file or directory request, and records in
`app.rects` the rectangle of everything the last frame drew that a test
clicks (Files rows, menu items, Widgets tabs; nothing is recorded in
normal use). With the fixed clock the canvas is drawn at times the test
sets. The test clicks fixture files (names with a newline and with `##`
among them), opens a directory of 5001 entries and
compares one frame's binding calls with a 50-entry one, scrolls to the end
of the list and opens the last entry, reads pixels of the canvas, shrinks
the window to 1x1 and back, makes a panel throw from inside a list
clipper callback, and resets the layout with a panel closed.
`test/imgui-app-viewer.js` tests the viewer's reading and classification
without a window, under plain `hermes-node`; `panels/viewer.js` therefore
requires neither `hermes-node:ui` nor `hermes-node:imgui`.

`test/fixtures/imgui/app-measure.js` measures the application's
JavaScript time and binding calls per frame, driven, in two states: the
default view, and with a directory of 5000 entries open in Files. The
numbers, Debug and Release, are recorded in
`docs/superpowers/plans/progress-imgui-binding.md`. They include the two
calls per drawn row that recording `app.rects` costs in driven mode.

It has been run on Linux (GL, under Xvfb, framebuffer scale 1).

## Not a check

This example has no `run.sh`, so `check-hermes-node-examples` does not run
it.
