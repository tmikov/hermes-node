# hnui-spike: hardware probe for the UI compositor design

Throwaway diagnostic code, committed because it has to be run on a machine
with a real GPU -- check the branch out there and build it. It is **not**
built by CMake and is not part of any test suite.

## Run it

In a **real X session on an AMD GPU** -- not Xvfb, not a remote or indirect
display. The whole point is what the driver answers.

```
sudo apt-get install -y mesa-common-dev libgl-dev libglx-dev libx11-dev
clang -O1 -g -Wall -o /tmp/glx_share glx_share.c -lX11 -lGL -lpthread
/tmp/glx_share
```

A window flashes briefly. Output ends with a block marked
`COPY THIS BLOCK INTO THE RESULTS NOTE`.

## Then what

Commit a results note at
`docs/notes/<date>-linux-hardware-spike-results.md`: the machine, that pasted
block, and anything surprising. Say **AMD** explicitly -- one driver is being
measured, and the design should record which rather than claim "hardware".
Useful extras: `glxinfo -B`, and `lspci -k | grep -A3 VGA` for the kernel
driver.

## Why it exists

Two questions gate
`docs/superpowers/specs/2026-09-18-ui-compositor-design.md` and software
rendering cannot settle either: whether the framebuffer config Sokol picks
also supports pbuffers, and whether GL sync objects are visible across a
share group. The second decides whether the compositor's main thread can poll
a fence the render thread created, or has to fall back to blocking the render
thread instead.

**The full brief, including what each outcome forces, is
`docs/notes/2026-09-21-linux-hardware-spike-instructions.md`.** Read that
before drawing conclusions from the output; this file is only how to run it.
