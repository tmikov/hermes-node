/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "backend.h"

#include "gl_present.h"
#include "sokol_config.h"
#include "ui_trace.h"

// Declarations only -- the implementation lives in sokol_gfx_impl.cpp and
// sokol_gfx_present_impl.cpp. Included here for the ABI checks below, which
// are the reason this file is where RULING 45 asked for them: it is the one
// translation unit that has a legitimate need for both this header and
// hnui_abi.h (reached through backend.h).
#include <sokol_gfx.h>

// GL_GLEXT_PROTOTYPES before the GL headers is what makes the 3.2+/4.x
// entry points this file calls -- glTexStorage2D for immutable surface
// storage, glFenceSync/glClientWaitSync/glDeleteSync for completion --
// DECLARED rather than merely present in libGL. The spike probe
// (test/fixtures/hnui-spike/glx_share.c) reaches them exactly this way,
// and it is the code these paths were measured with. glx.h pulls in gl.h,
// so the define has to come first.
#define GL_GLEXT_PROTOTYPES 1
#include <GL/gl.h>
#include <GL/glx.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

namespace hermes {
namespace node_compat {

//===========================================================================
// RULING 45: hnui_gpu_view really is sg_view
//===========================================================================
//
// `hnui_abi.h` cannot include `sokol_gfx.h`: it must stay plain C and stay
// includable by an addon that has never heard of Sokol. So `hnui_gpu_view`
// is declared there as a standalone `{ uint32_t id; }` -- and when Task 7
// wrote it, that was an *inference* from Sokol's naming convention rather
// than a copy, because no `sg_view` existed anywhere in this tree to check
// against. Sokol was vendored one commit ago; this is where the inference
// stops being trusted.
//
// The failure these guard against is the worst one that header can produce.
// Addons compile against `hnui_abi.h` and convert its handles to Sokol's by
// value; a layout disagreement would not fail to compile and would not fail
// to link -- it would pass a differently-shaped handle to `sg_*` and index
// the wrong resource pool slot. A comment would not survive a Sokol bump.
// These do: a bump that changes `sg_view` fails this build.

static_assert(
    sizeof(hnui_gpu_view) == sizeof(sg_view),
    "hnui_gpu_view must be the same size as Sokol's sg_view: hnui_target "
    "carries Sokol view handles across the C ABI and addons convert between "
    "the two by value. Update hnui_abi.h to match the vendored sokol_gfx.h.");

static_assert(
    alignof(hnui_gpu_view) == alignof(sg_view),
    "hnui_gpu_view and sg_view must have the same alignment; otherwise the "
    "two disagree about where they sit inside any struct that embeds them, "
    "hnui_target included.");

static_assert(
    offsetof(hnui_gpu_view, id) == offsetof(sg_view, id),
    "hnui_gpu_view::id must sit at the same offset as sg_view::id.");

static_assert(
    sizeof(hnui_gpu_view::id) == sizeof(sg_view::id),
    "hnui_gpu_view::id and sg_view::id must be the same width. Equal sizeof "
    "and equal offsetof on a one-member struct would both still hold if one "
    "side's id were narrower and padded.");

// The range assumption behind `hnui_target::color_format` being `int32_t`
// rather than `sg_pixel_format`. The ABI header carries Sokol's pixel
// format as a plain int32_t so it need not include sokol_gfx.h, which is
// only sound if every `sg_pixel_format` value fits in one. Sokol terminates
// the enum with `_SG_PIXELFORMAT_FORCE_U32 = 0x7FFFFFFF` -- that is the
// largest value any conforming compiler can give it, and it is exactly
// INT32_MAX. Nothing said so until now.
static_assert(
    static_cast<long long>(_SG_PIXELFORMAT_FORCE_U32) ==
        static_cast<long long>(INT32_MAX),
    "sg_pixel_format's terminator is no longer INT32_MAX, so the enum's "
    "values may no longer fit in hnui_target::color_format's int32_t. "
    "Widening that field is an ABI break (HNUI_ABI_VERSION).");

namespace {

//===========================================================================
// GLX entry points
//===========================================================================
//
// Declared here rather than taken from <GL/glxext.h>, which only declares
// them when GLX_GLXEXT_PROTOTYPES is defined and which a given platform SDK
// may or may not ship at the version this needs. The spike probe
// (test/fixtures/hnui-spike/glx_share.c) does the same, and it is the code
// these paths were measured with.
using PfnGlxCreateContextAttribsArb =
    GLXContext (*)(Display *, GLXFBConfig, GLXContext, Bool, const int *);

#ifndef GLX_CONTEXT_MAJOR_VERSION_ARB
#define GLX_CONTEXT_MAJOR_VERSION_ARB 0x2091
#endif
#ifndef GLX_CONTEXT_MINOR_VERSION_ARB
#define GLX_CONTEXT_MINOR_VERSION_ARB 0x2092
#endif
#ifndef GLX_CONTEXT_FLAGS_ARB
#define GLX_CONTEXT_FLAGS_ARB 0x2094
#endif
#ifndef GLX_CONTEXT_PROFILE_MASK_ARB
#define GLX_CONTEXT_PROFILE_MASK_ARB 0x9126
#endif
#ifndef GLX_CONTEXT_CORE_PROFILE_BIT_ARB
#define GLX_CONTEXT_CORE_PROFILE_BIT_ARB 0x00000001
#endif
#ifndef GLX_CONTEXT_FORWARD_COMPATIBLE_BIT_ARB
#define GLX_CONTEXT_FORWARD_COMPATIBLE_BIT_ARB 0x00000002
#endif

/// Desktop GL 4.3 core, and no fallback. 4.3 is where Sokol turns on
/// `gl_texture_views` (`sokol_gfx.h:10505`), which the surface protocol
/// needs; a 3.3 context would run until the first texture view and then
/// assert inside Sokol. The spike probe falls back to 3.3 deliberately --
/// it is a probe, and reporting the rest of its answers was worth more than
/// stopping -- but a compositor that did so would be choosing a
/// configuration it knows breaks later.
constexpr int kRequiredGlMajor = 4;
constexpr int kRequiredGlMinor = 3;

//===========================================================================
// A scoped X error handler
//===========================================================================
//
// Xlib's default handler calls exit() on a protocol error, so a driver that
// refuses the context request would end the process instead of rejecting
// createWindow's Promise -- which is the wrong half of the spec's "Failure
// splits in two". Xlib offers only a process-global handler, so this saves
// and restores it around the calls that can provoke one. Single-threaded by
// construction: everything here runs on the platform thread inside
// init_cb.
int gXErrorSeen = 0;
char gXErrorText[256];

int collectXError(Display *display, XErrorEvent *event) {
  gXErrorSeen = 1;
  XGetErrorText(display, event->error_code, gXErrorText, sizeof(gXErrorText));
  return 0;
}

class ScopedXErrorHandler {
 public:
  ScopedXErrorHandler() {
    gXErrorSeen = 0;
    gXErrorText[0] = '\0';
    previous_ = XSetErrorHandler(collectXError);
  }
  ~ScopedXErrorHandler() {
    XSetErrorHandler(previous_);
  }

  ScopedXErrorHandler(const ScopedXErrorHandler &) = delete;
  ScopedXErrorHandler &operator=(const ScopedXErrorHandler &) = delete;

  /// Xlib is asynchronous: a request that fails does so on the server, and
  /// the error arrives whenever the next round trip happens. Without a sync
  /// the handler above has typically not run yet when the caller checks.
  bool sawError(Display *display) const {
    XSync(display, False);
    return gXErrorSeen != 0;
  }

  const char *text() const {
    return gXErrorText[0] ? gXErrorText : "unknown X error";
  }

 private:
  XErrorHandler previous_ = nullptr;
};

//===========================================================================
// The context route (spec: Lifecycle, stage 1)
//===========================================================================

enum class GlRoute {
  /// v1. The worker's shared context goes current with no drawable at all.
  /// Measured working on llvmpipe (2026-09-19) and on AMD radeonsi
  /// (2026-09-21); see docs/notes/.
  kSurfaceless,
  /// A contingency for a driver that refuses the above: a 1x1 pbuffer as
  /// the worker's drawable. Not built in v1, and therefore refused by name
  /// rather than silently treated as surfaceless.
  kPbuffer,
};

constexpr const char *kRouteEnvVar = "HERMES_NODE_UI_GL_ROUTE";

//===========================================================================
// The completion mode (spec: Lifecycle, stage 2)
//===========================================================================

/// How the producer's GPU completion is established.
///
/// **Decided by a startup probe, not by driver name** (spec: Completion
/// per backend). Sync objects were visible across the share group on
/// llvmpipe and on AMD radeonsi, so polling is the expected outcome -- but
/// NVIDIA's proprietary driver is untested and is the most likely of any
/// to differ, and the probe exists so that it need not be.
enum class CompletionMode {
  /// Not yet decided; the probe has not run.
  kUndecided,
  /// The producer creates a fence and the PLATFORM thread polls it with
  /// timeout 0 once per frame callback. The preferred path.
  kPolling,
  /// The fallback: the producer waits on its own fence before publishing,
  /// and the platform thread never touches one.
  kBlocking,
};

constexpr const char *kCompletionEnvVar = "HERMES_NODE_UI_GL_COMPLETION";

/// How long the probe gives an unsignalled fence before concluding that
/// cross-context polling does not work here.
///
/// **Not a test assertion, and the plan's "no timing-based assertions"
/// rule does not reach it**: that rule is about tests, whose verdicts must
/// not depend on machine load. This is a startup measurement of a driver
/// property, it has no verdict to give but its own, and the fallback it
/// selects on timeout is the SAFE answer rather than a failure. The number
/// is an order of magnitude above the only real measurement anyone here
/// has -- AMD radeonsi took 94,603 timeout-0 polls over 14.57 ms
/// (docs/notes/2026-09-21-linux-hardware-spike-results.md) -- because the
/// cost of being wrong in one direction (a needless fallback to a slower
/// but correct mode) is much smaller than in the other.
constexpr int kProbeBudgetMs = 200;

/// How long the worker will wait for the platform thread to answer the
/// probe at all. Distinct from the budget above: this one bounds a
/// handshake, not a fence. Reaching it means the frame callback is not
/// running, which is a situation the fallback also survives.
constexpr int kProbeHandshakeTimeoutMs = 2000;

/// The completion-mode override, parsed alongside the route.
///
/// Parsed in stage 1, although it is not applied until stage 2: stage 1 is
/// the earliest point that can report a bad value through a path the
/// program can observe (it rejects createWindow's Promise), and a
/// misspelled value noticed later would have already let the probe pick a
/// mode -- so the program would run, in a mode the operator did not ask
/// for, with the refusal arriving after the decision it was meant to
/// influence.
bool parseCompletionOverride(CompletionMode *out, std::string &error) {
  const char *requested = ::getenv(kCompletionEnvVar);
  if (requested == nullptr || requested[0] == '\0') {
    *out = CompletionMode::kUndecided;
    return true;
  }
  if (std::strcmp(requested, "poll") == 0) {
    *out = CompletionMode::kPolling;
    return true;
  }
  if (std::strcmp(requested, "block") == 0) {
    *out = CompletionMode::kBlocking;
    return true;
  }
  error = "unknown value for " + std::string(kCompletionEnvVar) + ": '" +
      requested +
      "'. Known modes: poll (the platform thread polls the producer's "
      "fence with timeout 0) and block (the producer waits on its own "
      "fence before publishing). Unset it to let the startup probe "
      "choose.";
  return false;
}

//===========================================================================
// GlBackend
//===========================================================================

class GlBackend final : public CompositorBackend {
 public:
  bool selectContextRoute(std::string &error) override;
  bool createSharedContext(std::string &error) override;
  bool makeCurrentOnWorker(std::string &error) override;

  bool createGenerationSurfaces(
      GenerationId gen,
      int width,
      int height,
      std::string &error) override;
  void destroyGenerationSurfaces(GenerationId gen) override;
  bool frameTarget(SurfaceRef ref, hnui_target *out) override;
  void submitAndTrackCompletion(
      SurfaceRef ref,
      void (*onComplete)(void *user, SurfaceRef ref, bool ok),
      void *user) override;

  void setConsumerCompletion(
      void (*onComplete)(void *user, SurfaceRef ref, bool ok),
      void *user) override;

  bool wrapForPresenting(GenerationId gen, std::string &error) override;
  bool acquireFrameSwapchain() override;
  void presentClear() override;
  void blit(SurfaceRef ref) override;
  void releaseWrappers(GenerationId gen) override;
  void presentInvalid() override;
  void pollCompletions() override;
  bool readPresentedPixels(int width, int height, uint8_t *out) override;

 private:
  /// One generation's surfaces, as both halves of this backend see them.
  ///
  /// The GL texture names are the crossing point: the worker creates them
  /// and the presenting instance wraps those same names (see
  /// GlPresenter::wrap). Everything else here belongs to the producer.
  struct GenerationSurfaces {
    uint32_t textures[kSlotsPerGeneration] = {};
    sg_image images[kSlotsPerGeneration] = {};
    sg_view colorViews[kSlotsPerGeneration] = {};
    sg_view resolveViews[kSlotsPerGeneration] = {};
    int width = 0;
    int height = 0;
  };

  /// One outstanding fence, producer or consumer.
  struct PendingFence {
    void *fence = nullptr;
    SurfaceRef ref{};
    void (*onComplete)(void *user, SurfaceRef ref, bool ok) = nullptr;
    void *user = nullptr;
  };

  /// Poll \p pending in place, invoking and erasing whatever completed.
  /// Platform thread only.
  void pollFences(std::vector<PendingFence> &pending, const char *what);

  /// The stage-2 probe's platform-thread half, run from pollCompletions().
  void advanceCompletionProbe();

  /// `sg_setup()` for the PRODUCING instance, plus the stage-2 completion
  /// probe. Worker thread, immediately after its context goes current --
  /// which is the earliest moment either is possible and the latest that
  /// is still before the window's Promise resolves.
  bool setupProducerInstance(std::string &error);

  GlRoute route_ = GlRoute::kSurfaceless;
  Display *display_ = nullptr;
  GLXContext platformContext_ = nullptr;
  GLXContext workerContext_ = nullptr;

  GlPresenter presenter_;

  /// Written by the worker (createGenerationSurfaces /
  /// destroyGenerationSurfaces), read by the platform thread
  /// (wrapForPresenting). A generation is inserted strictly before
  /// anything of it can be published, and erased strictly after the
  /// presenting side has released its wrappers -- but "strictly" there is
  /// an ordering argument about the protocol, not about memory, and two
  /// threads reaching a std::map need an actual lock either way.
  mutable std::mutex surfacesMutex_;
  std::map<uint32_t, GenerationSurfaces> surfaces_;

  /// The environment override, parsed in selectContextRoute() so that a
  /// bad value is refused before a window exists. kUndecided means "no
  /// override; run the probe".
  CompletionMode requestedMode_ = CompletionMode::kUndecided;
  /// Written once by the platform thread when the probe resolves, read by
  /// the worker on every submission.
  std::atomic<CompletionMode> mode_{CompletionMode::kUndecided};

  std::mutex probeMutex_;
  std::condition_variable probeCv_;
  void *probeFence_ = nullptr;
  bool probeRunning_ = false;
  bool probeDone_ = false;
  long probePolls_ = 0;
  std::chrono::steady_clock::time_point probeDeadline_{};

  std::mutex pendingMutex_;
  std::vector<PendingFence> pendingProducer_;
  std::vector<PendingFence> pendingConsumer_;

  /// One trace per completion path per run, not one per frame.
  ///
  /// **Why they exist.** `mode_` is the SELECTION, and the "completion
  /// mode:" line printed from it says only which arm was chosen -- so
  /// deleting an arm of submitAndTrackCompletion() outright left every
  /// assertion in test/ui-present.js green (measured; that file's own
  /// comment records the previous layer of the same hole, where the line
  /// printed the REQUEST rather than the state). These are traced from
  /// inside the arms, so each is evidence that the path ran.
  ///
  /// Once per run rather than per frame because the lines are test surface:
  /// a per-frame line would put hundreds of them ahead of the capture lines
  /// that file also matches in order, in a quantity that depends on the
  /// driver.
  std::atomic<bool> tracedBlockingSubmission_{false};
  std::atomic<bool> tracedPollingSubmission_{false};

  void (*consumerComplete_)(void *user, SurfaceRef ref, bool ok) = nullptr;
  void *consumerUser_ = nullptr;
};

/// The GL internal format for a surface in \p format.
///
/// Refused by name rather than defaulted: a surface allocated in a format
/// the swapchain does not use produces a picture that is wrong in a way
/// (swapped channels, wrong gamma) that looks like a shader bug for a long
/// time before anyone suspects the allocation.
bool glInternalFormatFor(int format, GLenum *out) {
  switch (static_cast<sg_pixel_format>(format)) {
    case SG_PIXELFORMAT_RGBA8:
      *out = GL_RGBA8;
      return true;
    case SG_PIXELFORMAT_SRGB8A8:
      *out = GL_SRGB8_ALPHA8;
      return true;
    default:
      return false;
  }
}

bool GlBackend::selectContextRoute(std::string &error) {
  // The completion-mode override is parsed here as well, although it is
  // not applied until stage 2 -- see parseCompletionOverride() for why the
  // two are separated.
  if (!parseCompletionOverride(&requestedMode_, error))
    return false;

  const char *requested = ::getenv(kRouteEnvVar);

  if (requested == nullptr || requested[0] == '\0') {
    route_ = GlRoute::kSurfaceless;
    uiTrace("route: surfaceless (default)");
    return true;
  }

  if (std::strcmp(requested, "surfaceless") == 0) {
    route_ = GlRoute::kSurfaceless;
    uiTrace("route: surfaceless (%s)", kRouteEnvVar);
    return true;
  }

  // Refused, not degraded. The spec is explicit that "a configuration for
  // which the built route fails is refused with a message, not silently
  // degraded" -- and an operator who set this variable did so because
  // something was already wrong, which is exactly when being quietly given
  // the default is worst.
  if (std::strcmp(requested, "pbuffer") == 0) {
    error =
        "the pbuffer GL context route is not built in v1. It is the "
        "contingency for a driver that refuses a surfaceless make-current, "
        "and neither driver measured so far needs it (llvmpipe and AMD "
        "radeonsi; see docs/notes/2026-09-21-linux-hardware-spike-results."
        "md). Unset " +
        std::string(kRouteEnvVar) + " to use the surfaceless route.";
    return false;
  }

  error = "unknown value for " + std::string(kRouteEnvVar) + ": '" + requested +
      "'. Known routes: surfaceless (built), pbuffer (not built "
      "in v1).";
  return false;
}

bool GlBackend::createSharedContext(std::string &error) {
  // Everything below uses only what init_cb has: the current display and
  // the current context. That constraint is why the spike probe was written
  // the way it was (question A), and it is what keeps this independent of
  // sokol_app's private _sapp state.
  display_ = glXGetCurrentDisplay();
  platformContext_ = glXGetCurrentContext();
  if (display_ == nullptr || platformContext_ == nullptr) {
    error =
        "sokol_app's GL context is not current on the platform thread. "
        "createSharedContext() must be called from init_cb, after "
        "_sapp_glx_create_context() has made it current.";
    return false;
  }

  int fbConfigId = 0;
  if (glXQueryContext(
          display_, platformContext_, GLX_FBCONFIG_ID, &fbConfigId) !=
      Success) {
    error = "glXQueryContext(GLX_FBCONFIG_ID) failed on sokol_app's context.";
    return false;
  }

  // The worker's context must be created against the SAME framebuffer
  // config as sokol_app's, and GLX hands back an id rather than a handle.
  // Resolving the id to a handle is question A-1 of the hardware spike, and
  // is the reason the probe checked it at all.
  const int idAttributes[] = {GLX_FBCONFIG_ID, fbConfigId, None};
  int matchCount = 0;
  GLXFBConfig *configs = glXChooseFBConfig(
      display_, DefaultScreen(display_), idAttributes, &matchCount);
  if (configs == nullptr || matchCount == 0) {
    if (configs != nullptr)
      XFree(configs);
    error =
        "the framebuffer config sokol_app's context was created from does "
        "not resolve back to a handle.";
    return false;
  }
  GLXFBConfig config = configs[0];
  XFree(configs);

  auto createContextAttribs =
      reinterpret_cast<PfnGlxCreateContextAttribsArb>(glXGetProcAddressARB(
          reinterpret_cast<const GLubyte *>("glXCreateContextAttribsARB")));
  if (createContextAttribs == nullptr) {
    error =
        "glXCreateContextAttribsARB is unavailable, so no core-profile "
        "context can be created. sokol_app requires the same extension and "
        "would have panicked already, so reaching this means the GLX "
        "function pointer went missing between its call and ours.";
    return false;
  }

  const int contextAttributes[] = {
      GLX_CONTEXT_MAJOR_VERSION_ARB,
      kRequiredGlMajor,
      GLX_CONTEXT_MINOR_VERSION_ARB,
      kRequiredGlMinor,
      GLX_CONTEXT_PROFILE_MASK_ARB,
      GLX_CONTEXT_CORE_PROFILE_BIT_ARB,
      GLX_CONTEXT_FLAGS_ARB,
      GLX_CONTEXT_FORWARD_COMPATIBLE_BIT_ARB,
      0,
      0};

  {
    ScopedXErrorHandler xErrors;
    // The third argument is the share context: this is the whole point of
    // the call. Direct rendering (True) is requested rather than required
    // -- GLX falls back to indirect on its own, and the spike measured a
    // direct context on both drivers tested.
    workerContext_ = createContextAttribs(
        display_, config, platformContext_, True, contextAttributes);
    if (xErrors.sawError(display_) || workerContext_ == nullptr) {
      if (workerContext_ != nullptr) {
        glXDestroyContext(display_, workerContext_);
        workerContext_ = nullptr;
      }
      char message[512];
      std::snprintf(
          message,
          sizeof(message),
          "could not create a GL %d.%d core context sharing with "
          "sokol_app's (%s). The compositor requires %d.%d core: below it, "
          "Sokol's texture-view path is unavailable and the surface "
          "protocol cannot work.",
          kRequiredGlMajor,
          kRequiredGlMinor,
          xErrors.text(),
          kRequiredGlMajor,
          kRequiredGlMinor);
      error = message;
      return false;
    }
  }

  uiTrace(
      "shared context created (%s), GL %d.%d core",
      glXIsDirect(display_, workerContext_) ? "direct" : "indirect",
      kRequiredGlMajor,
      kRequiredGlMinor);

  // The presenting instance, set up here because this is init_cb and
  // sokol_app's own context is current on this thread -- the one moment
  // that is true before the frame loop begins (spec: Architecture,
  // "init_cb: ... sgp_setup() the presenting copy"). It also yields the
  // swapchain's colour format, which the worker needs for its own
  // sg_setup() and cannot ask sokol_app for.
  if (!presenter_.setup(error))
    return false;

  return true;
}

bool GlBackend::makeCurrentOnWorker(std::string &error) {
  if (workerContext_ == nullptr) {
    error =
        "makeCurrentOnWorker() called before createSharedContext() "
        "succeeded.";
    return false;
  }

  // Stage 1 already settled this; the switch exists so that adding the
  // pbuffer route later is a case here rather than a rewrite. kPbuffer
  // cannot be reached -- selectContextRoute() refuses it -- but is handled
  // rather than defaulted, so that building it forgets nothing.
  switch (route_) {
    case GlRoute::kSurfaceless: {
      ScopedXErrorHandler xErrors;
      const Bool ok =
          glXMakeContextCurrent(display_, None, None, workerContext_);
      if (!ok || xErrors.sawError(display_)) {
        error = std::string(
                    "the shared GL context could not be made current on the "
                    "worker thread with no drawable (") +
            xErrors.text() +
            "). This driver needs the pbuffer route, which is not built in "
            "v1 -- see docs/superpowers/specs/"
            "2026-09-18-ui-compositor-design.md, \"GLX sharing\".";
        return false;
      }
      uiTrace("worker context current, surfaceless");
      return setupProducerInstance(error);
    }
    case GlRoute::kPbuffer:
      error =
          "the pbuffer route is not built in v1; selectContextRoute() "
          "should have refused it before sapp_run().";
      return false;
  }

  error = "unreachable GL context route.";
  return false;
}

/// Sokol's logger for the PRODUCING instance. Its twin lives in
/// gl_present.cpp; the two are separate so that a message says which
/// instance produced it.
void producerLog(
    const char *tag,
    uint32_t logLevel,
    uint32_t logItemId,
    const char *message,
    uint32_t lineNr,
    const char *filename,
    void *userData) {
  (void)tag;
  (void)userData;
  static const char *const kLevels[] = {"panic", "error", "warning", "info"};
  const char *level = logLevel < 4 ? kLevels[logLevel] : "?";
  std::fprintf(
      stderr,
      "[sokol-produce:%s] item %u%s%s (%s:%u)\n",
      level,
      logItemId,
      message ? ": " : "",
      message ? message : "",
      filename ? filename : "sokol_gfx.h",
      lineNr);
  std::fflush(stderr);
}

bool GlBackend::setupProducerInstance(std::string &error) {
  sg_desc desc = {};
  // Spec, The native C API: "sg_setup uses environment defaults of the
  // surface color format, depth_format = NONE, sample_count = 1, so
  // default pipelines in sokol_imgui and sokol_gl match with no
  // configuration." All three are stated here rather than defaulted,
  // because an addon's pipeline inherits them and a mismatch surfaces as a
  // format error inside the addon.
  desc.environment.defaults.color_format =
      static_cast<sg_pixel_format>(presenter_.colorFormat());
  desc.environment.defaults.depth_format = SG_PIXELFORMAT_NONE;
  desc.environment.defaults.sample_count = 1;
  desc.logger.func = producerLog;
  sg_setup(&desc);
  if (!sg_isvalid()) {
    error =
        "the producing sokol_gfx instance could not be set up on the "
        "worker's shared GL context.";
    return false;
  }
  uiTrace(
      "producing instance ready: color_format %d", presenter_.colorFormat());

  //=========================================================================
  // Stage 2: the completion probe
  //=========================================================================

  if (requestedMode_ != CompletionMode::kUndecided) {
    mode_.store(requestedMode_);
    // **Read back from mode_, not from requestedMode_.** These are the
    // value asked for and the value in force, and printing the first
    // while the second is what submissions actually consult means the
    // trace reassures about a setting it never checked. Found by
    // mutation: changing this store to always select polling left the
    // message saying "blocking", and test/ui-present.js -- whose only
    // evidence about the completion mode is this line -- still passed.
    // A test that cannot see the thing it names is not a test, so the
    // line now reports the state rather than the request.
    const CompletionMode applied = mode_.load();
    uiTrace(
        "completion mode: %s (%s)",
        applied == CompletionMode::kPolling ? "polling" : "blocking",
        kCompletionEnvVar);
    return true;
  }

  // "the module creates a fence on the producer context following a
  // trivial GPU operation and polls it with timeout 0 from the main
  // thread" (spec: Completion per backend). The trivial operation is a
  // clear of a small offscreen target -- raw GL rather than sokol, because
  // it must not disturb the resource pools an addon is about to use, and
  // because there is no drawable on this context to clear otherwise.
  GLuint probeTexture = 0;
  GLuint probeFramebuffer = 0;
  GLenum internalFormat = GL_RGBA8;
  glGenTextures(1, &probeTexture);
  glBindTexture(GL_TEXTURE_2D, probeTexture);
  glTexStorage2D(GL_TEXTURE_2D, 1, internalFormat, 16, 16);
  glBindTexture(GL_TEXTURE_2D, 0);
  glGenFramebuffers(1, &probeFramebuffer);
  glBindFramebuffer(GL_FRAMEBUFFER, probeFramebuffer);
  glFramebufferTexture2D(
      GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, probeTexture, 0);
  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);

  GLsync fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
  // The flush is not optional: an unflushed fence need never signal, and
  // the platform thread polls with timeout 0 and no flush bit, so it
  // cannot flush it either. This is the same rule the real submissions
  // below follow.
  glFlush();

  bool decided = false;
  if (fence != nullptr) {
    {
      std::lock_guard<std::mutex> lock(probeMutex_);
      probeFence_ = static_cast<void *>(fence);
      probeRunning_ = true;
      probeDone_ = false;
      probePolls_ = 0;
      probeDeadline_ = std::chrono::steady_clock::now() +
          std::chrono::milliseconds(kProbeBudgetMs);
    }
    std::unique_lock<std::mutex> lock(probeMutex_);
    decided = probeCv_.wait_for(
        lock, std::chrono::milliseconds(kProbeHandshakeTimeoutMs), [this] {
          return probeDone_;
        });
    if (!decided) {
      // The frame callback never answered. Stop the platform thread from
      // touching the fence, then delete it here -- under the same lock, so
      // the two cannot both be holding it.
      probeRunning_ = false;
      if (probeFence_ != nullptr) {
        glDeleteSync(static_cast<GLsync>(probeFence_));
        probeFence_ = nullptr;
      }
    }
  }

  if (!decided) {
    mode_.store(CompletionMode::kBlocking);
    uiTrace(
        "completion mode: blocking (the probe was never answered -- no "
        "frame callback ran within %d ms)",
        kProbeHandshakeTimeoutMs);
  }

  glDeleteFramebuffers(1, &probeFramebuffer);
  glDeleteTextures(1, &probeTexture);
  while (glGetError() != GL_NO_ERROR) {
  }
  // Raw GL happened on this context; hand sokol back a cache that matches.
  // See the Linux spike's section 4 for what skipping this costs.
  sg_reset_state_cache();
  return true;
}

void GlBackend::advanceCompletionProbe() {
  // The whole body holds probeMutex_, including the GL call. That is
  // deliberate: the worker deletes this fence itself if the handshake
  // times out, and it does so under this same lock, so holding it here is
  // what stops this thread from polling a sync object that has just been
  // deleted. The call costs a timeout-0 poll, and the worker it blocks is
  // parked in wait_for() -- which releases the lock -- so nothing is
  // serialised that was not already waiting.
  std::lock_guard<std::mutex> lock(probeMutex_);
  if (!probeRunning_ || probeFence_ == nullptr)
    return;

  const GLenum result =
      glClientWaitSync(static_cast<GLsync>(probeFence_), 0, 0);
  ++probePolls_;

  CompletionMode chosen = CompletionMode::kBlocking;
  const char *why = nullptr;
  if (result == GL_ALREADY_SIGNALED || result == GL_CONDITION_SATISFIED) {
    chosen = CompletionMode::kPolling;
    why = "the producer's fence signalled to this thread";
  } else if (result == GL_WAIT_FAILED) {
    // Terminal during normal operation; during the probe it merely selects
    // the fallback (spec: "GL_WAIT_FAILED *during the probe* selects the
    // fallback; during normal operation it stays terminal").
    why = "GL_WAIT_FAILED while polling the producer's fence";
  } else if (std::chrono::steady_clock::now() > probeDeadline_) {
    why = "the producer's fence did not signal within the probe budget";
  } else {
    // GL_TIMEOUT_EXPIRED inside the budget: not an answer yet. radeonsi
    // took 94,603 polls over 14.57 ms to get here, so this is the ordinary
    // case for several frames.
    return;
  }

  glDeleteSync(static_cast<GLsync>(probeFence_));
  probeFence_ = nullptr;
  probeRunning_ = false;
  probeDone_ = true;
  mode_.store(chosen);
  uiTrace(
      "completion mode: %s (probe: %s after %ld poll(s))",
      chosen == CompletionMode::kPolling ? "polling" : "blocking",
      why,
      probePolls_);
  probeCv_.notify_all();
}

bool GlBackend::createGenerationSurfaces(
    GenerationId gen,
    int width,
    int height,
    std::string &error) {
  GLenum internalFormat = 0;
  if (!glInternalFormatFor(presenter_.colorFormat(), &internalFormat)) {
    error =
        "the swapchain's colour format has no GL internal format this "
        "backend knows how to allocate a surface in.";
    return false;
  }

  GenerationSurfaces surfaces;
  surfaces.width = width;
  surfaces.height = height;

  // **Immutable storage, and this is the constraint the Linux spike
  // found** (docs/notes/2026-09-19-linux-spike-results.md, section 4).
  // Sokol turns on gl_texture_views for GL 4.3 and above
  // (sokol_gfx.h:10505) and its view path then calls glTextureView(),
  // which requires an immutable source; a glTexImage2D texture trips an
  // assert inside Sokol rather than failing here.
  glGenTextures(kSlotsPerGeneration, surfaces.textures);
  for (int i = 0; i < kSlotsPerGeneration; ++i) {
    glBindTexture(GL_TEXTURE_2D, surfaces.textures[i]);
    glTexStorage2D(GL_TEXTURE_2D, 1, internalFormat, width, height);
  }
  glBindTexture(GL_TEXTURE_2D, 0);

  if (glGetError() != GL_NO_ERROR) {
    glDeleteTextures(kSlotsPerGeneration, surfaces.textures);
    while (glGetError() != GL_NO_ERROR) {
    }
    sg_reset_state_cache();
    error = "the surface textures for a generation could not be allocated.";
    return false;
  }
  // Raw GL happened; see the spike note above setupProducerInstance().
  sg_reset_state_cache();

  for (int i = 0; i < kSlotsPerGeneration; ++i) {
    sg_image_desc imageDesc = {};
    imageDesc.type = SG_IMAGETYPE_2D;
    imageDesc.width = width;
    imageDesc.height = height;
    imageDesc.num_mipmaps = 1;
    imageDesc.pixel_format =
        static_cast<sg_pixel_format>(presenter_.colorFormat());
    imageDesc.sample_count = 1;
    // BOTH usages, per spec's Surfaces: "created with both
    // color_attachment and resolve_attachment usage ... A renderer doing
    // MSAA draws into its own multisampled image and resolves into the
    // surface." Without resolve_attachment the resolve view below is
    // refused, and an MSAA addon has nowhere to resolve to.
    imageDesc.usage.color_attachment = true;
    imageDesc.usage.resolve_attachment = true;
    imageDesc.gl_textures[0] = surfaces.textures[i];
    imageDesc.gl_texture_target = GL_TEXTURE_2D;
    imageDesc.label = "hnui-surface";
    surfaces.images[i] = sg_make_image(&imageDesc);
    if (sg_query_image_state(surfaces.images[i]) != SG_RESOURCESTATE_VALID) {
      error = "a surface image could not be created for a generation.";
      return false;
    }

    sg_view_desc colorDesc = {};
    colorDesc.color_attachment.image = surfaces.images[i];
    colorDesc.label = "hnui-surface-color";
    surfaces.colorViews[i] = sg_make_view(&colorDesc);
    if (sg_query_view_state(surfaces.colorViews[i]) != SG_RESOURCESTATE_VALID) {
      error = "a surface colour-attachment view could not be created.";
      return false;
    }

    sg_view_desc resolveDesc = {};
    resolveDesc.resolve_attachment.image = surfaces.images[i];
    resolveDesc.label = "hnui-surface-resolve";
    surfaces.resolveViews[i] = sg_make_view(&resolveDesc);
    if (sg_query_view_state(surfaces.resolveViews[i]) !=
        SG_RESOURCESTATE_VALID) {
      error = "a surface resolve-attachment view could not be created.";
      return false;
    }
  }

  {
    std::lock_guard<std::mutex> lock(surfacesMutex_);
    surfaces_[gen.value] = surfaces;
  }
  uiTrace(
      "generation %u: %d surfaces at %dx%d",
      gen.value,
      kSlotsPerGeneration,
      width,
      height);
  return true;
}

void GlBackend::destroyGenerationSurfaces(GenerationId gen) {
  GenerationSurfaces surfaces;
  {
    std::lock_guard<std::mutex> lock(surfacesMutex_);
    auto it = surfaces_.find(gen.value);
    if (it == surfaces_.end())
      return;
    surfaces = it->second;
    surfaces_.erase(it);
  }

  // Worker thread only, which is what OwnerDispatch is for: the last
  // reference to a generation is dropped by whichever thread ran the
  // completion that dropped it, and SurfaceTable's destroy callback posts
  // here rather than freeing inline.
  for (int i = 0; i < kSlotsPerGeneration; ++i) {
    sg_destroy_view(surfaces.colorViews[i]);
    sg_destroy_view(surfaces.resolveViews[i]);
    sg_destroy_image(surfaces.images[i]);
  }
  glDeleteTextures(kSlotsPerGeneration, surfaces.textures);
  while (glGetError() != GL_NO_ERROR) {
  }
  sg_reset_state_cache();
  uiTrace("generation %u: surfaces destroyed", gen.value);
}

bool GlBackend::frameTarget(SurfaceRef ref, hnui_target *out) {
  std::lock_guard<std::mutex> lock(surfacesMutex_);
  auto it = surfaces_.find(ref.gen.value);
  // A stale SurfaceRef from a destroyed generation is refused here rather
  // than answered with a dangling handle -- see backend.h.
  if (it == surfaces_.end() || ref.slot >= kSlotsPerGeneration)
    return false;

  out->color_view.id = it->second.colorViews[ref.slot].id;
  out->resolve_view.id = it->second.resolveViews[ref.slot].id;
  out->width = it->second.width;
  out->height = it->second.height;
  out->color_format = presenter_.colorFormat();
  out->sample_count = 1;
  out->generation = ref.gen.value;
  return true;
}

void GlBackend::submitAndTrackCompletion(
    SurfaceRef ref,
    void (*onComplete)(void *user, SurfaceRef ref, bool ok),
    void *user) {
  // The module commits, never the addon (spec: The native C API --
  // "Addons never call sg_setup, sg_shutdown or sg_commit").
  sg_commit();

  GLsync fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
  // **glFlush, because GL's sg_commit does not.** Without it the fence can
  // sit unflushed in the command stream, and the platform thread polls
  // with timeout 0 and no GL_SYNC_FLUSH_COMMANDS_BIT -- so it cannot flush
  // it either, and the frame would never complete.
  glFlush();
  if (fence == nullptr) {
    onComplete(user, ref, false);
    return;
  }

  CompletionMode mode = mode_.load();
  // kUndecided cannot normally be reached (setupProducerInstance always
  // settles it), but if it ever were, blocking is the answer that is
  // merely slower rather than the one that never completes.
  if (mode == CompletionMode::kUndecided)
    mode = CompletionMode::kBlocking;

  if (mode == CompletionMode::kBlocking) {
    // The producer waits on its own fence and the platform thread never
    // touches one. The first wait carries the flush bit as well, which is
    // belt and braces over the explicit glFlush above.
    //
    // The loop is unbounded on purpose: "A frame the GPU never completes
    // is never published; the last surface stays on screen and ticks are
    // withheld. No timeout in v1" (spec: Errors). Waiting in chunks rather
    // than with GL_TIMEOUT_IGNORED keeps the call interruptible-looking in
    // a debugger, and costs nothing.
    constexpr GLuint64 kWaitChunkNs = 1000ull * 1000ull * 1000ull;
    GLenum result =
        glClientWaitSync(fence, GL_SYNC_FLUSH_COMMANDS_BIT, kWaitChunkNs);
    while (result == GL_TIMEOUT_EXPIRED)
      result = glClientWaitSync(fence, 0, kWaitChunkNs);
    glDeleteSync(fence);
    const bool ok =
        result == GL_ALREADY_SIGNALED || result == GL_CONDITION_SATISFIED;
    // AFTER the wait, deliberately: traced before it, this line would prove
    // only that the arm was entered, where here it also proves the wait
    // returned and with what.
    if (!tracedBlockingSubmission_.exchange(true)) {
      uiTrace(
          "completion: blocking submission waited on the producer thread "
          "(ok=%s)",
          ok ? "true" : "false");
    }
    onComplete(user, ref, ok);
    return;
  }

  // Polling: ownership of the fence transfers to the platform thread,
  // which polls and deletes it. The producer never touches it again.
  //
  // Traced at the hand-off, and what covers the other half of the path is
  // what follows it: in this mode the ONLY thing that can finish a frame is
  // the platform thread's pollFences(), so the "capture: written" line
  // test/ui-present.js matches next cannot appear unless the hand-off was
  // picked up there.
  if (!tracedPollingSubmission_.exchange(true)) {
    uiTrace(
        "completion: polling submission handed the fence to the platform "
        "thread");
  }

  std::lock_guard<std::mutex> lock(pendingMutex_);
  pendingProducer_.push_back(
      PendingFence{static_cast<void *>(fence), ref, onComplete, user});
}

void GlBackend::setConsumerCompletion(
    void (*onComplete)(void *user, SurfaceRef ref, bool ok),
    void *user) {
  consumerComplete_ = onComplete;
  consumerUser_ = user;
}

void GlBackend::pollFences(
    std::vector<PendingFence> &pending,
    const char *what) {
  // Extract under the lock, invoke outside it -- the discipline
  // SurfaceTable::LockedScope follows for the same reason: a completion
  // handler runs unbounded work (it publishes a frame, which takes the
  // table's lock and can destroy a generation) and must not do it while
  // this mutex is held.
  std::vector<std::pair<PendingFence, bool>> completed;
  {
    std::lock_guard<std::mutex> lock(pendingMutex_);
    for (size_t i = 0; i < pending.size();) {
      GLsync fence = static_cast<GLsync>(pending[i].fence);
      const GLenum result = glClientWaitSync(fence, 0, 0);
      if (result == GL_TIMEOUT_EXPIRED) {
        ++i;
        continue;
      }
      const bool ok =
          result == GL_ALREADY_SIGNALED || result == GL_CONDITION_SATISFIED;
      if (!ok) {
        uiTrace(
            "%s completion: GL_WAIT_FAILED for generation %u slot %u",
            what,
            pending[i].ref.gen.value,
            static_cast<unsigned>(pending[i].ref.slot));
      }
      glDeleteSync(fence);
      completed.emplace_back(pending[i], ok);
      pending.erase(pending.begin() + static_cast<long>(i));
    }
  }

  for (auto &entry : completed) {
    if (entry.first.onComplete != nullptr)
      entry.first.onComplete(entry.first.user, entry.first.ref, entry.second);
  }
}

bool GlBackend::wrapForPresenting(GenerationId gen, std::string &error) {
  uint32_t textures[kSlotsPerGeneration] = {};
  int width = 0;
  int height = 0;
  {
    std::lock_guard<std::mutex> lock(surfacesMutex_);
    auto it = surfaces_.find(gen.value);
    if (it == surfaces_.end()) {
      error = "no surfaces exist for that generation.";
      return false;
    }
    for (int i = 0; i < kSlotsPerGeneration; ++i)
      textures[i] = it->second.textures[i];
    width = it->second.width;
    height = it->second.height;
  }
  return presenter_.wrap(
      gen, textures, kSlotsPerGeneration, width, height, error);
}

bool GlBackend::acquireFrameSwapchain() {
  return presenter_.acquireSwapchain();
}

void GlBackend::presentClear() {
  presenter_.clear();
}

void GlBackend::blit(SurfaceRef ref) {
  void *fence = presenter_.blitAndFence(ref);
  if (fence == nullptr) {
    // Nothing was submitted, so no completion will ever arrive -- and the
    // caller took a read reference before calling. Releasing it here is
    // the difference between a frame that was not drawn and a slot that
    // never returns to FREE. `ok` is true because nothing is sampling the
    // surface: this is "no blit", not "a blit whose completion could not
    // be established".
    if (consumerComplete_ != nullptr)
      consumerComplete_(consumerUser_, ref, true);
    return;
  }
  std::lock_guard<std::mutex> lock(pendingMutex_);
  pendingConsumer_.push_back(
      PendingFence{fence, ref, consumerComplete_, consumerUser_});
}

void GlBackend::releaseWrappers(GenerationId gen) {
  presenter_.releaseWrappers(gen);
}

bool GlBackend::readPresentedPixels(int width, int height, uint8_t *out) {
  return presenter_.readPixels(width, height, out);
}

void GlBackend::presentInvalid() {
  // Genuinely nothing to do, and this is NOT a placeholder. GLX has no
  // notion of an unusable swapchain: the case exists because macOS reports
  // one for an obscured window (`sokol_app.h:14872`). backend.h keeps the
  // method so that the shared frame callback handles both backends through
  // one path, and an empty body here is that path's GL half stated in code
  // rather than left to a comment.
}

void GlBackend::pollCompletions() {
  // The probe first, and unconditionally: it is answered from here, and
  // the worker is blocked waiting for that answer before the window's
  // Promise can resolve. A frame callback that skipped this while "not
  // ready yet" would deadlock the startup it is part of.
  advanceCompletionProbe();

  // Producer fences the worker transferred here, then the consumer fences
  // this thread created itself. Producer first, so that a surface whose
  // drawing completed this frame can be taken and presented by the same
  // frame callback rather than waiting for the next one.
  pollFences(pendingProducer_, "producer");
  pollFences(pendingConsumer_, "consumer");
}

} // namespace

CompositorBackend::~CompositorBackend() = default;

CompositorBackend *createGlBackend() {
  return new GlBackend();
}

} // namespace node_compat
} // namespace hermes
