/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/**
 * The reference addon's conformance mode (spec: The reference addon): the
 * entry points the visual demo does not call, each called once, and one
 * line per observation.
 *
 * Driven from demo.js's `conformance` mode, which owns the sequencing --
 * events to inject, frames to wait for, a clipboard owner to start -- and
 * prints these lines in a fixed order for test/ui-conformance.js to match.
 * Every line begins with the entry point it is about, and reports what was
 * OBSERVED: a returned value, a status, a count read back. The four void
 * commands have nothing to return, so their lines say only what was issued;
 * what the platform thread applied is in the trace, which the test reads.
 *
 * Lines produced inside callbacks -- a frame callback, an event listener --
 * are queued here and collected with `conformanceTakeLines()` once the
 * callback has run, so their order is the order they happened in.
 */

#include "demo_addon.h"

#include <hnui.h>
#include <node_api.h>
#include <sokol_gfx.h>

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Lines                                                               */
/* ------------------------------------------------------------------ */

#define LINE_MAX_COUNT 32
#define LINE_BYTES 256

static char g_lines[LINE_MAX_COUNT][LINE_BYTES];
static int g_lineCount = 0;

static void addLine(const char *line) {
  if (g_lineCount < LINE_MAX_COUNT) {
    snprintf(g_lines[g_lineCount], LINE_BYTES, "%s", line);
    ++g_lineCount;
  }
}

static napi_value makeString(napi_env env, const char *text) {
  napi_value value;
  napi_create_string_utf8(env, text, NAPI_AUTO_LENGTH, &value);
  return value;
}

/** `conformanceTakeLines()` -> the queued lines, oldest first. */
static napi_value takeLines(napi_env env, napi_callback_info info) {
  napi_value result;
  int i;
  (void)info;
  napi_create_array_with_length(env, (size_t)g_lineCount, &result);
  for (i = 0; i < g_lineCount; ++i) {
    napi_set_element(env, result, (uint32_t)i, makeString(env, g_lines[i]));
  }
  g_lineCount = 0;
  return result;
}

/* ------------------------------------------------------------------ */
/* hnui_api_version, hnui_get_window_state                             */
/* ------------------------------------------------------------------ */

static napi_value apiVersion(napi_env env, napi_callback_info info) {
  char line[LINE_BYTES];
  (void)info;
  snprintf(
      line,
      sizeof(line),
      "hnui_api_version %d (compiled against %d)",
      hnui_api_version(),
      (int)HNUI_ABI_VERSION);
  return makeString(env, line);
}

static napi_value windowState(napi_env env, napi_callback_info info) {
  char line[LINE_BYTES];
  hnui_window_state state;

  (void)info;
  memset(&state, 0, sizeof(state));
  state.size = (uint32_t)sizeof(state);
  hnui_get_window_state(hnui_get_window(env), &state);
  snprintf(
      line,
      sizeof(line),
      "hnui_get_window_state width=%d height=%d framebuffer=%dx%d "
      "dpi_scale=%g focused=%d fullscreen=%d minimized=%d",
      (int)state.width,
      (int)state.height,
      (int)state.framebuffer_width,
      (int)state.framebuffer_height,
      state.dpi_scale,
      state.focused ? 1 : 0,
      state.fullscreen ? 1 : 0,
      state.minimized ? 1 : 0);
  return makeString(env, line);
}

/* ------------------------------------------------------------------ */
/* One frame through the resolve view                                  */
/* ------------------------------------------------------------------ */

/**
 * Render one frame the way an MSAA renderer does: into a multisampled image
 * of the addon's own, resolved into the slot through `resolve_view`. The
 * slot itself is single-sampled (`sample_count` 1), so the colour view is
 * NOT what an MSAA pass attaches as its colour attachment -- the addon's
 * image is -- and the slot appears only as the resolve target.
 *
 * The image is created inside the frame callback, as v1's resource
 * contract requires, and destroyed at its end: it is needed for exactly
 * one frame. A renderer doing this every frame would keep it, and remake
 * it when the target's size or generation changed.
 *
 * The pass is a flat clear, which is enough: every sample holds the same
 * value, so the resolve's average is that value, and the capture can be
 * compared exactly.
 */
static void msaaFrame(hnui_window *w, void *user, double timestampMs) {
  char line[LINE_BYTES];
  hnui_target target;
  sg_image_desc imageDesc;
  sg_view_desc viewDesc;
  sg_image image;
  sg_view view;
  sg_pass pass;

  (void)user;
  (void)timestampMs;
  memset(&target, 0, sizeof(target));
  target.size = (uint32_t)sizeof(target);
  if (!hnui_frame_target(w, &target)) {
    addLine("hnui_frame_target returned false inside a frame callback");
    return;
  }
  snprintf(
      line,
      sizeof(line),
      "hnui_frame_target size=%dx%d sample_count=%d color_view_nonzero=%d "
      "resolve_view_nonzero=%d distinct=%d",
      (int)target.width,
      (int)target.height,
      (int)target.sample_count,
      target.color_view.id != 0,
      target.resolve_view.id != 0,
      target.color_view.id != target.resolve_view.id);
  addLine(line);

  memset(&imageDesc, 0, sizeof(imageDesc));
  imageDesc.type = SG_IMAGETYPE_2D;
  imageDesc.width = target.width;
  imageDesc.height = target.height;
  imageDesc.num_mipmaps = 1;
  /* The resolve source and destination must share a format: the target
     reports its own. */
  imageDesc.pixel_format = (sg_pixel_format)target.color_format;
  imageDesc.sample_count = 4;
  imageDesc.usage.color_attachment = true;
  imageDesc.label = "hnui-demo-msaa";
  image = sg_make_image(&imageDesc);

  memset(&viewDesc, 0, sizeof(viewDesc));
  viewDesc.color_attachment.image = image;
  viewDesc.label = "hnui-demo-msaa-color";
  view = sg_make_view(&viewDesc);

  memset(&pass, 0, sizeof(pass));
  pass.attachments.colors[0] = view;
  pass.attachments.resolves[0].id = target.resolve_view.id;
  pass.action.colors[0].load_action = SG_LOADACTION_CLEAR;
  /* The multisampled contents are not needed once resolved. */
  pass.action.colors[0].store_action = SG_STOREACTION_DONTCARE;
  /* MAGENTA: 0 and 1 only, for the exact comparison. */
  pass.action.colors[0].clear_value.r = 1.0f;
  pass.action.colors[0].clear_value.g = 0.0f;
  pass.action.colors[0].clear_value.b = 1.0f;
  pass.action.colors[0].clear_value.a = 1.0f;
  sg_begin_pass(&pass);
  sg_end_pass();

  snprintf(
      line,
      sizeof(line),
      "hnui_frame_target msaa image sample_count=%d size=%dx%d resolved into "
      "resolve_view",
      sg_query_image_sample_count(image),
      sg_query_image_width(image),
      sg_query_image_height(image));
  addLine(line);

  sg_destroy_view(view);
  sg_destroy_image(image);
}

static napi_value requestMsaaFrame(napi_env env, napi_callback_info info) {
  napi_value result;
  (void)info;
  napi_create_int32(
      env,
      hnui_request_animation_frame(hnui_get_window(env), msaaFrame, NULL),
      &result);
  return result;
}

/* ------------------------------------------------------------------ */
/* hnui_cancel_animation_frame                                         */
/* ------------------------------------------------------------------ */

/**
 * Three callbacks for the same tick, in registration order: `second` is
 * cancelled before the tick; `third` is cancelled by `first` while the tick
 * is running, which must stop it too ("one waiting for the next tick, or one
 * later in the tick now running"); and an id never issued is cancelled for
 * good measure, which must do nothing at all. Only `first` may run.
 */
static char g_ran[64];
static int g_thirdId = 0;

static void noteRan(const char *name) {
  size_t used = strlen(g_ran);
  snprintf(
      g_ran + used, sizeof(g_ran) - used, "%s%s", used > 0 ? "," : "", name);
}

static void cancelFirst(hnui_window *w, void *user, double timestampMs) {
  (void)user;
  (void)timestampMs;
  noteRan("first");
  hnui_cancel_animation_frame(w, g_thirdId);
}

static void cancelSecond(hnui_window *w, void *user, double timestampMs) {
  (void)w;
  (void)user;
  (void)timestampMs;
  noteRan("second");
}

static void cancelThird(hnui_window *w, void *user, double timestampMs) {
  (void)w;
  (void)user;
  (void)timestampMs;
  noteRan("third");
}

/** `conformanceStartCancel()` -> the ids line. Wait one frame, then read
    `conformanceCancelResult()`. */
static napi_value startCancel(napi_env env, napi_callback_info info) {
  char line[LINE_BYTES];
  hnui_window *w = hnui_get_window(env);
  int first;
  int second;

  (void)info;
  g_ran[0] = '\0';
  first = hnui_request_animation_frame(w, cancelFirst, NULL);
  second = hnui_request_animation_frame(w, cancelSecond, NULL);
  g_thirdId = hnui_request_animation_frame(w, cancelThird, NULL);
  hnui_cancel_animation_frame(w, second);
  hnui_cancel_animation_frame(w, 0x7FFFFFFF);
  snprintf(
      line,
      sizeof(line),
      "hnui_cancel_animation_frame ids nonzero and distinct=%d",
      first > 0 && second > 0 && g_thirdId > 0 && first != second &&
          second != g_thirdId && first != g_thirdId);
  return makeString(env, line);
}

static napi_value cancelResult(napi_env env, napi_callback_info info) {
  char line[LINE_BYTES];
  (void)info;
  snprintf(
      line,
      sizeof(line),
      "hnui_cancel_animation_frame ran=%s",
      g_ran[0] != '\0' ? g_ran : "(none)");
  return makeString(env, line);
}

/* ------------------------------------------------------------------ */
/* hnui_remove_event_listener, hnui_event_dispatch_quiescent           */
/* ------------------------------------------------------------------ */

/**
 * Listener `remover` is registered first and removes `removed` while the
 * first key event is being delivered. `removed` is later in that event's
 * list, so it still receives that event -- and no later one.
 *
 * `removed`'s user data is heap memory, and the one question this exists
 * to answer is when it may be freed: not from `remover` (a removed listener
 * may still be about to run for the event in flight), but once
 * `hnui_event_dispatch_quiescent` says no listener is on the stack -- here,
 * in the next frame callback. Under ASAN, freeing it any earlier and then
 * having the listener run would be a use-after-free report rather than a
 * wrong number.
 *
 * Only KEY_DOWN is counted: the X server may deliver focus or mouse events
 * while these are registered, and they are not what is being tested.
 */
typedef struct RemovedListenerData {
  int received;
  int lastKey;
} RemovedListenerData;

static RemovedListenerData *g_removedData = NULL;
static int g_removedId = 0;
static int g_removerId = 0;
static bool g_removalDone = false;
static char g_removerKeys[64];
static char g_quiescentInListener[32];

static void appendInt(char *buffer, size_t size, int value) {
  size_t used = strlen(buffer);
  snprintf(buffer + used, size - used, "%s%d", used > 0 ? "," : "", value);
}

static void
removerListener(hnui_window *w, void *user, const hnui_event *event) {
  (void)user;
  if (event->type != HNUI_EVENT_KEY_DOWN) {
    return;
  }
  appendInt(g_removerKeys, sizeof(g_removerKeys), (int)event->key_code);
  appendInt(
      g_quiescentInListener,
      sizeof(g_quiescentInListener),
      hnui_event_dispatch_quiescent(w) ? 1 : 0);
  if (!g_removalDone) {
    g_removalDone = true;
    hnui_remove_event_listener(w, g_removedId);
  }
}

static void
removedListener(hnui_window *w, void *user, const hnui_event *event) {
  RemovedListenerData *data = (RemovedListenerData *)user;
  (void)w;
  if (event->type != HNUI_EVENT_KEY_DOWN) {
    return;
  }
  ++data->received;
  data->lastKey = (int)event->key_code;
}

/** `conformanceStartListeners()`; then inject two key presses. */
static napi_value startListeners(napi_env env, napi_callback_info info) {
  hnui_window *w = hnui_get_window(env);
  (void)info;
  g_removedData = (RemovedListenerData *)calloc(1, sizeof(*g_removedData));
  g_removerKeys[0] = '\0';
  g_quiescentInListener[0] = '\0';
  g_removalDone = false;
  g_removerId = hnui_add_event_listener(w, removerListener, NULL);
  g_removedId = hnui_add_event_listener(w, removedListener, g_removedData);
  return NULL;
}

static void quiescenceFrame(hnui_window *w, void *user, double timestampMs) {
  char line[LINE_BYTES];
  bool quiescent = hnui_event_dispatch_quiescent(w);

  (void)user;
  (void)timestampMs;
  snprintf(
      line,
      sizeof(line),
      "hnui_remove_event_listener removed listener received %d event(s), "
      "last key=%d",
      g_removedData->received,
      g_removedData->lastKey);
  addLine(line);
  snprintf(
      line,
      sizeof(line),
      "hnui_remove_event_listener remaining listener received keys=%s",
      g_removerKeys);
  addLine(line);
  snprintf(
      line,
      sizeof(line),
      "hnui_event_dispatch_quiescent inside listener=%s",
      g_quiescentInListener);
  addLine(line);
  if (quiescent) {
    free(g_removedData);
    g_removedData = NULL;
  }
  snprintf(
      line,
      sizeof(line),
      "hnui_event_dispatch_quiescent next frame callback=%d (%s)",
      quiescent ? 1 : 0,
      quiescent ? "removed listener's user data freed there"
                : "user data NOT freed");
  addLine(line);
  /* The remaining listener's job is done too. Its user data is NULL, so
     there is nothing to wait for before or after. */
  hnui_remove_event_listener(w, g_removerId);
}

static napi_value requestQuiescenceFrame(
    napi_env env,
    napi_callback_info info) {
  (void)info;
  hnui_request_animation_frame(hnui_get_window(env), quiescenceFrame, NULL);
  return NULL;
}

/* ------------------------------------------------------------------ */
/* hnui_cancel_close                                                   */
/* ------------------------------------------------------------------ */

/**
 * Called once outside any event, where it must answer false, and once from
 * a native listener while an injected QUIT_REQUESTED is delivered as
 * HNUI_EVENT_CLOSE_REQUESTED, where it must answer true -- and does
 * cancel: the run carries on, where an uncancelled close would end it.
 * Injection is enough here: what is observed is the entry point's own
 * bookkeeping, after the converter, not anything Sokol does.
 */
static int g_closeOutside = -1;
static int g_closeInside = -1;
static int g_closeListenerId = 0;

static void
closeCancelListener(hnui_window *w, void *user, const hnui_event *event) {
  (void)user;
  if (event->type != HNUI_EVENT_CLOSE_REQUESTED || g_closeInside != -1) {
    return;
  }
  g_closeInside = hnui_cancel_close(w) ? 1 : 0;
  /* Its job is done; removal is deferred, and the user data is NULL. */
  hnui_remove_event_listener(w, g_closeListenerId);
}

/** `conformanceStartCloseCancel()`; then inject a QUIT_REQUESTED. */
static napi_value startCloseCancel(napi_env env, napi_callback_info info) {
  hnui_window *w = hnui_get_window(env);
  (void)info;
  g_closeOutside = hnui_cancel_close(w) ? 1 : 0;
  g_closeInside = -1;
  g_closeListenerId = hnui_add_event_listener(w, closeCancelListener, NULL);
  return NULL;
}

/** `conformanceCloseCancelResult()` -> one line. */
static napi_value closeCancelResult(napi_env env, napi_callback_info info) {
  char line[LINE_BYTES];
  (void)info;
  snprintf(
      line,
      sizeof(line),
      "hnui_cancel_close outside=%d inside=%d",
      g_closeOutside,
      g_closeInside);
  return makeString(env, line);
}

/* ------------------------------------------------------------------ */
/* The four void commands                                              */
/* ------------------------------------------------------------------ */

/** `conformanceCommands()` -> one line per command issued. */
static napi_value commands(napi_env env, napi_callback_info info) {
  napi_value result;
  hnui_window *w = hnui_get_window(env);

  (void)info;
  hnui_set_cursor(w, HNUI_CURSOR_POINTING_HAND);
  hnui_show_cursor(w, false);
  hnui_show_cursor(w, true);
  hnui_lock_pointer(w, true);
  hnui_lock_pointer(w, false);

  napi_create_array_with_length(env, 3, &result);
  napi_set_element(
      env,
      result,
      0,
      makeString(env, "hnui_set_cursor HNUI_CURSOR_POINTING_HAND (4) issued"));
  napi_set_element(
      env, result, 1, makeString(env, "hnui_show_cursor 0 then 1 issued"));
  napi_set_element(
      env, result, 2, makeString(env, "hnui_lock_pointer 1 then 0 issued"));
  return result;
}

/* ------------------------------------------------------------------ */
/* The clipboard, and hnui_free                                        */
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

/** `conformanceWriteClipboard(text)` -> the line. */
static napi_value writeClipboard(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  char text[128];
  char line[LINE_BYTES];
  size_t length = 0;

  text[0] = '\0';
  napi_get_cb_info(env, info, &argc, argv, NULL, NULL);
  if (argc >= 1) {
    napi_get_value_string_utf8(env, argv[0], text, sizeof(text), &length);
  }
  hnui_write_clipboard(hnui_get_window(env), text);
  snprintf(line, sizeof(line), "hnui_write_clipboard \"%s\" issued", text);
  return makeString(env, line);
}

/**
 * `conformanceReadClipboard(timeoutMs, label)` -> two lines: what
 * `hnui_read_clipboard_sync` answered, then what was handed to `hnui_free`.
 * On `HNUI_OK` that is the copy the read allocated; on any other status
 * `*out` must be NULL, and NULL is what is freed -- which `hnui_free`
 * accepts.
 */
static napi_value readClipboard(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  int32_t timeoutMs = 0;
  char label[64];
  size_t length = 0;
  char *text = (char *)"<untouched>";
  char line[LINE_BYTES];
  napi_value result;
  int status;

  label[0] = '\0';
  napi_get_cb_info(env, info, &argc, argv, NULL, NULL);
  if (argc >= 1) {
    napi_get_value_int32(env, argv[0], &timeoutMs);
  }
  if (argc >= 2) {
    napi_get_value_string_utf8(env, argv[1], label, sizeof(label), &length);
  }

  status = hnui_read_clipboard_sync(hnui_get_window(env), timeoutMs, &text);
  napi_create_array_with_length(env, 2, &result);
  if (status == HNUI_OK) {
    snprintf(
        line,
        sizeof(line),
        "hnui_read_clipboard_sync%s status=%s text=\"%s\"",
        label,
        statusName(status),
        text);
    napi_set_element(env, result, 0, makeString(env, line));
    hnui_free(text);
    napi_set_element(
        env, result, 1, makeString(env, "hnui_free released the HNUI_OK copy"));
  } else {
    snprintf(
        line,
        sizeof(line),
        "hnui_read_clipboard_sync%s status=%s out=%s",
        label,
        statusName(status),
        text == NULL ? "NULL" : "NOT NULL");
    napi_set_element(env, result, 0, makeString(env, line));
    /* Only ever NULL here; a non-NULL `*out` is reported above and leaked
       rather than freed, since it is not known to be the host's. */
    hnui_free(NULL);
    napi_set_element(
        env, result, 1, makeString(env, "hnui_free(NULL) returned"));
  }
  return result;
}

void demoDefineConformanceExports(napi_env env, napi_value exports) {
  napi_property_descriptor descs[] = {
      {"conformanceTakeLines",
       NULL,
       takeLines,
       NULL,
       NULL,
       NULL,
       napi_default,
       NULL},
      {"conformanceApiVersion",
       NULL,
       apiVersion,
       NULL,
       NULL,
       NULL,
       napi_default,
       NULL},
      {"conformanceWindowState",
       NULL,
       windowState,
       NULL,
       NULL,
       NULL,
       napi_default,
       NULL},
      {"conformanceRequestMsaaFrame",
       NULL,
       requestMsaaFrame,
       NULL,
       NULL,
       NULL,
       napi_default,
       NULL},
      {"conformanceStartCancel",
       NULL,
       startCancel,
       NULL,
       NULL,
       NULL,
       napi_default,
       NULL},
      {"conformanceCancelResult",
       NULL,
       cancelResult,
       NULL,
       NULL,
       NULL,
       napi_default,
       NULL},
      {"conformanceStartListeners",
       NULL,
       startListeners,
       NULL,
       NULL,
       NULL,
       napi_default,
       NULL},
      {"conformanceRequestQuiescenceFrame",
       NULL,
       requestQuiescenceFrame,
       NULL,
       NULL,
       NULL,
       napi_default,
       NULL},
      {"conformanceStartCloseCancel",
       NULL,
       startCloseCancel,
       NULL,
       NULL,
       NULL,
       napi_default,
       NULL},
      {"conformanceCloseCancelResult",
       NULL,
       closeCancelResult,
       NULL,
       NULL,
       NULL,
       napi_default,
       NULL},
      {"conformanceCommands",
       NULL,
       commands,
       NULL,
       NULL,
       NULL,
       napi_default,
       NULL},
      {"conformanceWriteClipboard",
       NULL,
       writeClipboard,
       NULL,
       NULL,
       NULL,
       napi_default,
       NULL},
      {"conformanceReadClipboard",
       NULL,
       readClipboard,
       NULL,
       NULL,
       NULL,
       napi_default,
       NULL},
  };
  napi_define_properties(env, exports, sizeof(descs) / sizeof(descs[0]), descs);
}
