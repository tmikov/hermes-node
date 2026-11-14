# Linux hardware GL probe instructions

**Written 2026-09-21, for a run on an AMD GPU.** This closes the last two
questions gating
`docs/superpowers/specs/2026-09-18-ui-compositor-design.md`. Unlike the macOS
brief, the probe is **committed**: `test/fixtures/hnui-spike/glx_share.c`.
Check out `work-imgui` on the hardware machine and build it there.

The earlier Linux round (`docs/notes/2026-09-19-linux-spike-results.md`) ran
under **llvmpipe**, which answered both questions in ways that settle
nothing. The macOS round
(`docs/notes/2026-09-21-macos-spike-results.md`) is done and passed.

## The two questions

1. **Does the framebuffer config Sokol picks also advertise
   `GLX_PBUFFER_BIT`?** Sokol's filter asks for `GLX_WINDOW_BIT` and never
   for pbuffers (`sokol_app.h:12829`). llvmpipe's config advertises pbuffer
   support anyway, so the case that motivated patching Sokol's filter has
   never actually been exercised.
2. **Are GL sync objects visible across a share group on this driver?** The
   design's preferred completion path has the main thread poll
   `glClientWaitSync(fence, 0, 0)` once per frame callback, on a fence the
   worker thread created in a shared context. If that does not work here, the
   fallback -- the producer blocking on its own fence before publishing --
   ships instead. Both are already specified; this decides which is the
   default.

Question 2 is the one that matters. Question 1 only decides whether a
one-line Sokol patch is needed, and only if the surfaceless path also fails.

## Build and run

Packages (Ubuntu/Debian):

```
sudo apt-get install -y mesa-common-dev libgl-dev libglx-dev libx11-dev
```

Build and run **in a real X session on the AMD GPU** -- not Xvfb, not a
remote/indirect display, since the whole point is the driver's answers:

```
clang -O1 -g -Wall -o /tmp/glx_share test/fixtures/hnui-spike/glx_share.c \
      -lX11 -lGL -lpthread
/tmp/glx_share
```

(The binary goes to `/tmp` so the checkout stays clean; the directory also
carries a `.gitignore` in case you build in place.)

A window flashes briefly. The probe prints PASS/FAIL/NOTE lines and ends
with a block marked "COPY THIS BLOCK INTO THE RESULTS NOTE".

Useful context to capture alongside it: `glxinfo -B` output, the kernel
driver in use (`lspci -k | grep -A3 VGA`), and whether this is Mesa's
`radeonsi` or AMD's proprietary stack.

## What the probe reports

| Key | Meaning |
|---|---|
| `config_has_pbuffer` | Question 1. **0 is the interesting answer** -- it means llvmpipe was misleading and pbuffers are unavailable from Sokol's config. |
| `shared_created`, `direct` | A context sharing with the base one, made the way `init_cb` would have to. `direct: 0` would be a surprise worth reporting. |
| `surfaceless_ok` | Current on another thread with **no drawable**. If 1, question 1 is moot: no pbuffer, no patch. |
| `pbuffer_current_ok` | Only attempted if surfaceless failed. |
| `fence_poll_ok`, `polls`, `ms` | Question 2, measured the way the compositor does it: timeout-0 polling from the other thread, after ~200 full-screen clears of a 2048x2048 target so the fence is not already signalled. |
| `fence_blocking_ok` | The same fence with a blocking wait, for contrast. |
| `texture_visible`, `content_ok` | The worker's texture seen from the other context, and its **content** read back through an FBO -- name visibility is not content. |

## What each outcome forces

- **`surfaceless_ok = 1`** (expected): the design keeps route (b) and needs
  no Sokol patch, whatever `config_has_pbuffer` says.
- **`surfaceless_ok = 0`, `config_has_pbuffer = 1`**: route (b) is out,
  pbuffers work, still no patch.
- **`surfaceless_ok = 0`, `config_has_pbuffer = 0`**: this is the case the
  proposed patch exists for. The design's route (a) becomes real work: patch
  Sokol's config filter to require `GLX_PBUFFER_BIT`.
- **`fence_poll_ok = 1`**: the preferred GL completion path stands.
- **`fence_poll_ok = 0` but `fence_blocking_ok = 1`**: worth reporting
  precisely, because it is the awkward middle. The design would fall back to
  producer-side blocking rather than trying to make polling work.
- **`fence_poll_ok = 0` and `fence_blocking_ok = 0`**: sync objects are not
  shared here. Fallback ships, and the spec's claim that they are shared
  "on llvmpipe" stays exactly that narrow.
- **`content_ok = 0`** would be the serious one: it would mean the surface
  handoff itself does not survive the crossing on this driver, and the
  design's whole premise would need revisiting. Not expected.

## Reporting back

Commit a results note as `docs/notes/<date>-linux-hardware-spike-results.md`,
in the shape of the two existing ones: the machine, the pasted block, then a
short "what this forces" section. **Say AMD explicitly** -- one driver was
measured, and the spec should say which rather than claim "hardware".

## Out of scope

**EGL and GLES3.** Sokol can be forced to EGL on Linux and always uses it for
GLES3, and the spec has not decided whether v1 supports those. That decision
does not need a hardware session; keep this run to GLX so its two answers are
unambiguous.
