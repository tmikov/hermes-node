/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ui_module.h"

#include "event_injector.h"
#include "platform_services.h"
#include "sapp_event_convert.h"
#include "ui_internal.h"

#include <backend.h>
#include <sokol_log.h>
#include <test_hooks.h>
#include <ui_trace.h>

#include <hermes/node-compat/ui/hnui_abi.h>

#include <hermes/node-compat/compositor/channels.h>
#include <hermes/node-compat/compositor/owner_dispatch.h>
#include <hermes/node-compat/compositor/surface_table.h>
#include <hermes/node-compat/compositor/tick_machine.h>

#include <sokol_app.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace hermes {
namespace node_compat {

//===========================================================================
// State
//===========================================================================
//
// The platform-thread half of the window: the sokol_app callbacks, the
// commands they apply, and the state only this thread touches. Shared state
// is declared in ui_internal.h.

namespace {

/// Platform thread only: what is on screen, and which generations this
/// thread holds wrappers for.
SurfaceRef gPresenting{};
bool gHavePresenting = false;
std::vector<GenerationId> gWrappedGenerations;

/// The platform thread's tracked window state: the `window` snapshot of
/// the last event it converted, refreshed from `sapp_*` queries before each
/// real event (the converter handles focus and minimize, not size).
/// Platform thread only.
WindowState gTracked;

/// Whether the tick machine has been told the tracked minimized state. An
/// ICONIFIED that arrives before `gPresentReady` has no machine to tell, so
/// the first frame after readiness catches it up. Platform thread only.
bool gTickMinimizedSynced = false;

/// The most the platform thread will wait for another client's selection
/// reply, whatever deadline the request carries. Every frame and every
/// event waits behind it, and a native caller may pass a deadline of a
/// minute; that caller gets kTimeout after this long instead.
constexpr int kClipboardReadCapMs = 2000;

/// Write a captured frame as a binary PPM (P6).
///
/// A deliberately dull format: a five-byte header and three bytes per
/// pixel, which a test can parse in a few lines of JavaScript with no
/// decoder. The alpha channel is dropped because the comparison is over
/// colour and the swapchain's alpha is not something the pattern controls.
bool writeCapturePpm(
    const std::string &path,
    int width,
    int height,
    const std::vector<uint8_t> &rgba) {
  std::FILE *file = std::fopen(path.c_str(), "wb");
  if (file == nullptr)
    return false;
  std::fprintf(file, "P6\n%d %d\n255\n", width, height);
  std::vector<uint8_t> rgb(static_cast<size_t>(width) * height * 3);
  for (size_t i = 0, n = static_cast<size_t>(width) * height; i < n; ++i) {
    rgb[i * 3 + 0] = rgba[i * 4 + 0];
    rgb[i * 3 + 1] = rgba[i * 4 + 1];
    rgb[i * 3 + 2] = rgba[i * 4 + 2];
  }
  const size_t written = std::fwrite(rgb.data(), 1, rgb.size(), file);
  std::fclose(file);
  return written == rgb.size();
}

/// Report what this frame actually presented, and take a capture if it is
/// the frame somebody asked for. Platform thread, once per presented
/// frame, immediately after the blit.
///
/// **Matching, not polling** (`test_hooks.h`): the presenter says what it
/// drew and finds out in the same call whether that was the frame
/// requested. Every field must agree -- and the one that does the work
/// here is `publicationId`, because a re-blit of the still-presenting
/// surface reports every other field identically.
void maybeCapture() {
  if (!uiTestHooksEnabled() || !gCapture.pending())
    return;

  CaptureRequest observed;
  observed.generation = gPresenting.gen;
  observed.publicationId = gTick->presentingPublicationId();
  // The presented surface's OWN size, keyed by its generation -- never
  // gGenerationWidth/Height, which the JS thread rewrites on a resize and
  // which would then describe a generation other than the one on screen
  // (and be a data race besides). The generation is alive here: this
  // thread holds its wrappers.
  int targetWidth = 0;
  int targetHeight = 0;
  if (gBackend->generationSize(gPresenting.gen, &targetWidth, &targetHeight)) {
    observed.targetWidth = targetWidth;
    observed.targetHeight = targetHeight;
  }
  observed.framebufferWidth = sapp_width();
  observed.framebufferHeight = sapp_height();
  observed.dpiScale = gMetrics.dpiScale;

  std::string path;
  int giveUpAfterFrames = 0;
  int framesObserved = 0;
  {
    std::lock_guard<std::mutex> lock(gCaptureMutex);
    if (gCaptureState.finished)
      return;
    path = gCaptureState.path;
    giveUpAfterFrames = gCaptureState.giveUpAfterFrames;
    framesObserved = ++gCaptureState.framesObserved;
  }

  if (gCapture.tryMatch(observed)) {
    const int width = observed.framebufferWidth;
    const int height = observed.framebufferHeight;
    std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4);
    const bool read =
        gBackend->readPresentedPixels(width, height, pixels.data());
    const bool wrote = read && writeCapturePpm(path, width, height, pixels);
    {
      std::lock_guard<std::mutex> lock(gCaptureMutex);
      gCaptureState.finished = true;
      gCaptureState.matched = wrote;
      gCaptureState.publicationId = observed.publicationId;
      gCaptureState.width = width;
      gCaptureState.height = height;
    }
    uiTrace(
        "capture: %s generation %u publication %llu (%dx%d)",
        wrote ? "written" : "FAILED",
        observed.generation.value,
        static_cast<unsigned long long>(observed.publicationId),
        width,
        height);
    // A refused post -- the dispatcher was closed by CLOSING -- leaves
    // __captureFrame()'s Promise unsettled. The JS thread has already
    // abandoned it (uiTestHooksAbandonCapture), so nothing waits on it; the
    // window is a match that was under way when CLOSING began.
    if (gDispatch == nullptr || !gDispatch->post(onCaptureFinished))
      uiTrace("capture: post refused (dispatcher closed)");
    return;
  }

  // Not this frame. A budget in FRAMES, not milliseconds: the suite runs
  // 16-way parallel and the plan forbids timing-based assertions, so a
  // test that needs to prove a request was NOT satisfied counts presented
  // frames instead of waiting a while.
  if (giveUpAfterFrames > 0 && framesObserved >= giveUpAfterFrames) {
    gCapture.cancel();
    {
      std::lock_guard<std::mutex> lock(gCaptureMutex);
      gCaptureState.finished = true;
      gCaptureState.matched = false;
    }
    uiTrace(
        "capture: no match after %d presented frame(s); publication %llu "
        "was presenting",
        framesObserved,
        static_cast<unsigned long long>(observed.publicationId));
    // As above: a refused post leaves the abandoned Promise unsettled.
    if (gDispatch == nullptr || !gDispatch->post(onCaptureFinished))
      uiTrace("capture: post refused (dispatcher closed)");
  }
}

/// Destroy the presenting instance's wrappers for every generation that
/// has stopped needing them.
///
/// **On a trigger, not by waiting** (spec: Generations). Waiting for the
/// reference count to reach zero would deadlock, since the wrappers are
/// themselves counted owners; `wrappersReleasable()` is the trigger, and
/// it is asked only about generations this thread actually holds wrappers
/// for.
void releaseObsoleteWrappers() {
  for (size_t i = 0; i < gWrappedGenerations.size();) {
    const GenerationId gen = gWrappedGenerations[i];
    if (gTable->wrappersReleasable(gen)) {
      gBackend->releaseWrappers(gen);
      gTable->releaseWrappers(gen);
      gWrappedGenerations.erase(
          gWrappedGenerations.begin() + static_cast<long>(i));
    } else {
      ++i;
    }
  }
}

//===========================================================================
// Platform thread: Sokol callbacks
//===========================================================================

/// Sokol's logger, and the fatal half of the spec's "Failure splits in
/// two".
///
/// Window and context creation failing is fatal **by contract**: Sokol
/// routes it to `_SAPP_PANIC` from inside `sapp_run()`, before `init_cb`
/// ever runs, and there is no recovery path. Returning from a logger does
/// not make Sokol stop -- it carries on into the state the panic was about
/// -- so this terminates instead.
void sokolLog(
    const char *tag,
    uint32_t logLevel,
    uint32_t logItemId,
    const char *message,
    uint32_t lineNr,
    const char *filename,
    void *userData) {
  (void)userData;
  // A panic does not return: uiSokolLog restores the terminal and exits,
  // the same path both sokol_gfx instances' loggers take.
  uiSokolLog(
      tag ? tag : "sokol",
      logLevel,
      logItemId,
      message,
      lineNr,
      filename,
      "hermes-node-ui: the windowing system could not be initialised. This "
      "is fatal: sokol_app panics from inside sapp_run(), before any "
      "callback of ours runs, and offers no way to recover.");
}

/// Bring the tracked state's size, DPI scale and fullscreen flag up to date
/// from Sokol. The converter handles what an event itself changes --
/// focus, minimize -- and copies the rest from its source, so a RESIZED
/// that was not preceded by this would report the old size.
///
/// On Linux sokol_app maps window size to framebuffer size 1:1
/// (sokol_app.h, _sapp_linux_run), so sapp_width() is both. Divided by the
/// DPI scale anyway rather than assumed equal: the logical/physical split
/// is real on every other platform this interface will meet, and a
/// framebuffer size that silently doubles as a logical one is the kind of
/// thing that reads correctly right up until it does not.
void refreshTrackedState() {
  const float dpiScale = sapp_dpi_scale();
  gTracked.framebufferWidth = sapp_width();
  gTracked.framebufferHeight = sapp_height();
  gTracked.dpiScale = dpiScale;
  gTracked.width = dpiScale > 0.0f ? static_cast<int>(sapp_widthf() / dpiScale)
                                   : sapp_width();
  gTracked.height = dpiScale > 0.0f
      ? static_cast<int>(sapp_heightf() / dpiScale)
      : sapp_height();
  gTracked.fullscreen = sapp_is_fullscreen();
}

const char *clipboardStatusName(ClipboardStatus status) {
  switch (status) {
    case ClipboardStatus::kOk:
      return "ok";
    case ClipboardStatus::kUnavailable:
      return "unavailable";
    case ClipboardStatus::kTimeout:
      return "timeout";
    case ClipboardStatus::kTooLarge:
      return "too-large";
    case ClipboardStatus::kError:
      return "error";
  }
  return "error";
}

/// Read the clipboard now, waiting at most \p timeoutMs for another
/// client's reply. Platform thread only.
///
/// Through the platform's own read (`platform_services.h`), never
/// `sapp_get_clipboard_string()`, whose failures return its previous
/// buffer. One byte under the ABI's limit, which counts the terminator.
ClipboardResult readClipboardNow(int timeoutMs) {
  return uiPlatformReadClipboard(timeoutMs, HNUI_CLIPBOARD_MAX_BYTES - 1);
}

/// A PlatformSource over the real platform, as of now.
PlatformSource realPlatformSource() {
  PlatformSource source;
  source.window = gTracked;
  // A paste carries the text read when it happened, through the same read
  // `readClipboard()` uses. A read that did not succeed means the paste is
  // not forwarded (the converter refuses it) -- an empty-but-successful
  // read still is, since pasting nothing is a real paste.
  source.readClipboard = [](std::string &out) {
    ClipboardResult result = readClipboardNow(kClipboardReadDefaultTimeoutMs);
    if (result.status != ClipboardStatus::kOk) {
      uiTrace(
          "paste: clipboard read %s; not forwarded",
          clipboardStatusName(result.status));
      return false;
    }
    out = std::move(result.text);
    return true;
  };
  source.droppedPaths = [] {
    std::vector<std::string> paths;
    const int count = sapp_get_num_dropped_files();
    for (int i = 0; i < count; ++i)
      paths.emplace_back(sapp_get_dropped_file_path(i));
    return paths;
  };
  return source;
}

/// Hand every injected event to uiHandlePlatformEvent(), with the injected
/// clipboard, paths and window substituted where present.
///
/// **One post for the whole batch.** The flag is held for the duration
/// (taken only once there is a first event, so an empty frame never holds
/// it), so no dispatch is posted until every event of the batch is queued,
/// and a batch that a test injected together is delivered by one dispatch
/// -- which is what lets test/ui-events.js tell "promise callbacks ran
/// between two events" apart from "they ran after the dispatch". Two
/// cases still split a batch, and neither loses anything: a dispatch
/// already outstanding when the batch began may run mid-batch (the events
/// after it then post for themselves, the ordinary path), and one already
/// running on the JS thread drains events as they land.
void drainInjectedEvents() {
  InjectedPlatformEvent injected;
  if (!gInjector.takeNext(&injected))
    return;
  const bool batched = !gDispatchPosted.exchange(true);

  do {
    refreshTrackedState();
    PlatformSource source = realPlatformSource();
    if (injected.hasClipboard) {
      source.readClipboard = [&injected](std::string &out) {
        out = injected.clipboard;
        return true;
      };
    }
    // Always substituted: Sokol's drop buffer holds nothing for an event
    // Sokol did not produce, and sapp_get_dropped_file_path() asserts on
    // an index it did not hand out.
    source.droppedPaths = [&injected] { return injected.paths; };
    if (injected.hasWindow)
      source.window = injected.window;
    uiHandlePlatformEvent(injected.event, source);
  } while (gInjector.takeNext(&injected));

  if (!batched)
    return;
  // Released, then the queue is checked -- not "did this batch push
  // anything". While the flag was held, any postEventDispatch() returned
  // without posting, on the strength of this holder; checking the queue is
  // what keeps that promise. A batch whose every event the converter
  // refused is covered by the same check.
  gDispatchPosted.store(false);
  if (!gInput.empty())
    postEventDispatch();
}

//===========================================================================
// Platform commands
//===========================================================================

/// The one place an `hnui_cursor` becomes a Sokol cursor. Explicit rather
/// than a cast: the two enumerations are named alike and happen to line up
/// today, and nothing but this table keeps them honest when either moves.
sapp_mouse_cursor sappCursorFor(int cursor) {
  switch (static_cast<hnui_cursor>(cursor)) {
    case HNUI_CURSOR_DEFAULT:
      return SAPP_MOUSECURSOR_DEFAULT;
    case HNUI_CURSOR_ARROW:
      return SAPP_MOUSECURSOR_ARROW;
    case HNUI_CURSOR_IBEAM:
      return SAPP_MOUSECURSOR_IBEAM;
    case HNUI_CURSOR_CROSSHAIR:
      return SAPP_MOUSECURSOR_CROSSHAIR;
    case HNUI_CURSOR_POINTING_HAND:
      return SAPP_MOUSECURSOR_POINTING_HAND;
    case HNUI_CURSOR_RESIZE_EW:
      return SAPP_MOUSECURSOR_RESIZE_EW;
    case HNUI_CURSOR_RESIZE_NS:
      return SAPP_MOUSECURSOR_RESIZE_NS;
    case HNUI_CURSOR_RESIZE_NWSE:
      return SAPP_MOUSECURSOR_RESIZE_NWSE;
    case HNUI_CURSOR_RESIZE_NESW:
      return SAPP_MOUSECURSOR_RESIZE_NESW;
    case HNUI_CURSOR_RESIZE_ALL:
      return SAPP_MOUSECURSOR_RESIZE_ALL;
    case HNUI_CURSOR_NOT_ALLOWED:
      return SAPP_MOUSECURSOR_NOT_ALLOWED;
    case HNUI_CURSOR_FORCE_I32:
      break;
  }
  return SAPP_MOUSECURSOR_DEFAULT;
}

/// A `Command::readClipboard`. Platform thread.
///
/// The read is bounded by the request's remaining deadline and by
/// kClipboardReadCapMs. **Nothing here waits on the JS thread**: a sync
/// read's answer is written into its slot from this thread, because the JS
/// thread is blocked on that slot and could never drain a queued reply.
void applyReadClipboard(const Command &command) {
  const uint64_t id = command.requestId();
  const bool viaSlot = command.replyThroughSlot();
  const char *path = viaSlot ? "sync" : "promise";

  ClipboardResult result;
  const uint64_t now = clipboardClockNs();
  if (now >= command.deadlineNs()) {
    // Its waiter has already given up (or is about to): reading would block
    // this thread for an answer nobody takes. A Promise still has to
    // settle, so it is told kTimeout; a sync waiter needs nothing.
    uiTrace(
        "clipboard read %s #%llu: deadline passed; skipped",
        path,
        static_cast<unsigned long long>(id));
    if (viaSlot)
      return;
    result.status = ClipboardStatus::kTimeout;
  } else {
    const uint64_t remainingNs = command.deadlineNs() - now;
    // Rounded up: a deadline 0.4 ms away is a short wait, not none.
    const uint64_t remainingMs = (remainingNs + 999999) / 1000000;
    const int timeoutMs = static_cast<int>(std::min<uint64_t>(
        remainingMs, static_cast<uint64_t>(kClipboardReadCapMs)));
    result = readClipboardNow(timeoutMs);
    uiTrace(
        "clipboard read %s #%llu: status=%s bytes=%zu",
        path,
        static_cast<unsigned long long>(id),
        clipboardStatusName(result.status),
        result.text.size());
  }

  if (viaSlot) {
    if (!gClipboardReads.answer(id, std::move(result))) {
      uiTrace(
          "clipboard read sync #%llu: waiter gone; reply discarded",
          static_cast<unsigned long long>(id));
    }
    return;
  }

  UiEvent reply;
  reply.type = UiEventType::kClipboardReply;
  reply.requestId = id;
  reply.clipboardStatus = result.status;
  reply.text = std::move(result.text);
  gInput.push(reply);
  postEventDispatch();
}

/// Apply one command with the matching sapp_* call. Platform thread only.
/// **Each trace prints the value read back afterwards**, never the
/// request, so a trace cannot pass with its call removed.
void applyCommand(const Command &command) {
  switch (command.kind()) {
    case Command::Kind::kSetTitle: {
      sapp_set_window_title(command.title().c_str());
      // The read-backs below exist for the trace and run only when it is on:
      // each is a synchronous round trip to the window system.
      if (!uiTracingEnabled())
        break;
      std::string name;
      if (uiPlatformWindowTitle(name))
        uiTrace("command title: readback='%s'", name.c_str());
      else
        uiTrace("command title: no readback available");
      break;
    }
    case Command::Kind::kSetFullscreen:
      // sapp_toggle_fullscreen() toggles; compare first.
      if (sapp_is_fullscreen() != command.flag())
        sapp_toggle_fullscreen();
      uiTrace("command fullscreen: is_fullscreen=%d", sapp_is_fullscreen());
      break;
    case Command::Kind::kSetIcon: {
      sapp_icon_desc icon = {};
      icon.images[0].width = command.width();
      icon.images[0].height = command.height();
      icon.images[0].pixels.ptr = command.cursorRgba().data();
      icon.images[0].pixels.size = command.cursorRgba().size();
      sapp_set_icon(&icon);
      if (!uiTracingEnabled())
        break;
      uiTrace("command icon: %s", uiPlatformDescribeIcon().c_str());
      break;
    }
    case Command::Kind::kSetCursor: {
      sapp_set_mouse_cursor(sappCursorFor(command.cursor()));
      uiTrace(
          "command cursor %d: sapp_mouse_cursor=%d",
          command.cursor(),
          static_cast<int>(sapp_get_mouse_cursor()));
      break;
    }
    case Command::Kind::kSetCursorImage: {
      sapp_image_desc image = {};
      image.width = command.width();
      image.height = command.height();
      image.cursor_hotspot_x = command.hotspotX();
      image.cursor_hotspot_y = command.hotspotY();
      image.pixels.ptr = command.cursorRgba().data();
      image.pixels.size = command.cursorRgba().size();
      // sapp_bind_mouse_cursor_image returns the cursor it was given whether
      // or not binding worked, so its result is not traced: only the
      // selection is read back, and the bind itself is unverified.
      sapp_bind_mouse_cursor_image(SAPP_MOUSECURSOR_CUSTOM_0, &image);
      sapp_set_mouse_cursor(SAPP_MOUSECURSOR_CUSTOM_0);
      uiTrace(
          "command custom cursor: sapp_mouse_cursor=%d",
          static_cast<int>(sapp_get_mouse_cursor()));
      break;
    }
    case Command::Kind::kShowCursor:
      sapp_show_mouse(command.flag());
      uiTrace("command show cursor: sapp_mouse_shown=%d", sapp_mouse_shown());
      break;
    case Command::Kind::kLockPointer:
      sapp_lock_mouse(command.flag());
      uiTrace(
          "command lock pointer: sapp_mouse_locked=%d", sapp_mouse_locked());
      break;
    case Command::Kind::kReadClipboard:
      applyReadClipboard(command);
      break;
    case Command::Kind::kWriteClipboard: {
      uiPlatformWriteClipboard(command.text());
      if (!uiTracingEnabled())
        break;
      uiTrace(
          "command clipboard: %s",
          uiPlatformDescribeClipboardWrite(command.text().size()).c_str());
      break;
    }
  }
}

void drainCommands() {
  std::vector<Command> commands;
  gCommands.drain(commands);
  for (const Command &command : commands)
    applyCommand(command);
}

void onInit() {
  std::string error;
  if (!gBackend->createSharedContext(error)) {
    // The recoverable half. The main loop keeps running -- this returns
    // into it -- so that the rejection reaches JS rather than racing a
    // process teardown.
    gStartupError = error;
    // Refused once the JS thread is CLOSING: the rejection is never
    // delivered, and this thread presents its clear colour until _exit().
    if (gDispatch == nullptr || !gDispatch->post(onStartupFailed))
      uiTrace("init_cb: post refused (dispatcher closed)");
    return;
  }

  // The window exists and the backend is set up: whatever the platform
  // still owes the window (macOS: putting it on screen without activating
  // the application, under the test hooks) happens now.
  uiPlatformAfterInit();

  refreshTrackedState();
  gMetrics = gTracked;

  uiTrace("init_cb: shared context ready");
  // Refused once the JS thread is CLOSING: the window's readiness is never
  // delivered, gPresentReady stays false, and this thread presents its
  // clear colour until _exit().
  if (gDispatch == nullptr || !gDispatch->post(onSharedContextReady))
    uiTrace("init_cb: post refused (dispatcher closed)");
}

/// The presenting half, once per platform frame: take a newly published
/// surface if there is one, blit it or the previous one, release completed
/// reads, and send the frame tick if one is due (spec: Architecture,
/// `frame_cb`), and drain the command queue and the clipboard reads.
void onFrame() {
  // The test hooks' frame barrier (__waitPlatformFrames) counts every call,
  // whichever of the returns below it leaves by.
  struct FrameBarrierGuard {
    bool active;
    ~FrameBarrierGuard() {
      if (active)
        uiTestHookFrameEnded();
    }
  } frameBarrier{uiTestHooksEnabled()};
  if (frameBarrier.active)
    uiTestHookFrameBegan();

  if (gBackend == nullptr)
    return;

  // **Before the readiness check, and unconditionally.** This is where the
  // stage-2 completion probe is answered, and the worker is blocked
  // waiting for that answer before the window's Promise can resolve -- so
  // a frame callback that skipped it while "not ready yet" would deadlock
  // the very startup it is part of.
  gBackend->pollCompletions();

  // Commands from JavaScript, before the injected events: a test that
  // issues commands and then injects a marker relies on the marker never
  // overtaking them. Before the readiness check, like the completions, so
  // a command pushed early is applied by the first frame rather than left
  // behind.
  drainCommands();

  // Injected events, immediately after the completions. Before the
  // readiness check like them, so that an event injected early is queued
  // rather than left behind -- the JS side delivers nothing until its
  // window object exists anyway.
  if (uiTestHooksEnabled())
    drainInjectedEvents();

  // Exactly one acquisition per frame, whichever of the three presenting
  // calls below follows it.
  if (!gBackend->acquireFrameSwapchain()) {
    // No usable swapchain: present nothing, and release nothing, because
    // nothing was submitted. GL never reports this; macOS does, for an
    // obscured window.
    gBackend->presentInvalid();
    return;
  }

  if (!gPresentReady.load(std::memory_order_acquire)) {
    // The window exists from the moment sapp_run() starts, and the JS
    // thread has not finished binding its context yet. Something still
    // has to go on screen: _sapp_linux_frame swaps unconditionally after
    // this returns, so skipping the clear would present undefined
    // back-buffer contents rather than nothing.
    gBackend->presentClear();
    return;
  }

  // An ICONIFIED converted before readiness had no machine to tell.
  if (!gTickMinimizedSynced) {
    gTick->setMinimized(gTracked.minimized);
    gTickMinimizedSynced = true;
  }

  // The composed gate is used below for the tick; the take is composed in
  // the same way, and for the same reason -- takeFrame() moves the
  // machine's state under the table's own lock, so the presenter cannot
  // take a frame the machine has not finished publishing.
  std::optional<SurfaceRef> taken = gTick->takeFrame(*gTable);
  if (taken) {
    std::string error;
    if (!gBackend->wrapForPresenting(taken->gen, error))
      terminateGraphicsSession(error);
    if (std::find(
            gWrappedGenerations.begin(),
            gWrappedGenerations.end(),
            taken->gen) == gWrappedGenerations.end()) {
      // One wrapper registration per generation, matching the one set of
      // wrappers the backend created. Each is a counted owner, which is
      // what keeps the generation alive while this thread can still draw
      // from it.
      gTable->addWrapper(taken->gen);
      gWrappedGenerations.push_back(taken->gen);
    }
    gPresenting = *taken;
    gHavePresenting = true;
  }

  if (!gHavePresenting) {
    // Nothing has ever been published. Attachment contents are initially
    // undefined (sokol_gfx.h:3714), so the presenter samples nothing and
    // shows a clear colour instead.
    gBackend->presentClear();
  } else {
    // **Every blit takes a read reference before submission**, including
    // a re-present of an unchanged surface, and releases it when that
    // blit completes on the GPU (spec: Slot states, Blit). The release is
    // onConsumerComplete's job.
    gTable->addRead(gPresenting);
    gBackend->blit(gPresenting);
    maybeCapture();
  }

  releaseObsoleteWrappers();

  // **The composed overload**, not anyFreeSlot() followed by
  // shouldSendTick(bool): the free-slot reading and the tick transition
  // happen in one critical section, so the answer cannot go stale between
  // them. `shouldSendTick(&table)` would bind to the bool overload
  // through a pointer conversion and silently mean "a slot is always
  // free"; TickMachine deletes that overload so it cannot compile.
  //
  // **A refused post is not repaired.** shouldSendTick() CONSUMES the
  // request: a true return has already cleared `pending_` and moved the
  // machine to kTickQueued, so a post the dispatcher refuses leaves it
  // there. post() refuses only once the window is CLOSING has closed the
  // dispatcher, and from then on no tick can ever be delivered again, so
  // the machine's state is moot and TickMachine needs no entry point to
  // put the tick back. The edge is also nearly unreachable: CLOSING calls
  // SurfaceTable::beginClosing() before it closes the dispatcher, and a
  // closing table makes this gate answer no
  // (TickMachineTest.NoTickIsSentOnceTheTableIsClosing), so what remains is
  // a window of a few instructions between this thread's gate and its post.
  if (gTick->shouldSendTick(*gTable)) {
    if (gDispatch == nullptr || !gDispatch->post(runFrameTick))
      uiTrace("frame tick: post refused (dispatcher closed)");
  }
}

void onEvent(const sapp_event *event) {
  refreshTrackedState();
  uiHandlePlatformEvent(*event, realPlatformSource());
}

void onCleanup() {
  uiTrace("cleanup_cb");
}

} // namespace

void uiHandlePlatformEvent(
    const sapp_event &event,
    const PlatformSource &source) {
  // Every quit request is cancelled, unconditionally and BEFORE anything
  // else, because Sokol acts on its flag as soon as this callback returns --
  // and a quit Sokol acted on would end sapp_run(), whose Linux teardown
  // closes the X display the JS thread's GL context was created on while
  // that thread still has it current (on macOS the quit path exits 0 from
  // this thread instead, which cannot carry a status). That holds in
  // CLOSING too: a second close request arriving while 'exit' listeners
  // run must not be the one that tears the window down under them. Whether
  // the window closes is the JS thread's decision, made by the `close`
  // listeners this event is forwarded to, and carried out there by ending
  // the process (uiCloseWindow); the platform thread is never told.
  if (event.type == SAPP_EVENTTYPE_QUIT_REQUESTED) {
    sapp_cancel_quit();
    uiTrace("quit requested: cancelled, forwarded as close");
    // The test hooks' quit barrier (__waitPlatformQuitRequests) counts the
    // cancellation once it has been executed.
    if (uiTestHooksEnabled())
      uiTestHookQuitCancelled();
  }

  UiEvent converted;
  if (!convertSappEvent(event, source, &converted))
    return;
  gTracked = converted.window;

  // A minimized window receives no ticks (spec: Frames). Told here, on the
  // platform thread, before the event is even queued -- so by the time a
  // `minimize` listener requests a frame, the machine already withholds
  // it. Before readiness there is no machine; onFrame catches it up.
  if ((converted.type == UiEventType::kMinimize ||
       converted.type == UiEventType::kRestore) &&
      gPresentReady.load(std::memory_order_acquire)) {
    gTick->setMinimized(converted.window.minimized);
    gTickMinimizedSynced = true;
  }

  gInput.push(converted);
  postEventDispatch();
}

int uiRunPlatformLoop() {
  gBackend = createCompositorBackend();
  if (gBackend == nullptr) {
    std::fprintf(
        stderr,
        "hermes-node-ui: no GPU backend is available for this build.\n");
    return 1;
  }

  // Stage 1, and it must happen here rather than in init_cb: Sokol ranks
  // its framebuffer config and creates its context inside sapp_run()
  // (sokol_app.h:14272-14274), long before init_cb is first called from the
  // frame path. A route decided afterwards could not change either.
  std::string error;
  if (!gBackend->selectContextRoute(error)) {
    std::fprintf(stderr, "hermes-node-ui: %s\n", error.c_str());
    std::fflush(stderr);
    // Posted as well as printed, so the program's own `catch` runs rather
    // than the Promise simply never settling. There is no Sokol loop yet to
    // keep alive for delivery -- sapp_run() has not been called -- so what
    // guarantees the JS thread gets to observe it is that UiHost joins that
    // thread after this returns.
    gStartupError = error;
    if (gDispatch == nullptr || !gDispatch->post(onStartupFailed))
      uiTrace("startup failure: post refused (dispatcher closed)");
    return 1;
  }

  // The route is settled and nothing has been created yet: the one point
  // at which the platform can install what Sokol's own setup will then use
  // (macOS: the NSApplication subclass, before Sokol's
  // [NSApplication sharedApplication]).
  uiPlatformBeforeRun();

  WindowRequest request;
  {
    std::lock_guard<std::mutex> lock(gMutex);
    request = gRequest;
  }

  // What an event converted before init_cb reports, until init_cb's
  // refresh replaces the size. A new window is taken to have focus, which
  // is also what the JS object said before Task 11b kept it current.
  gTracked.focused = true;
  gTracked.fullscreen = request.fullscreen;

  sapp_desc desc = {};
  desc.init_cb = onInit;
  desc.frame_cb = onFrame;
  desc.event_cb = onEvent;
  desc.cleanup_cb = onCleanup;
  desc.width = request.width;
  desc.height = request.height;
  // Borrowed, not copied, by sokol_app -- which is why gRequest has
  // static storage and this reads from it rather than from the local copy
  // above.
  desc.window_title = gRequest.title.c_str();
  desc.high_dpi = request.highDpi;
  desc.fullscreen = request.fullscreen;
  desc.logger.func = sokolLog;
  // 4.3 core, matching what GlBackend requires of the context it shares
  // with. Sokol already defaults to 4.3 on Linux; stated explicitly so the
  // two cannot drift apart silently, since a lower context would run until
  // the first texture view and then assert inside Sokol.
  desc.gl.major_version = 4;
  desc.gl.minor_version = 3;
  // Drops are delivered with their paths (`hnui_event::paths`), bounded by
  // the ABI's own limits so that what Sokol keeps and what the ABI
  // promises cannot disagree.
  desc.enable_dragndrop = true;
  desc.max_dropped_files = HNUI_DROP_MAX_FILES;
  desc.max_dropped_file_path_length = HNUI_DROP_MAX_PATH_BYTES;
  // Needed for `ui.writeClipboard`, and for answering other clients that
  // ask for our selection. It also makes Sokol emit CLIPBOARD_PASTED, on
  // X11 right after the KEY_DOWN of a Ctrl+V with no other modifier
  // (`_sapp_x11_key_event`, sokol_app.h:13562-13570); the event's text then
  // comes from readClipboardNow(), which blocks this thread inside event_cb
  // for up to kClipboardReadDefaultTimeoutMs when the owner is slow.
  desc.enable_clipboard = true;
  desc.clipboard_size = HNUI_CLIPBOARD_MAX_BYTES;

  uiTrace("sapp_run: %dx%d", desc.width, desc.height);
  sapp_run(&desc);

  // Not reached: nothing calls sapp_quit() or sapp_request_quit(), every
  // quit request is cancelled in event_cb, and on macOS sapp_run() is
  // [NSApp run], which never returns. That is deliberate rather than a gap:
  // on Linux sapp_run() returning would close the X display the JS thread's
  // GL context was created on, under that thread. The process ends from the
  // JS thread instead -- process.exit(), the fatal path, or
  // uiBeforeRuntimeCleanup() -- with the window still on screen. Traced, so
  // that a run reaching here anyway says so.
  uiTrace("sapp_run returned");
  return 0;
}

} // namespace node_compat
} // namespace hermes
