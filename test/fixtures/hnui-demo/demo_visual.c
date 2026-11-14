/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/**
 * The reference addon's visual mode: a rotating triangle drawn with
 * `sokol_gl`, and a key that changes the clear colour.
 *
 * **This file is the worked example for `hnui.h`** (spec: The reference
 * addon, job 2). Everything a renderer needs is here but the version
 * check, which is the first line of the module init in `demo_addon.c`; the
 * test probes that have accumulated around this addon live in the other
 * files of this directory. Read top to bottom, it is the whole recipe:
 *
 *   1. At load, the addon's `NAPI_MODULE_INIT` calls `HNUI_CHECK_VERSION`
 *      (demo_addon.c).
 *   2. Once the window exists, register an event listener with
 *      `hnui_add_event_listener` (`startDemo`).
 *   3. On every frame -- driven from JavaScript's `requestAnimationFrame`
 *      (`drawDemoFrame`) or by `hnui_request_animation_frame` itself
 *      (`startNativeDemoFrames`) -- ask `hnui_frame_target` for the slot,
 *      begin a pass against its colour view, clear, draw, end.
 *
 * # The one thing `sokol_gl` has to be told
 *
 * `sgl_setup()` is given no pixel format and no sample count, so its default
 * pipeline takes both from the environment the HOST passed to `sg_setup`:
 * the surface colour format and one sample. The surfaces `hnui_frame_target`
 * hands out are made with exactly those, so the pipeline and the pass agree
 * by construction (spec: The native C API) -- and if they ever stopped
 * agreeing, Sokol's validation would reject the first `sgl_draw()` here,
 * which is why those two are left to the defaults.
 *
 * **The depth format is stated, and it has to be.** The host's environment
 * says depth NONE, as the spec requires, but `sokol_gl` enables depth
 * writes in its default pipeline and only turns them off when the depth
 * format it was GIVEN is `SG_PIXELFORMAT_NONE` (`_sgl_init_pipeline`). Left
 * at the default, the format resolves to NONE inside `sg_make_pipeline`
 * with depth writes still on, and validation rejects it:
 * `VALIDATE_PIPELINEDESC_DEPTH_FORMAT_NONE_BUT_DEPTH_WRITE_ENABLED`. So the
 * spec's "default pipelines in sokol_imgui and sokol_gl match with no
 * configuration" holds for colour format and sample count and not for
 * depth: every `sokol_gl` user on this host passes `depth_format = NONE`.
 *
 * # What this file never does
 *
 * `sg_setup`, `sg_shutdown` and `sg_commit` belong to the host. `sokol_gl`
 * needs a commit to know a frame ended, and gets one: it registers a commit
 * listener with the host's instance, and the host commits after the last
 * callback of every tick. `sgl_setup()` creates `sg_*` resources, so it runs
 * inside the first frame callback, where v1 says resource creation belongs
 * -- not at load, and never from an event listener. `sgl_shutdown()` is
 * never called: the demo runs until the process ends, and the host's
 * sokol_gfx instance, with every resource in it, ends with the process.
 * The host defines no teardown point yet for an addon that must give its
 * resources back earlier -- that is the pre-cleanup lifecycle hook, a later
 * task's (dz 01a0d88a).
 */

#include "demo_addon.h"

#include <hnui.h>
#include <node_api.h>
#include <sokol_gfx.h>

/* sokol_gl's implementation is compiled into this addon, against the
   sg_* symbols the host exports; the addon carries no sokol_gfx
   implementation of its own. */
#define SOKOL_GL_IMPL
#include <util/sokol_gl.h>

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

/**
 * The two clear colours a key toggles between. Every channel is 0 or 1,
 * which land on 0 and 255 under any float-to-unorm8 rounding rule a driver
 * might use -- that is what lets test/ui-demo-visual.js compare corner
 * samples exactly. Neither is the presenter's own `32/255` clear, so a
 * missing blit is legible.
 */
static const float kClearColors[2][3] = {
    {1.0f, 1.0f, 1.0f}, /* white */
    {0.0f, 0.0f, 1.0f}, /* blue */
};

static int g_clearIndex = 0;
static int g_listenerId = 0;
static bool g_sglReady = false;
static int g_framesDrawn = 0;

/* Test-only: test/ui-demo-visual.js fixes the angle so that a capture does
   not depend on when the frame was drawn. */
static bool g_rotationFrozen = false;
static float g_frozenAngle = 0.0f;

/* ------------------------------------------------------------------ */
/* Input                                                               */
/* ------------------------------------------------------------------ */

/**
 * The listener: any key press toggles the clear colour.
 *
 * `repeat` is past the first fields of `hnui_event`, so it is read only
 * when the host says it wrote it -- the size-prefix rule, which costs one
 * comparison and is what keeps this addon correct against a host built
 * from an older header.
 */
static void onEvent(hnui_window *w, void *user, const hnui_event *event) {
  bool repeat;

  (void)w;
  (void)user;
  if (event->type != HNUI_EVENT_KEY_DOWN) {
    return;
  }
  repeat = event->size >= HNUI_EVENT_MIN_SIZE && event->repeat != 0;
  if (!repeat) {
    g_clearIndex = 1 - g_clearIndex;
  }
}

/* ------------------------------------------------------------------ */
/* Drawing                                                             */
/* ------------------------------------------------------------------ */

/** sokol_gl reports through this; a panic means continuing would be
    undefined behaviour, so it ends the process. */
static void sglLog(
    const char *tag,
    uint32_t level,
    uint32_t item,
    const char *message,
    uint32_t line,
    const char *file,
    void *user) {
  static const char *const kLevels[] = {"panic", "error", "warning", "info"};
  (void)tag;
  (void)user;
  fprintf(
      stderr,
      "[sokol-gl:%s] item %u%s%s (%s:%u)\n",
      level < 4 ? kLevels[level] : "?",
      (unsigned)item,
      message != NULL ? ": " : "",
      message != NULL ? message : "",
      file != NULL ? file : "sokol_gl.h",
      (unsigned)line);
  if (level == 0) {
    abort();
  }
}

/** Once, inside the first frame callback: see this file's header. */
static void ensureSokolGl(void) {
  sgl_desc_t desc;
  if (g_sglReady) {
    return;
  }
  memset(&desc, 0, sizeof(desc));
  /* No color_format or sample_count: those defaults ARE the host's
     environment, and that is what is being checked. depth_format must be
     stated -- see this file's header. */
  desc.depth_format = SG_PIXELFORMAT_NONE;
  desc.logger.func = sglLog;
  sgl_setup(&desc);
  g_sglReady = true;
}

/**
 * Draw one frame into this frame's slot.
 *
 * \p timestampMs is the frame's timestamp, shared by every callback of the
 * tick; the angle is derived from it rather than counted per frame, so the
 * rotation speed does not depend on the frame rate.
 *
 * \return false outside a frame callback, when there is no slot to draw
 *   into -- not an error, and nothing is drawn.
 */
static bool drawFrame(hnui_window *w, double timestampMs) {
  hnui_target target;
  sg_pass pass;
  const float *clear = kClearColors[g_clearIndex];
  float angle;
  float radius;
  int i;

  memset(&target, 0, sizeof(target));
  target.size = (uint32_t)sizeof(target);
  /* Asking is what marks the frame drawn: see hnui_frame_target. */
  if (!hnui_frame_target(w, &target)) {
    return false;
  }
  ensureSokolGl();

  memset(&pass, 0, sizeof(pass));
  /* By value: hnui_gpu_view and sg_view share a layout, but a pointer cast
     between them would break C's effective-type rule. */
  pass.attachments.colors[0].id = target.color_view.id;
  pass.action.colors[0].load_action = SG_LOADACTION_CLEAR;
  pass.action.colors[0].store_action = SG_STOREACTION_STORE;
  pass.action.colors[0].clear_value.r = clear[0];
  pass.action.colors[0].clear_value.g = clear[1];
  pass.action.colors[0].clear_value.b = clear[2];
  pass.action.colors[0].clear_value.a = 1.0f;

  /* Sized from what the target reports THIS frame, in pixels. A resize
     needs no handling beyond that: the next frame's target is the new
     size. The radius keeps the triangle inside the inscribed circle with a
     margin, so it never reaches a corner. */
  angle = g_rotationFrozen ? g_frozenAngle
                           : (float)(timestampMs * 0.001 * 1.5707963);
  radius = 0.35f *
      (float)(target.width < target.height ? target.width : target.height);

  sgl_defaults();
  sgl_matrix_mode_projection();
  sgl_ortho(0.0f, (float)target.width, (float)target.height, 0.0f, -1.0f, 1.0f);
  sgl_matrix_mode_modelview();
  sgl_translate(0.5f * (float)target.width, 0.5f * (float)target.height, 0.0f);
  sgl_rotate(angle, 0.0f, 0.0f, 1.0f);
  sgl_begin_triangles();
  for (i = 0; i < 3; ++i) {
    float a = (float)i * 2.0943951f; /* 120 degrees */
    sgl_v2f_c3f(
        radius * sinf(a),
        -radius * cosf(a),
        i == 0 ? 1.0f : 0.0f,
        i == 1 ? 1.0f : 0.0f,
        i == 2 ? 1.0f : 0.0f);
  }
  sgl_end();

  sg_begin_pass(&pass);
  sgl_draw();
  sg_end_pass();
  /* No sg_commit: the host commits once the tick's last callback returns. */

  ++g_framesDrawn;
  return true;
}

/** The native frame-driving mode: re-registers itself every tick, which is
    how a native animation loop is written. */
static void nativeFrame(hnui_window *w, void *user, double timestampMs) {
  (void)user;
  drawFrame(w, timestampMs);
  hnui_request_animation_frame(w, nativeFrame, NULL);
}

/* ------------------------------------------------------------------ */
/* The JS surface                                                      */
/* ------------------------------------------------------------------ */

/** `startDemo()`: register the key listener. Idempotent. */
static napi_value startDemo(napi_env env, napi_callback_info info) {
  napi_value result;
  hnui_window *w = hnui_get_window(env);

  (void)info;
  if (w != NULL && g_listenerId == 0) {
    g_listenerId = hnui_add_event_listener(w, onEvent, NULL);
  }
  napi_get_boolean(env, g_listenerId != 0, &result);
  return result;
}

/** `drawDemoFrame(timestampMs)`, called from a requestAnimationFrame
    callback -> whether a frame was drawn. */
static napi_value drawDemoFrame(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_value result;
  double timestampMs = 0.0;
  hnui_window *w = hnui_get_window(env);
  bool drew = false;

  napi_get_cb_info(env, info, &argc, argv, NULL, NULL);
  if (argc >= 1) {
    napi_get_value_double(env, argv[0], &timestampMs);
  }
  if (w != NULL) {
    drew = drawFrame(w, timestampMs);
  }
  napi_get_boolean(env, drew, &result);
  return result;
}

/** `startNativeDemoFrames()`: hand the loop to the addon. */
static napi_value startNativeDemoFrames(napi_env env, napi_callback_info info) {
  napi_value result;
  hnui_window *w = hnui_get_window(env);
  int id = 0;

  (void)info;
  if (w != NULL) {
    id = hnui_request_animation_frame(w, nativeFrame, NULL);
  }
  napi_create_int32(env, id, &result);
  return result;
}

/** Test-only: `freezeRotation(radians)` fixes the triangle's angle. */
static napi_value freezeRotation(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  double angle = 0.0;

  napi_get_cb_info(env, info, &argc, argv, NULL, NULL);
  if (argc >= 1) {
    napi_get_value_double(env, argv[0], &angle);
  }
  g_rotationFrozen = true;
  g_frozenAngle = (float)angle;
  return NULL;
}

/** Test-only: how many frames this mode has drawn. Each drawn frame is one
    publication when nothing else draws, which is how a test names the
    publication it wants captured. */
static napi_value demoFramesDrawn(napi_env env, napi_callback_info info) {
  napi_value result;
  (void)info;
  napi_create_int32(env, g_framesDrawn, &result);
  return result;
}

void demoDefineVisualExports(napi_env env, napi_value exports) {
  napi_property_descriptor descs[] = {
      {"startDemo", NULL, startDemo, NULL, NULL, NULL, napi_default, NULL},
      {"drawDemoFrame",
       NULL,
       drawDemoFrame,
       NULL,
       NULL,
       NULL,
       napi_default,
       NULL},
      {"startNativeDemoFrames",
       NULL,
       startNativeDemoFrames,
       NULL,
       NULL,
       NULL,
       napi_default,
       NULL},
      {"freezeRotation",
       NULL,
       freezeRotation,
       NULL,
       NULL,
       NULL,
       napi_default,
       NULL},
      {"demoFramesDrawn",
       NULL,
       demoFramesDrawn,
       NULL,
       NULL,
       NULL,
       napi_default,
       NULL},
  };
  napi_define_properties(env, exports, sizeof(descs) / sizeof(descs[0]), descs);
}
