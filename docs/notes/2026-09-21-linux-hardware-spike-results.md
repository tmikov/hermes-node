# Linux hardware GL spike results for the UI compositor

**Run 2026-09-21.** The probe in
`docs/notes/2026-09-21-linux-hardware-spike-instructions.md`
(`test/fixtures/hnui-spike/glx_share.c`, as committed at `b834d58`),
answering the two GL questions the llvmpipe round
(`docs/notes/2026-09-19-linux-spike-results.md`) could not settle for
`docs/superpowers/specs/2026-09-18-ui-compositor-design.md`.

**Machine:** Ubuntu 24.04.4 LTS, kernel `7.0.0-28-generic`, **AMD Radeon
780M** (Phoenix3 APU, PCI `66:00.0`), kernel driver `amdgpu`, **Mesa
`radeonsi`** 25.2.8 (LLVM 20.1.2, DRM 3.64) -- the open-source stack, not
AMD's proprietary one. GL 4.6 core, direct rendering, `Accelerated: yes`.
Built with the system clang, `-O1 -g -Wall`.

**One deviation from the brief: the session is Wayland, not a native X
session.** `DISPLAY=:0` is **XWayland**, so GLX went through XWayland's GLX
(`GLX_VENDOR=Mesa Project and SGI`) onto the real GPU via DRI3. That is not
Xvfb and not an indirect display -- `direct: 1`, renderer string is
radeonsi -- so the driver-level answers (share groups, sync objects, texture
content) are radeonsi's own. What it cannot vouch for is **question 1 under
native Xorg**: the fbconfig list comes from the X server, and XWayland's
config set is not guaranteed to match Xorg's modesetting/amdgpu DDX.

**Verdict: every question passes. The design's preferred paths hold on AMD
hardware.**

## Output

```
INFO chosen config drawable_type=0x7 (WINDOW=1 PIXMAP=1 PBUFFER=1)
INFO question C: sokol's own config DOES advertise GLX_PBUFFER_BIT
INFO GL_VENDOR=AMD
INFO GL_RENDERER=AMD Radeon 780M Graphics (radeonsi, phoenix, LLVM 20.1.2, DRM 3.64, 7.0.0-28-generic)
INFO GL_VERSION=4.6 (Core Profile) Mesa 25.2.8-0ubuntu0.24.04.2
INFO GLX_VENDOR=Mesa Project and SGI
PASS question A-1: config id 0x104 resolved to a handle (1 match)
PASS question A-2: shared context created (direct)
INFO question C-2: a pbuffer WAS created from sokol's config
PASS question B-1: shared context current on another thread, NO drawable
INFO worker created texture 1 (gl error 0x0)
INFO worker inserted fence 0x75685c00ff30 after ~200 full-screen clears
PASS question D-1: timeout-0 polling from the main thread -> 0x911a after 94603 poll(s), 14.57 ms
PASS question D-2: blocking wait -> 0x911a
PASS question E-1: worker's texture 1 is visible here
PASS question E-2: read back 0x112233, expected 0x112233

==== COPY THIS BLOCK INTO THE RESULTS NOTE ====
vendor:            AMD
renderer:          AMD Radeon 780M Graphics (radeonsi, phoenix, LLVM 20.1.2, DRM 3.64, 7.0.0-28-generic)
gl_version:        4.6 (Core Profile) Mesa 25.2.8-0ubuntu0.24.04.2
config_has_pbuffer: 1   (C: 1 = llvmpipe-like, 0 = the case that matters)
shared_created:    1   direct: 1
surfaceless_ok:    1   pbuffer_current_ok: -1
fence_poll_ok:     1   polls: 94603   ms: 14.57
fence_blocking_ok: 1
texture_visible:   1   content_ok: 1
==============================================
```

Exit status 0.

## Reading it

- **Question 2, sync objects across the share group -- yes.** Unlike the
  llvmpipe round, the fence was genuinely unsignalled when polling began:
  94,603 timeout-0 polls over 14.57 ms before `GL_CONDITION_SATISFIED`
  (`0x911a`). So this measures a cross-context fence *transitioning*, which
  the llvmpipe run (`GL_ALREADY_SIGNALED`) never did.
- **Question 1, pbuffer bit -- still advertised.** Sokol's config is
  `drawable_type=0x7` here as it was on llvmpipe, and a pbuffer was actually
  created from it. Moot anyway: `surfaceless_ok = 1`, so the pbuffer path was
  never needed (`pbuffer_current_ok: -1` means not attempted).
- **Content crosses the context boundary.** The worker's texture read back
  `0x112233` through an FBO in the other context -- content, not just the
  name.

## What this forces

- **Route (b), surfaceless, stands. No Sokol config-filter patch** on this
  driver.
- **The preferred GL completion path stands:** the main thread polls
  `glClientWaitSync(fence, 0, 0)` per frame callback on a fence the worker
  created. Producer-side blocking remains the specified fallback but is not
  the default.
- **The spec should name the driver**: sync-object sharing is now measured
  on llvmpipe and on **AMD radeonsi (Mesa 25.2.8)**, not "hardware" in
  general.

## Still open

- **NVIDIA's proprietary driver**, which is the one most likely to differ on
  both questions. Not measured.
- **Question 1 under native Xorg** rather than XWayland (see above). Low
  stakes, since surfaceless works and makes the pbuffer bit irrelevant.
