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
 * **This file is the fixed pattern and the test probes**; the demo proper
 * -- the rotating `sokol_gl` triangle, and the worked example for `hnui.h`
 * -- is demo_visual.c, and the conformance mode is demo_conformance.c (see
 * demo_addon.h for why they are apart). The module init at the bottom of
 * this file registers all three.
 *
 * The fixed pattern exists because the spec's tests say "What draws in
 * these tests is the reference addon, in a mode that renders a fixed
 * pattern rather than a rotating one": a rotating triangle makes captured
 * pixels depend on elapsed time, which a capture keyed to a publication id
 * must not depend on. demo.js shows it as its `pattern` mode, and
 * test/ui-present.js captures it.
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

#include "demo_addon.h"

#include <hnui.h>
#include <node_api.h>
#include <sokol_gfx.h>

#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
/* memset: every sokol desc struct is zeroed before its fields are set,
   because Sokol's canaries require it and C89-style designated
   initializers are not used here. */
#include <string.h>
#include <stdlib.h>

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
/* The most recent hnui_frame_target answer, for lastTarget(). Zero until
   the first one, and zeroed again by a refusal: generation 0 is reserved
   for "no generation", so a test can mistake neither "never asked" nor
   "asked and refused" for an answer -- a stale target from an earlier
   frame would otherwise satisfy an assertion about this one. */
static hnui_target g_lastTarget;

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
    memset(&g_lastTarget, 0, sizeof(g_lastTarget));
    return false;
  }
  g_lastTarget = target;

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
 * `{generation, width, height}` of the most recent `hnui_frame_target`
 * answer -- what the host told this renderer to draw into, which is the
 * only place a resize is visible from inside an addon. That is the claim
 * "resize needs no handling at all" rests on, so test/ui-resize-generation.js
 * reads it back from here rather than from anything the host reports about
 * itself.
 */
static napi_value lastTarget(napi_env env, napi_callback_info info) {
  napi_value result;
  napi_value value;

  (void)info;
  napi_create_object(env, &result);
  napi_create_uint32(env, g_lastTarget.generation, &value);
  napi_set_named_property(env, result, "generation", value);
  napi_create_int32(env, g_lastTarget.width, &value);
  napi_set_named_property(env, result, "width", value);
  napi_create_int32(env, g_lastTarget.height, &value);
  napi_set_named_property(env, result, "height", value);
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

/* ------------------------------------------------------------------ */
/* The event log                                                       */
/* ------------------------------------------------------------------ */

/**
 * One ordered log, written by both sides: the native event listener
 * appends `native:<name> ...`, and JavaScript appends through `logLine`.
 *
 * **One log, not printf and console.log.** The two reach the same pipe by
 * different routes -- C stdio's buffer, and a libuv stream's write queue --
 * and nothing orders one against the other, so a test asserting that the
 * native listener ran before the JS one could not read that from the
 * output. Here the order of appends is the order of execution, because
 * both sides run on the same thread.
 */
#define LOG_MAX_LINES 1024
#define LOG_LINE_BYTES 160

static char g_log[LOG_MAX_LINES][LOG_LINE_BYTES];
static int g_logCount = 0;
/* Reported as a final line rather than dropped silently: a test that lost
   its tail would otherwise fail for a reason it cannot see. */
static bool g_logOverflowed = false;
static int g_eventListenerId = 0;
/* How many close requests the event-log listener still cancels; set by
   armCloseCancel(). */
static int g_closeCancelsArmed = 0;

static void appendLog(const char *line) {
  if (g_logCount >= LOG_MAX_LINES) {
    g_logOverflowed = true;
    return;
  }
  snprintf(g_log[g_logCount], LOG_LINE_BYTES, "%s", line);
  ++g_logCount;
}

/** The JS `emit` name for an event type, so the two sides' lines pair up. */
static const char *eventName(int32_t type) {
  switch (type) {
    case HNUI_EVENT_KEY_DOWN:
      return "keydown";
    case HNUI_EVENT_KEY_UP:
      return "keyup";
    case HNUI_EVENT_TEXT_INPUT:
      return "char";
    case HNUI_EVENT_MOUSE_DOWN:
      return "mousedown";
    case HNUI_EVENT_MOUSE_UP:
      return "mouseup";
    case HNUI_EVENT_MOUSE_MOVE:
      return "mousemove";
    case HNUI_EVENT_MOUSE_WHEEL:
      return "wheel";
    case HNUI_EVENT_MOUSE_ENTER:
      return "mouseenter";
    case HNUI_EVENT_MOUSE_LEAVE:
      return "mouseleave";
    case HNUI_EVENT_RESIZE:
      return "resize";
    case HNUI_EVENT_FOCUS:
      return "focus";
    case HNUI_EVENT_BLUR:
      return "blur";
    case HNUI_EVENT_MINIMIZE:
      return "minimize";
    case HNUI_EVENT_RESTORE:
      return "restore";
    case HNUI_EVENT_CLOSE_REQUESTED:
      return "close";
    case HNUI_EVENT_PASTE:
      return "paste";
    case HNUI_EVENT_FILE_DROP:
      return "drop";
    default:
      return "unknown";
  }
}

/**
 * The native listener: one line per event, with the fields test/
 * ui-events.js checks. Focus changes also read the window state back
 * through `hnui_get_window_state`, which is the native half of "the mirror
 * is updated before any listener for the event runs".
 */
static void onEvent(hnui_window *w, void *user, const hnui_event *event) {
  char line[LOG_LINE_BYTES];
  const char *name = eventName(event->type);
  hnui_window_state state;

  (void)user;
  switch (event->type) {
    case HNUI_EVENT_KEY_DOWN:
    case HNUI_EVENT_KEY_UP:
      snprintf(
          line,
          sizeof(line),
          "native:%s key=%d mods=%d",
          name,
          (int)event->key_code,
          (int)event->modifiers);
      break;
    case HNUI_EVENT_TEXT_INPUT:
    case HNUI_EVENT_PASTE:
      snprintf(
          line,
          sizeof(line),
          "native:%s text=%s",
          name,
          event->text != NULL ? event->text : "(null)");
      break;
    case HNUI_EVENT_MOUSE_DOWN:
    case HNUI_EVENT_MOUSE_UP:
      snprintf(
          line, sizeof(line), "native:%s button=%d", name, (int)event->button);
      break;
    case HNUI_EVENT_MOUSE_MOVE:
      snprintf(line, sizeof(line), "native:%s dx=%g", name, event->dx);
      break;
    case HNUI_EVENT_FILE_DROP:
      snprintf(
          line,
          sizeof(line),
          "native:%s paths=%u",
          name,
          (unsigned)event->path_count);
      break;
    case HNUI_EVENT_FOCUS:
    case HNUI_EVENT_BLUR:
      memset(&state, 0, sizeof(state));
      state.size = (uint32_t)sizeof(state);
      hnui_get_window_state(w, &state);
      snprintf(
          line,
          sizeof(line),
          "native:%s focused=%d",
          name,
          state.focused ? 1 : 0);
      break;
    default:
      snprintf(line, sizeof(line), "native:%s", name);
      break;
  }
  appendLog(line);

  /* armCloseCancel(n): cancel the next n close requests from here, and log
     what hnui_cancel_close answered, so the test reads the answer rather
     than inferring it from the window surviving. */
  if (event->type == HNUI_EVENT_CLOSE_REQUESTED && g_closeCancelsArmed > 0) {
    --g_closeCancelsArmed;
    snprintf(
        line,
        sizeof(line),
        "native:cancel_close=%d",
        hnui_cancel_close(w) ? 1 : 0);
    appendLog(line);
  }
}

static napi_value startEventLog(napi_env env, napi_callback_info info) {
  napi_value result;
  hnui_window *w = hnui_get_window(env);

  (void)info;
  if (w != NULL && g_eventListenerId == 0) {
    g_eventListenerId = hnui_add_event_listener(w, onEvent, NULL);
  }
  napi_create_int32(env, g_eventListenerId, &result);
  return result;
}

/** `armCloseCancel(n)`: the event-log listener calls hnui_cancel_close for
    the next \p n close requests, and logs `native:cancel_close=<0|1>`. */
static napi_value armCloseCancel(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  int32_t n = 0;

  napi_get_cb_info(env, info, &argc, argv, NULL, NULL);
  if (argc >= 1) {
    napi_get_value_int32(env, argv[0], &n);
  }
  g_closeCancelsArmed = n > 0 ? n : 0;
  return NULL;
}

/** `cancelCloseNow()` -> what hnui_cancel_close answers when called right
    now, outside any close request: false. */
static napi_value cancelCloseNow(napi_env env, napi_callback_info info) {
  napi_value result;
  (void)info;
  napi_get_boolean(env, hnui_cancel_close(hnui_get_window(env)), &result);
  return result;
}

static napi_value logLine(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  char line[LOG_LINE_BYTES];
  size_t length = 0;

  napi_get_cb_info(env, info, &argc, argv, NULL, NULL);
  line[0] = '\0';
  if (argc >= 1) {
    napi_get_value_string_utf8(env, argv[0], line, sizeof(line), &length);
  }
  appendLog(line);
  return NULL;
}

static napi_value takeEventLog(napi_env env, napi_callback_info info) {
  napi_value result;
  napi_value line;
  int i;
  int count = g_logCount;

  (void)info;
  napi_create_array_with_length(
      env, (size_t)count + (g_logOverflowed ? 1 : 0), &result);
  for (i = 0; i < count; ++i) {
    napi_create_string_utf8(env, g_log[i], NAPI_AUTO_LENGTH, &line);
    napi_set_element(env, result, (uint32_t)i, line);
  }
  if (g_logOverflowed) {
    napi_create_string_utf8(env, "LOG OVERFLOWED", NAPI_AUTO_LENGTH, &line);
    napi_set_element(env, result, (uint32_t)count, line);
  }
  g_logCount = 0;
  g_logOverflowed = false;
  return result;
}

/* ------------------------------------------------------------------ */
/* Tagged native frame callbacks, for test/ui-raf.js                   */
/* ------------------------------------------------------------------ */

/**
 * The tags `requestNativeFrame` was given, one slot per registration. A
 * slot's address is the callback's `user` pointer. Static storage rather
 * than a malloc'd copy, so a registration that is cancelled -- and is
 * therefore never run to free anything -- costs nothing to abandon.
 */
#define TAG_SLOTS 256
#define TAG_BYTES 32
static char g_tags[TAG_SLOTS][TAG_BYTES];
static int g_nextTag = 0;

/** Appends `native:<tag>` to the shared log, so ordering against the JS
    callbacks' own lines is the order they ran in. */
static void taggedFrame(hnui_window *w, void *user, double timestampMs) {
  char line[LOG_LINE_BYTES];
  (void)w;
  (void)timestampMs;
  snprintf(line, sizeof(line), "native:%s", (const char *)user);
  appendLog(line);
}

/** `requestNativeFrame(tag)` -> the id `hnui_request_animation_frame`
    returned, or 0 with no window. */
static napi_value requestNativeFrame(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_value result;
  size_t length = 0;
  char *tag;
  hnui_window *w = hnui_get_window(env);
  int id = 0;

  napi_get_cb_info(env, info, &argc, argv, NULL, NULL);
  tag = g_tags[g_nextTag];
  g_nextTag = (g_nextTag + 1) % TAG_SLOTS;
  tag[0] = '\0';
  if (argc >= 1) {
    napi_get_value_string_utf8(env, argv[0], tag, TAG_BYTES, &length);
  }
  if (w != NULL) {
    id = hnui_request_animation_frame(w, taggedFrame, tag);
  }
  napi_create_int32(env, id, &result);
  return result;
}

/** `cancelNativeFrame(id)`: `hnui_cancel_animation_frame`, with whatever
    id it is given -- a native one, a JavaScript one, or one never issued. */
static napi_value cancelNativeFrame(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  int32_t id = 0;

  napi_get_cb_info(env, info, &argc, argv, NULL, NULL);
  if (argc >= 1) {
    napi_get_value_int32(env, argv[0], &id);
  }
  hnui_cancel_animation_frame(hnui_get_window(env), id);
  return NULL;
}

/* ------------------------------------------------------------------ */
/* Panics on the producing instance (test/ui-sokol-panic.js)            */
/* ------------------------------------------------------------------ */

/**
 * Call the producing sokol_gfx instance's own logger with a panic item, the
 * way `_SG_PANIC` does. Present in every build, which a real validation
 * failure is not. Returns only if the logger returned -- which is the
 * defect -- and says so.
 */
static napi_value panicThroughProducerLogger(
    napi_env env,
    napi_callback_info info) {
  sg_desc desc = sg_query_desc();
  napi_value result;

  (void)info;
  if (desc.logger.func != NULL) {
    desc.logger.func(
        "sg",
        0,
        (uint32_t)SG_LOGITEM_VALIDATION_FAILED,
        "raised deliberately by hnui_demo_addon",
        (uint32_t)__LINE__,
        __FILE__,
        desc.logger.user_data);
  }
  napi_create_string_utf8(env, "RETURNED", NAPI_AUTO_LENGTH, &result);
  return result;
}

/**
 * A frame callback that begins a pass with the target's two views swapped:
 * the RESOLVE view where the colour attachment belongs and the colour view
 * as its resolve. A validating sokol_gfx rejects it (VALIDATE_BEGINPASS_
 * COLORATTACHMENTVIEW_TYPE among others) and panics; anything printed
 * after sg_begin_pass means it did not.
 */
static void invalidPassFrame(hnui_window *w, void *user, double timestamp) {
  hnui_target target;
  sg_pass pass;

  (void)user;
  (void)timestamp;
  memset(&target, 0, sizeof(target));
  target.size = (uint32_t)sizeof(target);
  if (!hnui_frame_target(w, &target))
    return;
  memset(&pass, 0, sizeof(pass));
  pass.attachments.colors[0].id = target.resolve_view.id;
  pass.attachments.resolves[0].id = target.color_view.id;
  pass.action.colors[0].load_action = SG_LOADACTION_CLEAR;
  sg_begin_pass(&pass);
  printf("PASS BEGAN\n");
  fflush(stdout);
  sg_end_pass();
}

static napi_value requestInvalidPassFrame(
    napi_env env,
    napi_callback_info info) {
  napi_value result;
  (void)info;
  napi_create_int32(
      env,
      hnui_request_animation_frame(hnui_get_window(env), invalidPassFrame, NULL),
      &result);
  return result;
}

/** The body of callStateOffThread()'s thread. */
static void *getStateOffThread(void *arg) {
  hnui_window_state state;
  memset(&state, 0, sizeof(state));
  state.size = sizeof(state);
  hnui_get_window_state((hnui_window *)arg, &state);
  return NULL;
}

/**
 * Call `hnui_get_window_state` from a thread that is not the window's
 * owner, and wait for it. The spec's answer (Errors) is an abort naming the
 * function, so this returns only if the check is missing -- which
 * test/ui-wrong-thread.js turns into a failure.
 */
static napi_value callStateOffThread(napi_env env, napi_callback_info info) {
  hnui_window *w = hnui_get_window(env);
  pthread_t thread;

  (void)info;
  if (w == NULL || pthread_create(&thread, NULL, getStateOffThread, w) != 0)
    return NULL;
  pthread_join(thread, NULL);
  return NULL;
}

/** `hnui_get_window_state`, as a JS object, or null with no window. */
static napi_value getWindowState(napi_env env, napi_callback_info info) {
  napi_value result;
  napi_value value;
  hnui_window_state state;
  hnui_window *w = hnui_get_window(env);

  (void)info;
  if (w == NULL) {
    napi_get_null(env, &result);
    return result;
  }
  memset(&state, 0, sizeof(state));
  state.size = (uint32_t)sizeof(state);
  hnui_get_window_state(w, &state);

  napi_create_object(env, &result);
  napi_create_int32(env, state.width, &value);
  napi_set_named_property(env, result, "width", value);
  napi_create_int32(env, state.height, &value);
  napi_set_named_property(env, result, "height", value);
  napi_create_int32(env, state.framebuffer_width, &value);
  napi_set_named_property(env, result, "framebufferWidth", value);
  napi_create_int32(env, state.framebuffer_height, &value);
  napi_set_named_property(env, result, "framebufferHeight", value);
  napi_create_double(env, state.dpi_scale, &value);
  napi_set_named_property(env, result, "dpiScale", value);
  napi_get_boolean(env, state.focused, &value);
  napi_set_named_property(env, result, "focused", value);
  napi_get_boolean(env, state.fullscreen, &value);
  napi_set_named_property(env, result, "fullscreen", value);
  napi_get_boolean(env, state.minimized, &value);
  napi_set_named_property(env, result, "minimized", value);
  return result;
}

/* ------------------------------------------------------------------ */
/* The synchronous clipboard read                                      */
/* ------------------------------------------------------------------ */

static const char *statusName(int status) {
  switch (status) {
    case HNUI_OK:
      return "HNUI_OK";
    case HNUI_ERROR:
      return "HNUI_ERROR";
    case HNUI_TIMEOUT:
      return "HNUI_TIMEOUT";
    case HNUI_INVALID_ARGUMENT:
      return "HNUI_INVALID_ARGUMENT";
    case HNUI_UNAVAILABLE:
      return "HNUI_UNAVAILABLE";
    case HNUI_TOO_LARGE:
      return "HNUI_TOO_LARGE";
    default:
      return "UNKNOWN";
  }
}

/**
 * `readClipboardSync(timeoutMs)` -> `{status, text}`: `hnui_read_clipboard_sync`
 * as ImGui's `GetClipboardTextFn` would call it. `status` is the
 * `HNUI_*` name; `text` is the string on `HNUI_OK` and otherwise whatever
 * the call left in `*out` -- which must be NULL, so it is reported as `null`
 * when it is and as the string it points at when it is not, letting a test
 * see a violation rather than having it hidden here.
 */
static napi_value readClipboardSync(napi_env env, napi_callback_info info) {
  napi_value argv[1];
  size_t argc = 1;
  int32_t timeoutMs = 0;
  char *text = (char *)"<untouched>";
  int status;
  napi_value result;
  napi_value value;

  napi_get_cb_info(env, info, &argc, argv, NULL, NULL);
  if (argc >= 1)
    napi_get_value_int32(env, argv[0], &timeoutMs);

  status = hnui_read_clipboard_sync(hnui_get_window(env), timeoutMs, &text);

  napi_create_object(env, &result);
  napi_create_string_utf8(env, statusName(status), NAPI_AUTO_LENGTH, &value);
  napi_set_named_property(env, result, "status", value);
  if (text == NULL) {
    napi_get_null(env, &value);
  } else {
    napi_create_string_utf8(env, text, NAPI_AUTO_LENGTH, &value);
  }
  napi_set_named_property(env, result, "text", value);
  /* Only what the call allocated is freed; hnui_free(NULL) is allowed and
     exercised on every non-OK path. */
  hnui_free(status == HNUI_OK ? text : NULL);
  return result;
}

/* ------------------------------------------------------------------ */
/* The size-prefix probe, for test/ui-abi-sizes.js                     */
/* ------------------------------------------------------------------ */

/**
 * A struct embedded in a larger buffer poisoned with 0xA5, so that bytes
 * past the `size` the caller claims EXIST to be checked. Offsets come from
 * the HNUI_*_MIN_SIZE constants, not sizeof: a sizeof-based probe would
 * pass for a writer that wrote exactly sizeof, which is the failure.
 */
#define PROBE_BUFFER_BYTES 256
#define PROBE_POISON 0xA5
#define PROBE_LINES 8

typedef union {
  uint64_t align;
  unsigned char bytes[PROBE_BUFFER_BYTES];
} ProbeBuffer;

static char g_probeLines[PROBE_LINES][64];
static int g_probeCount = 0;

static void probeResult(const char *name, const char *verdict) {
  if (g_probeCount < PROBE_LINES) {
    snprintf(g_probeLines[g_probeCount], 64, "%s %s", name, verdict);
    ++g_probeCount;
  }
}

static void poison(ProbeBuffer *buffer, uint32_t size) {
  memset(buffer->bytes, PROBE_POISON, sizeof(buffer->bytes));
  memcpy(buffer->bytes, &size, sizeof(size));
}

/** Whether every byte from \p from onward is still poison. */
static bool tailIntact(const ProbeBuffer *buffer, uint32_t from) {
  uint32_t i;
  for (i = from; i < PROBE_BUFFER_BYTES; ++i) {
    if (buffer->bytes[i] != PROBE_POISON) {
      return false;
    }
  }
  return true;
}

/**
 * Per writer, four sizes (`name:<case>` verdict lines):
 *   exact    size == MIN_SIZE: succeeds, nothing at or past `size` changes.
 *   short    size == MIN_SIZE - 1: refused, no byte changes.
 *   full     size == sizeof (hnui_window_state only; for hnui_target it is
 *            the exact case, the struct having no padding).
 *   larger   size == 64, above the host's sizeof: what an addon built
 *            against a NEWER, larger header passes. Every byte from the
 *            host's sizeof onward must be poison, and the bytes the exact
 *            case filled (past `size`) must be identical.
 * A writer that copied `out->size` bytes would write and read past what it
 * knows; one that copied sizeof regardless would fail `exact`.
 */
#define PROBE_LARGER_SIZE 64

typedef bool (*ProbeWriter)(hnui_window *w, void *out);

static bool writeTarget(hnui_window *w, void *out) {
  return hnui_frame_target(w, (hnui_target *)out);
}

/* void, so there is no status: `true` is a placeholder, and the verdicts
   rest on the bytes alone (hasStatus is false for it). */
static bool writeState(hnui_window *w, void *out) {
  hnui_get_window_state(w, (hnui_window_state *)out);
  return true;
}

/** Whether the first field after `size` was written (not still poison). */
static bool wroteFirstField(const ProbeBuffer *buffer) {
  int32_t first;
  memcpy(&first, buffer->bytes + sizeof(uint32_t), sizeof(first));
  return first != (int32_t)0xA5A5A5A5;
}

static void probeWriter(
    hnui_window *w,
    const char *name,
    ProbeWriter write,
    uint32_t minSize,
    uint32_t fullSize,
    bool hasStatus) {
  ProbeBuffer exact;
  ProbeBuffer buffer;
  ProbeBuffer before;
  char label[32];
  bool answered;

  poison(&exact, minSize);
  answered = write(w, exact.bytes);
  snprintf(label, sizeof(label), "%s:exact", name);
  probeResult(
      label,
      answered && wroteFirstField(&exact) && tailIntact(&exact, minSize)
          ? "ok"
          : "WRONG");

  poison(&buffer, minSize - 1);
  before = buffer;
  answered = write(w, buffer.bytes);
  snprintf(label, sizeof(label), "%s:short", name);
  probeResult(
      label,
      (!hasStatus || !answered) &&
              memcmp(&before, &buffer, sizeof(buffer)) == 0
          ? "ok"
          : "WRONG");

  poison(&buffer, fullSize);
  answered = write(w, buffer.bytes);
  snprintf(label, sizeof(label), "%s:full", name);
  probeResult(
      label,
      answered && wroteFirstField(&buffer) && tailIntact(&buffer, fullSize)
          ? "ok"
          : "WRONG");

  poison(&buffer, PROBE_LARGER_SIZE);
  answered = write(w, buffer.bytes);
  snprintf(label, sizeof(label), "%s:larger", name);
  probeResult(
      label,
      answered && tailIntact(&buffer, fullSize) &&
              memcmp(
                  buffer.bytes + sizeof(uint32_t),
                  exact.bytes + sizeof(uint32_t),
                  minSize - sizeof(uint32_t)) == 0
          ? "ok"
          : "WRONG");
}

static void sizeProbeFrame(hnui_window *w, void *user, double timestampMs) {
  (void)user;
  (void)timestampMs;
  probeWriter(
      w, "target", writeTarget, HNUI_TARGET_MIN_SIZE, sizeof(hnui_target), true);
  probeWriter(
      w,
      "state",
      writeState,
      HNUI_WINDOW_STATE_MIN_SIZE,
      sizeof(hnui_window_state),
      false);
}

/** `probeSizes()`: run the probe from the next frame callback, where
    hnui_frame_target answers. Read the verdicts with `takeSizeProbe()`
    after a frame has passed. */
static napi_value probeSizes(napi_env env, napi_callback_info info) {
  hnui_window *w = hnui_get_window(env);
  (void)info;
  g_probeCount = 0;
  if (w != NULL) {
    hnui_request_animation_frame(w, sizeProbeFrame, NULL);
  }
  return NULL;
}

static napi_value takeSizeProbe(napi_env env, napi_callback_info info) {
  napi_value result;
  napi_value line;
  int i;
  (void)info;
  napi_create_array_with_length(env, (size_t)g_probeCount, &result);
  for (i = 0; i < g_probeCount; ++i) {
    napi_create_string_utf8(env, g_probeLines[i], NAPI_AUTO_LENGTH, &line);
    napi_set_element(env, result, (uint32_t)i, line);
  }
  g_probeCount = 0;
  return result;
}

/**
 * `probeCommands()`: the four command entry points, with the refusals a
 * caller can make. Reports `{cursor, clipboard}` as `ok` or a description;
 * what the platform thread APPLIED is read from the trace by the test.
 * The refusals print their own lines on stderr.
 */
static napi_value probeCommands(napi_env env, napi_callback_info info) {
  napi_value result;
  napi_value value;
  hnui_window *w = hnui_get_window(env);
  size_t boundary = HNUI_CLIPBOARD_MAX_BYTES - 1;
  char *text = (char *)malloc(HNUI_CLIPBOARD_MAX_BYTES + 1);

  (void)info;
  napi_create_object(env, &result);
  if (w == NULL || text == NULL) {
    napi_create_string_utf8(env, "no window", NAPI_AUTO_LENGTH, &value);
    napi_set_named_property(env, result, "cursor", value);
    napi_set_named_property(env, result, "clipboard", value);
    free(text);
    return result;
  }
  hnui_set_cursor(w, (hnui_cursor)999);
  hnui_set_cursor(w, (hnui_cursor)-1);
  hnui_set_cursor(w, HNUI_CURSOR_POINTING_HAND);
  hnui_show_cursor(w, false);
  hnui_show_cursor(w, true);
  hnui_lock_pointer(w, true);
  hnui_lock_pointer(w, false);

  hnui_write_clipboard(w, NULL);
  memset(text, 'a', HNUI_CLIPBOARD_MAX_BYTES);
  text[HNUI_CLIPBOARD_MAX_BYTES] = '\0';
  hnui_write_clipboard(w, text); /* MAX_BYTES bytes: refused */
  text[boundary] = '\0';
  hnui_write_clipboard(w, text); /* MAX_BYTES - 1: accepted */
  free(text);

  napi_create_string_utf8(env, "ok", NAPI_AUTO_LENGTH, &value);
  napi_set_named_property(env, result, "cursor", value);
  napi_set_named_property(env, result, "clipboard", value);
  return result;
}

/* ------------------------------------------------------------------ */
/* Listener removal, for test/ui-events.js                             */
/* ------------------------------------------------------------------ */

static int g_listenerB = 0;
static int g_removalDone = 0;

static void listenerB(hnui_window *w, void *user, const hnui_event *event) {
  char line[LOG_LINE_BYTES];
  (void)w;
  (void)user;
  snprintf(line, sizeof(line), "B:%d", (int)event->key_code);
  appendLog(line);
}

/** Listener A logs whether delivery looks quiescent from inside it, and on
    its first event removes B -- twice, and an id never issued once -- all
    of which must be harmless. */
static void listenerA(hnui_window *w, void *user, const hnui_event *event) {
  char line[LOG_LINE_BYTES];
  (void)user;
  snprintf(
      line,
      sizeof(line),
      "A:%d quiescent=%d",
      (int)event->key_code,
      hnui_event_dispatch_quiescent(w) ? 1 : 0);
  appendLog(line);
  if (!g_removalDone) {
    g_removalDone = 1;
    hnui_remove_event_listener(w, g_listenerB);
    hnui_remove_event_listener(w, g_listenerB);
    hnui_remove_event_listener(w, 987654);
  }
}

/** A is registered BEFORE B, so that B is still ahead in event 1's list
    when A removes it. Only key events are logged (the first phase's
    listener sees everything else). */
static napi_value startRemovalProbe(napi_env env, napi_callback_info info) {
  hnui_window *w = hnui_get_window(env);
  (void)info;
  if (w != NULL) {
    hnui_add_event_listener(w, listenerA, NULL);
    g_listenerB = hnui_add_event_listener(w, listenerB, NULL);
  }
  return NULL;
}

static void quiescenceFrame(hnui_window *w, void *user, double timestampMs) {
  char line[LOG_LINE_BYTES];
  (void)user;
  (void)timestampMs;
  snprintf(
      line,
      sizeof(line),
      "frame: quiescent=%d",
      hnui_event_dispatch_quiescent(w) ? 1 : 0);
  appendLog(line);
}

static napi_value requestQuiescenceRead(
    napi_env env,
    napi_callback_info info) {
  hnui_window *w = hnui_get_window(env);
  (void)info;
  if (w != NULL) {
    hnui_request_animation_frame(w, quiescenceFrame, NULL);
  }
  return NULL;
}

/* ------------------------------------------------------------------ */
/* What the host says it is, for test/ui-version-check.js              */
/* ------------------------------------------------------------------ */

static napi_value hostSokolCommit(napi_env env, napi_callback_info info) {
  napi_value result;
  (void)info;
  napi_create_string_utf8(env, hnui_sokol_commit(), NAPI_AUTO_LENGTH, &result);
  return result;
}

static napi_value hostApiVersion(napi_env env, napi_callback_info info) {
  napi_value result;
  (void)info;
  napi_create_int32(env, hnui_api_version(), &result);
  return result;
}

/** `frameTargetAvailable()` -> what `hnui_frame_target` answers right now,
    for test/ui-exit-paths.js: false outside a frame callback, and false
    once the window is CLOSING even when called from inside one. */
static napi_value frameTargetAvailable(napi_env env, napi_callback_info info) {
  napi_value result;
  hnui_target target;
  hnui_window *w = hnui_get_window(env);
  (void)info;
  memset(&target, 0, sizeof(target));
  target.size = (uint32_t)sizeof(target);
  napi_get_boolean(
      env, w != NULL && hnui_frame_target(w, &target), &result);
  return result;
}

static napi_value hasWindow(napi_env env, napi_callback_info info) {
  napi_value result;
  (void)info;
  napi_get_boolean(env, hnui_get_window(env) != NULL, &result);
  return result;
}

NAPI_MODULE_INIT() {
  /* First, and the whole point of the second build of this file in
     test/ui-version-check.js: a host and an addon that disagree about the
     ABI or about Sokol's layout are caught here, before anything else runs.
     Returns NULL from this function with an error pending on a mismatch. */
  HNUI_CHECK_VERSION(env);
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
      {"lastTarget", NULL, lastTarget, NULL, NULL, NULL, napi_default, NULL},
      {"hasWindow", NULL, hasWindow, NULL, NULL, NULL, napi_default, NULL},
      {"frameTargetAvailable",
       NULL,
       frameTargetAvailable,
       NULL,
       NULL,
       NULL,
       napi_default,
       NULL},
      {"startEventLog",
       NULL,
       startEventLog,
       NULL,
       NULL,
       NULL,
       napi_default,
       NULL},
      {"logLine", NULL, logLine, NULL, NULL, NULL, napi_default, NULL},
      {"armCloseCancel",
       NULL,
       armCloseCancel,
       NULL,
       NULL,
       NULL,
       napi_default,
       NULL},
      {"cancelCloseNow",
       NULL,
       cancelCloseNow,
       NULL,
       NULL,
       NULL,
       napi_default,
       NULL},
      {"takeEventLog",
       NULL,
       takeEventLog,
       NULL,
       NULL,
       NULL,
       napi_default,
       NULL},
      {"panicThroughProducerLogger",
       NULL,
       panicThroughProducerLogger,
       NULL,
       NULL,
       NULL,
       napi_default,
       NULL},
      {"requestInvalidPassFrame",
       NULL,
       requestInvalidPassFrame,
       NULL,
       NULL,
       NULL,
       napi_default,
       NULL},
      {"callStateOffThread",
       NULL,
       callStateOffThread,
       NULL,
       NULL,
       NULL,
       napi_default,
       NULL},
      {"getWindowState",
       NULL,
       getWindowState,
       NULL,
       NULL,
       NULL,
       napi_default,
       NULL},
      {"probeNullWindow",
       NULL,
       probeNullWindow,
       NULL,
       NULL,
       NULL,
       napi_default,
       NULL},
      {"requestNativeFrame",
       NULL,
       requestNativeFrame,
       NULL,
       NULL,
       NULL,
       napi_default,
       NULL},
      {"cancelNativeFrame",
       NULL,
       cancelNativeFrame,
       NULL,
       NULL,
       NULL,
       napi_default,
       NULL},
      {"readClipboardSync",
       NULL,
       readClipboardSync,
       NULL,
       NULL,
       NULL,
       napi_default,
       NULL},
      {"probeSizes",
       NULL,
       probeSizes,
       NULL,
       NULL,
       NULL,
       napi_default,
       NULL},
      {"takeSizeProbe",
       NULL,
       takeSizeProbe,
       NULL,
       NULL,
       NULL,
       napi_default,
       NULL},
      {"probeCommands",
       NULL,
       probeCommands,
       NULL,
       NULL,
       NULL,
       napi_default,
       NULL},
      {"startRemovalProbe",
       NULL,
       startRemovalProbe,
       NULL,
       NULL,
       NULL,
       napi_default,
       NULL},
      {"requestQuiescenceRead",
       NULL,
       requestQuiescenceRead,
       NULL,
       NULL,
       NULL,
       napi_default,
       NULL},
      {"hostSokolCommit",
       NULL,
       hostSokolCommit,
       NULL,
       NULL,
       NULL,
       napi_default,
       NULL},
      {"hostApiVersion",
       NULL,
       hostApiVersion,
       NULL,
       NULL,
       NULL,
       napi_default,
       NULL},
  };
  napi_define_properties(
      env, exports, sizeof(descs) / sizeof(descs[0]), descs);
  demoDefineVisualExports(env, exports);
  demoDefineConformanceExports(env, exports);
  return exports;
}
