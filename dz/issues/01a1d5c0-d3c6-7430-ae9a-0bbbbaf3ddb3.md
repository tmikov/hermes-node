---
id: 01a1d5c0-d3c6-7430-ae9a-0bbbbaf3ddb3
title: "Frame-callback and event-listener ids are int and never reused: signed
  overflow after 2^31"
type: bug
status: open
resolution: null
component: null
assignee: null
created: 2026-11-13T16:32:26.566Z
creator: Tzvetan Mikov <tmikov@gmail.com>
---

`hnui_request_animation_frame`, `requestAnimationFrame` (one id space with
it, Ruling 57) and `hnui_add_event_listener` hand out `int` ids from
counters that only ever increase, and 0 is reserved for "no id". After
2^31 - 1 registrations the next increment is signed overflow -- undefined
behaviour in C and C++ -- and in practice wraps to negative ids, then
through 0, which callers are told means failure.

2^31 is reachable by a program that registers one frame callback per frame
(the ordinary rAF loop) only after about a year at 60 fps, and faster with
several callbacks per frame; a long-running kiosk or dashboard is the
plausible victim. Fixing it means either a wider counter behind the same
`int` ABI (wrapping deliberately and skipping 0 and live ids) or widening
the ABI before the first release. Found by the Slice C whole-slice review.

## Log

- 2026-11-13T16:32:26.566Z  Tzvetan Mikov <tmikov@gmail.com>  created
