---
id: 01a1fbb5-84af-7fbf-b71d-bee14e2cbcd0
title: Fullscreen on macOS is untested
type: task
status: open
resolution: null
component: null
assignee: null
created: 2026-11-21T01:25:39.631Z
creator: Tzvetan Mikov <tmikov@gmail.com>
---

`win.setFullscreen()` reaches `sapp_toggle_fullscreen()`, which on macOS is
`[window toggleFullScreen:]`: an animated transition into a new Space.
Nothing in the suite issues it on macOS.

`test/ui-commands.js` toggles fullscreen twice in one frame. On X11 that is
two property changes, and the test asserts `sapp_is_fullscreen()` after
each (`TRACE-gl`). On Metal the script does not issue the pair at all (it
takes the backend name as `argv[2]`), because what AppKit does with a
second toggle requested during the first one's animation is unknown and was
deliberately not measured in Task 16.

So on macOS nothing shows that a toggle takes effect, that
`win.fullscreen` in the mirrored window state follows it, that the
resulting resize starts a new surface generation, or what a second toggle
during the animation does.

A test would need one toggle, a wait for AppKit's did-enter-fullscreen
notification (the in-process driver, `mac_test_driver.mm`, already answers
`miniaturize` that way), a read-back, and the same for leaving. It would
also move the test machine's window into a new Space while it runs, which
is worth weighing before adding it to a suite that otherwise avoids taking
the screen from whoever is at the Mac.

## Log

- 2026-11-21T01:25:39.631Z  Tzvetan Mikov <tmikov@gmail.com>  created
