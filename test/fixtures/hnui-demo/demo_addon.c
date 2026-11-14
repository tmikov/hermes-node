/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/**
 * The reference addon for hermes-node's UI compositor.
 *
 * **Part of the deliverable, not a sample written afterwards** (spec: The
 * reference addon). Without it the first thing anyone rendered would have
 * to be written from scratch against an API that had never had a consumer,
 * and an API with no consumer is usually subtly unusable. It is written
 * *during* implementation for exactly that reason: if the demo is awkward
 * to write, the API is wrong and changes while it still can.
 *
 * **Plain C**, because `hnui_abi.h` is plain C and is pinned as such by a
 * C99 `-pedantic` probe translation unit (`unittests/HnuiAbiCProbe.c`).
 * An addon written in C++ would never exercise that claim.
 *
 * **It draws a FIXED pattern, not a rotating triangle.** The spec's demo
 * rotates; the spec's *tests* say "What draws in these tests is the
 * reference addon, in a mode that renders a fixed pattern rather than a
 * rotating one", and this task's gate is a picture. A rotating triangle
 * would make the captured pixels depend on elapsed time, which is the one
 * thing a capture keyed to a publication id must not depend on. The
 * rotating mode belongs with Task 14's fuller demo; what exists here is
 * the mode the tests need and a person can still look at.
 *
 * # The pattern
 *
 *   - the whole surface cleared to BLUE   (0, 0, 255)
 *   - the top-left quadrant filled YELLOW (255, 255, 0)
 *
 * Every channel value is 0 or 255, which is what lets test/ui-present.js
 * compare interior samples EXACTLY rather than with a tolerance: those are
 * the two values that survive any float-to-unorm8 rounding rule a driver
 * might use. The layout is deliberately asymmetric in both axes, so that a
 * vertical flip, a horizontal flip and a channel swap are three
 * distinguishable failures rather than one ambiguous one.
 *
 * # What this addon never does
 *
 * `sg_setup`, `sg_shutdown` and `sg_commit` belong to the host (spec: The
 * native C API). It creates its two resources inside a frame callback,
 * where a commit follows -- the contract v1 states for resource creation,
 * and the reason it is stated is Metal's id-pool, which asserts rather
 * than failing gracefully when resources churn outside those points.
 */

#include <hnui.h>
#include <node_api.h>
#include <sokol_gfx.h>

#include <stdbool.h>
#include <stdio.h>
/* memset: every sokol desc struct is zeroed before its fields are set,
   because Sokol's canaries require it and C89-style designated
   initializers are not used here. */
#include <string.h>

/* ------------------------------------------------------------------ */
/* The pattern's two shaders.                                          */
/* ------------------------------------------------------------------ */

/**
 * Six vertices from `gl_VertexID` alone: two triangles covering the
 * top-left quadrant in clip space, x in [-1, 0] and y in [0, 1].
 *
 * No vertex buffer, and therefore no `sg_apply_bindings` below: the
 * pipeline declares no vertex layout, so Sokol requires no vertex binding
 * (`_sg_validate_draw` compares required bindings against applied ones,
 * and both are zero). That keeps this addon to two resources, which is the
 * point of a reference addon -- the API is what is being demonstrated, not
 * a rendering technique.
 *
 * `#version 410` is Sokol's lower floor for desktop GL
 * (`sokol_gfx.h:924`); nothing here needs 430.
 */
static const char *kPatternVertexSource =
    "#version 410\n"
    "void main() {\n"
    "  vec2 corners[6] = vec2[6](\n"
    "    vec2(-1.0, 0.0), vec2(0.0, 0.0), vec2(0.0, 1.0),\n"
    "    vec2(-1.0, 0.0), vec2(0.0, 1.0), vec2(-1.0, 1.0));\n"
    "  gl_Position = vec4(corners[gl_VertexID], 0.0, 1.0);\n"
    "}\n";

static const char *kPatternFragmentSource =
    "#version 410\n"
    "out vec4 frag_color;\n"
    "void main() {\n"
    "  frag_color = vec4(1.0, 1.0, 0.0, 1.0);\n"
    "}\n";

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

static sg_shader g_shader;
static sg_pipeline g_pipeline;
static bool g_resourcesReady = false;
static int g_framesDrawn = 0;

/**
 * Created on the first frame callback, never outside one.
 *
 * \return false having printed why. A renderer that silently skipped its
 *   own setup would publish empty surfaces, which looks exactly like a
 *   compositor bug from the outside.
 */
static bool ensureResources(void) {
  sg_shader_desc shaderDesc;
  sg_pipeline_desc pipelineDesc;

  if (g_resourcesReady) {
    return true;
  }

  memset(&shaderDesc, 0, sizeof(shaderDesc));
  shaderDesc.vertex_func.source = kPatternVertexSource;
  shaderDesc.fragment_func.source = kPatternFragmentSource;
  shaderDesc.label = "hnui-demo-shader";
  g_shader = sg_make_shader(&shaderDesc);
  if (sg_query_shader_state(g_shader) != SG_RESOURCESTATE_VALID) {
    fprintf(stderr, "hnui-demo: the pattern shader failed to compile\n");
    return false;
  }

  memset(&pipelineDesc, 0, sizeof(pipelineDesc));
  pipelineDesc.shader = g_shader;
  pipelineDesc.primitive_type = SG_PRIMITIVETYPE_TRIANGLES;
  pipelineDesc.cull_mode = SG_CULLMODE_NONE;
  /* .colors[0].pixel_format, .depth.pixel_format and .sample_count are
     left to Sokol's defaulting, which takes them from the environment the
     HOST passed to sg_setup -- the surface colour format, depth NONE and
     sample_count 1. That is exactly what spec's "default pipelines in
     sokol_imgui and sokol_gl match with no configuration" promises an
     addon, so this addon is also the thing that checks the promise. */
  pipelineDesc.label = "hnui-demo-pipeline";
  g_pipeline = sg_make_pipeline(&pipelineDesc);
  if (sg_query_pipeline_state(g_pipeline) != SG_RESOURCESTATE_VALID) {
    fprintf(stderr, "hnui-demo: the pattern pipeline could not be created\n");
    return false;
  }

  g_resourcesReady = true;
  return true;
}

/**
 * Draw the fixed pattern into this frame's surface.
 *
 * The whole addon, really: ask for the target, begin a pass against its
 * colour view, clear, draw, end. Resize needs no handling at all -- the
 * target reports its size and generation every frame, which is the point
 * of that being per-frame data.
 */
static bool drawPattern(hnui_window *w) {
  hnui_target target;
  sg_pass pass;
  sg_view colorView;

  memset(&target, 0, sizeof(target));
  /* The size-prefix rule: the CALLER sets `size`, and the host writes at
     most that many bytes. An addon built against a later, larger header
     passes a larger size and gets the extra fields; this one passes its
     own. */
  target.size = (uint32_t)sizeof(target);
  if (!hnui_frame_target(w, &target)) {
    /* Outside a frame callback, or no slot was free. A legitimate answer,
       not an error -- and nothing was drawn, so the host reclaims the
       frame rather than publishing a surface with undefined contents. */
    return false;
  }

  /* By value, never by pointer cast: the two types have identical layout,
     but reinterpreting one pointer as the other violates C's effective-
     type rule even so, and addons build at -O2 where that is not a
     formality (hnui_abi.h, hnui_gpu_view). */
  memset(&colorView, 0, sizeof(colorView));
  colorView.id = target.color_view.id;

  memset(&pass, 0, sizeof(pass));
  pass.attachments.colors[0] = colorView;
  pass.action.colors[0].load_action = SG_LOADACTION_CLEAR;
  pass.action.colors[0].store_action = SG_STOREACTION_STORE;
  /* BLUE. Exactly 0 and 1, so the unorm8 result is exactly 0 and 255 on
     any driver -- see this file's header comment. */
  pass.action.colors[0].clear_value.r = 0.0f;
  pass.action.colors[0].clear_value.g = 0.0f;
  pass.action.colors[0].clear_value.b = 1.0f;
  pass.action.colors[0].clear_value.a = 1.0f;

  sg_begin_pass(&pass);
  sg_apply_pipeline(g_pipeline);
  sg_draw(0, 6, 1);
  sg_end_pass();
  /* No sg_commit: the host commits after the last callback of the tick,
     and arranges completion. */

  ++g_framesDrawn;
  return true;
}

/* ------------------------------------------------------------------ */
/* The native frame-callback mode                                      */
/* ------------------------------------------------------------------ */

/**
 * Registered with `hnui_request_animation_frame`, so that the native
 * tick path has a consumer too -- the spec asks lit to run both modes,
 * not just the JS-driven one, and a path with no caller is a path nobody
 * has checked.
 *
 * Re-registers itself, which is how a native animation loop is written:
 * a callback registered inside a callback runs on the next tick.
 */
static void nativeFrame(hnui_window *w, void *user, double timestampMs) {
  (void)user;
  (void)timestampMs;
  if (!ensureResources()) {
    return;
  }
  drawPattern(w);
  hnui_request_animation_frame(w, nativeFrame, NULL);
}

/* ------------------------------------------------------------------ */
/* The JS surface                                                      */
/* ------------------------------------------------------------------ */

static napi_value drawFixedPattern(napi_env env, napi_callback_info info) {
  napi_value result;
  hnui_window *w = hnui_get_window(env);
  bool drew = false;

  (void)info;
  if (w != NULL && ensureResources()) {
    drew = drawPattern(w);
  }
  napi_get_boolean(env, drew, &result);
  return result;
}

static napi_value startNativeFrames(napi_env env, napi_callback_info info) {
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

static napi_value framesDrawn(napi_env env, napi_callback_info info) {
  napi_value result;
  (void)info;
  napi_create_int32(env, g_framesDrawn, &result);
  return result;
}

/**
 * Call the two checked entry points with a NULL window handle, and report
 * what they answered.
 *
 * `hnui_get_window()` documents NULL as an ordinary answer -- "an addon that
 * can render headless may carry on" -- so an addon is entitled to pass that
 * NULL back in. Every other call site in this file guards with `w != NULL`,
 * which is precisely why nothing here reached the host's null path until
 * this existed: it used to abort the process with a wrong-thread message,
 * for a window that was simply absent.
 */
static napi_value probeNullWindow(napi_env env, napi_callback_info info) {
  hnui_target target;
  napi_value result;
  char answer[64];
  bool gotTarget;
  int rafId;

  (void)info;
  memset(&target, 0, sizeof(target));
  target.size = (uint32_t)sizeof(target);
  gotTarget = hnui_frame_target(NULL, &target);
  rafId = hnui_request_animation_frame(NULL, nativeFrame, NULL);
  snprintf(
      answer,
      sizeof(answer),
      "target=%s raf=%d",
      gotTarget ? "true" : "false",
      rafId);
  napi_create_string_utf8(env, answer, NAPI_AUTO_LENGTH, &result);
  return result;
}

static napi_value hasWindow(napi_env env, napi_callback_info info) {
  napi_value result;
  (void)info;
  napi_get_boolean(env, hnui_get_window(env) != NULL, &result);
  return result;
}

NAPI_MODULE_INIT() {
  napi_property_descriptor descs[] = {
      {"drawFixedPattern",
       NULL,
       drawFixedPattern,
       NULL,
       NULL,
       NULL,
       napi_default,
       NULL},
      {"startNativeFrames",
       NULL,
       startNativeFrames,
       NULL,
       NULL,
       NULL,
       napi_default,
       NULL},
      {"framesDrawn", NULL, framesDrawn, NULL, NULL, NULL, napi_default, NULL},
      {"hasWindow", NULL, hasWindow, NULL, NULL, NULL, napi_default, NULL},
      {"probeNullWindow",
       NULL,
       probeNullWindow,
       NULL,
       NULL,
       NULL,
       napi_default,
       NULL},
  };
  napi_define_properties(
      env, exports, sizeof(descs) / sizeof(descs[0]), descs);
  return exports;
}
