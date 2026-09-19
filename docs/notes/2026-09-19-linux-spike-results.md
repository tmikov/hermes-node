# Linux spike results for the UI compositor

**Run 2026-09-19.** Go/no-go probes for
`docs/superpowers/specs/2026-09-18-ui-compositor-design.md`. Throwaway code,
kept outside the repo; what matters is the answers.

**Machine:** Ubuntu 24.04.4, clang, Mesa 25.2.8, **llvmpipe** under Xvfb
(`xvfb-run -a -s "-screen 0 1280x800x24"`), GL 4.5 core, direct rendering.
Sokol at `c0db757`. **No hardware GPU was tested**, which limits two of the
answers below.

**Verdict: no blocker found. The design's preferred paths work; one new
constraint fell out, and two questions stay open until a real GPU and a Mac
are available.**

## 1. GLX context sharing -- PASS on every question

Probe: create a context and window the way `sokol_app` does, then do what our
`init_cb` would have to do.

| Question | Result |
|---|---|
| Recover the config from the current context (`glXQueryContext(GLX_FBCONFIG_ID)`) and resolve the id back to a `GLXFBConfig` handle | **PASS**, 1 match |
| Create a context sharing with `sokol_app`'s, given only the display and `glXGetCurrentContext()` | **PASS**, and the result is a *direct* context |
| Make it current **on another thread with no drawable at all** | **PASS** |
| A fence (`glFenceSync`) created by the worker, waited on from the other context | **PASS** -- `GL_ALREADY_SIGNALED` |
| The worker's texture visible in the other context | **PASS** |

Two consequences for the design:

- **The surfaceless route (b) works, so no pbuffer and no Sokol patch is
  needed** for context sharing on this driver.
- **The preferred GL completion path is available**: sync objects *are*
  shared within the share group, so the main thread can poll
  `glClientWaitSync` with timeout 0 rather than falling back to blocking the
  producer.

**Caveat, and it is a real one:** llvmpipe's chosen config reports
`drawable_type=0x7`, i.e. it advertises `GLX_PBUFFER_BIT` alongside
`GLX_WINDOW_BIT`. So this machine cannot exercise the case that motivated the
proposed patch to Sokol's config filter, and it says nothing about whether
NVIDIA's or AMD's drivers share sync objects. **Both answers must be
re-measured on hardware before the patch route is declared unnecessary.**

## 2. Xlib threading -- rule confirmed, no probe needed

`XInitThreads()` runs inside `_sapp_linux_run` (`sokol_app.h:14253`), a few
statements in, so it is not in effect while our JS thread is already running.
Xlib documents `XInitThreads` as needing to be the first Xlib call. The
design's rule stands as written: **no thread may touch Xlib before `init_cb`
signals**, and our JS thread does not -- it only posts a window request. The
probe itself called `XInitThreads()` first and saw no threading trouble.

## 3. Two `sokol_gfx` instances in one process -- PASS

- Compiling the implementation twice, the second time behind a generated
  header of 156 `#define sg_x sgp_x` lines: **156 `sg_*` symbols in one
  object, 156 `sgp_*` in the other, zero overlap**, with the **real GL
  backend** (`SOKOL_GLCORE`), not the dummy one.
- **The C++ inline-overload rule is confirmed and handled.** A C++
  translation unit that includes the rename header before `sokol_gfx.h`
  resolves `sg_setup(const sg_desc&)` to `sgp_setup`; the same file without
  the rename header resolves it to `sg_setup`. Including the rename after the
  declarations would therefore bind the overloads to the wrong copy, exactly
  as the review predicted.
- **Both copies link together under `-flto`** and the result runs.

Not covered here: Metal, and both instances actually *running* at once on two
threads. That is probe 2 in the macOS brief.

## 4. Offscreen-only frames and injected textures -- one new constraint

| Question | Result |
|---|---|
| 2000 frames of offscreen-only passes, never a swapchain pass | **PASS**, zero asserts, zero error logs, on both texture flavours |
| Render target wrapping a **single** injected texture (not `SG_NUM_INFLIGHT_FRAMES` of them) | **PASS**, state VALID |
| Sampling an injected-texture image while rendering into another | **FAIL for `glTexImage2D` textures, PASS for `glTexStorage2D` textures** |

**The new constraint: surface textures must use immutable storage.** Sokol
sets `gl_texture_views` for any GL >= 4.3 (`sokol_gfx.h:10505`), and its
texture-view path then calls `glTextureView()`
(`_sg_gl_create_view`, `sokol_gfx.h:11900`), which requires an immutable
source texture. A texture created with `glTexImage2D` trips
`SOKOL_ASSERT(glGetError() == GL_NO_ERROR)` at `sokol_gfx.h:11944`.

This does **not** bite the ordinary path: sokol allocates its own images with
`glTexStorage2D`/`glTexStorage3D` (`sokol_gfx.h:11217-11236`), so surfaces
sokol creates are already immutable. It bites anything that injects a
hand-made texture, which is the presenting side's wrapper. The design now
states the rule.

Note also that the first version of this probe aborted for its own reasons:
it made raw GL calls after `sg_setup()` without draining GL errors and
without `sg_reset_state_cache()`, which Sokol's documentation requires. That
is a rule the reference addon and its documentation should state, since every
addon mixing raw GL with `sokol_gfx` faces it.

## 5. llvmpipe under Xvfb -- PASS

`glxinfo` reports llvmpipe with max core profile 4.5, and every probe above
ran under it. Software rendering is viable for the lit tests, as the test
plan assumed.

## Still open

- **Everything Metal.** Command-buffer completion ordering is the load-bearing
  one: both the producer's publication marker and the consumer's read-release
  depend on it. See `docs/notes/2026-09-19-macos-metal-spike-instructions.md`.
- **Hardware GL.** The pbuffer question and cross-context sync visibility were
  measured on llvmpipe only.
