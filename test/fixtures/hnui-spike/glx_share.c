/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/// GLX hardware probe for the UI compositor design.
///
/// Answers the two questions software rendering cannot settle, plus the
/// three it can, so one run on real hardware produces the whole picture.
/// See docs/notes/2026-09-21-linux-hardware-spike-instructions.md for what
/// to do with the answers.
///
///   A. Can we create a context that SHARES with sokol_app's, using only
///      what sokol_app exposes at init_cb time: the display, and the
///      context that is current then?
///   B. Can that shared context be made current on ANOTHER thread with no
///      drawable at all, or does it need a 1x1 pbuffer?
///   C. Does the framebuffer config sokol picks (it asks for
///      GLX_WINDOW_BIT and never GLX_PBUFFER_BIT) support a pbuffer on this
///      driver? llvmpipe's does, which settles nothing.
///   D. Are GL sync objects visible across the share group -- can the main
///      thread wait on a fence the worker created? The design's preferred
///      completion path depends on this; the fallback is blocking the
///      producer instead.
///   E. Are textures visible across the share group, and does their content
///      survive the crossing?
///
/// Question D is measured twice: once with a blocking wait, and once by
/// polling with timeout 0, which is what the compositor actually does once
/// per frame callback. The second is the one that matters, because a fence
/// that is only observable to a blocking wait would be useless there.
///
/// Build and run (a REAL X session, not Xvfb -- the point is the driver):
///   clang -O1 -g -o glx_share glx_share.c -lX11 -lGL -lpthread
///   ./glx_share
///
/// Exit status is 0 unless setup failed; read the PASS/FAIL lines.

#define _GNU_SOURCE
#define GL_GLEXT_PROTOTYPES 1
#include <GL/glx.h>
#include <GL/gl.h>
#include <X11/Xlib.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef GLXContext (*PFNGLXCREATECONTEXTATTRIBSARBPROC_)(
    Display *, GLXFBConfig, GLXContext, Bool, const int *);
typedef void *GLsync_;
typedef GLsync_ (*PFNGLFENCESYNCPROC_)(GLenum, GLbitfield);
typedef GLenum (*PFNGLCLIENTWAITSYNCPROC_)(GLsync_, GLbitfield, GLuint64);
typedef void (*PFNGLDELETESYNCPROC_)(GLsync_);

#ifndef GL_SYNC_GPU_COMMANDS_COMPLETE
#define GL_SYNC_GPU_COMMANDS_COMPLETE 0x9117
#endif
#ifndef GL_SYNC_FLUSH_COMMANDS_BIT
#define GL_SYNC_FLUSH_COMMANDS_BIT 0x00000001
#endif
#ifndef GL_ALREADY_SIGNALED
#define GL_ALREADY_SIGNALED 0x911A
#endif
#ifndef GL_TIMEOUT_EXPIRED
#define GL_TIMEOUT_EXPIRED 0x911B
#endif
#ifndef GL_CONDITION_SATISFIED
#define GL_CONDITION_SATISFIED 0x911C
#endif
#ifndef GL_WAIT_FAILED
#define GL_WAIT_FAILED 0x911D
#endif

static Display *dpy;
static GLXFBConfig sokol_cfg;
static GLXContext sokol_ctx;
static GLXContext worker_ctx;
static GLXPbuffer worker_pbuf;
static GLXWindow glxwin;
static PFNGLXCREATECONTEXTATTRIBSARBPROC_ pCreateContextAttribs;
static PFNGLFENCESYNCPROC_ pFenceSync;
static PFNGLCLIENTWAITSYNCPROC_ pClientWaitSync;
static PFNGLDELETESYNCPROC_ pDeleteSync;

/* Results, printed as a block at the end so one paste carries everything. */
static const char *r_vendor = "?", *r_renderer = "?", *r_version = "?";
static int r_cfg_has_pbuffer = -1;
static int r_shared_created = -1, r_shared_direct = -1;
static int r_surfaceless = -1, r_pbuffer_current = -1;
static int r_fence_blocking = -1, r_fence_poll = -1;
static double r_poll_ms = -1.0;
static long r_poll_iters = -1;
static int r_texture_visible = -1, r_texture_content = -1;

static GLuint worker_tex;
static GLsync_ worker_fence;
static int worker_current = 0;

static int x_error_seen;
static int x_error_handler(Display *d, XErrorEvent *e) {
  char buf[256];
  XGetErrorText(d, e->error_code, buf, sizeof buf);
  fprintf(stderr, "  [X error] %s (code %d, request %d)\n", buf, e->error_code,
          e->request_code);
  x_error_seen = 1;
  return 0;
}

static double now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000.0 + ts.tv_nsec / 1.0e6;
}

/* The config sokol_app would choose: RGBA, GLX_WINDOW_BIT, depth 24. It
   never asks for GLX_PBUFFER_BIT, which is question C. */
static int pick_config(void) {
  int attrs[] = {GLX_RENDER_TYPE,  GLX_RGBA_BIT,  GLX_DRAWABLE_TYPE,
                 GLX_WINDOW_BIT,   GLX_RED_SIZE,  8,
                 GLX_GREEN_SIZE,   8,             GLX_BLUE_SIZE,
                 8,                GLX_ALPHA_SIZE, 8,
                 GLX_DEPTH_SIZE,   24,            GLX_DOUBLEBUFFER,
                 True,             None};
  int n = 0;
  GLXFBConfig *cfgs = glXChooseFBConfig(dpy, DefaultScreen(dpy), attrs, &n);
  if (!cfgs || !n) {
    printf("FAIL setup: no framebuffer config with GLX_WINDOW_BIT\n");
    return 0;
  }
  sokol_cfg = cfgs[0];
  int dt = 0;
  glXGetFBConfigAttrib(dpy, sokol_cfg, GLX_DRAWABLE_TYPE, &dt);
  r_cfg_has_pbuffer = (dt & GLX_PBUFFER_BIT) ? 1 : 0;
  printf("INFO chosen config drawable_type=0x%x (WINDOW=%d PIXMAP=%d PBUFFER=%d)\n",
         dt, !!(dt & GLX_WINDOW_BIT), !!(dt & GLX_PIXMAP_BIT),
         !!(dt & GLX_PBUFFER_BIT));
  printf("%s question C: sokol's own config %s advertise GLX_PBUFFER_BIT\n",
         r_cfg_has_pbuffer ? "INFO" : "NOTE",
         r_cfg_has_pbuffer ? "DOES" : "does NOT");
  XFree(cfgs);
  return 1;
}

static GLXContext make_context(GLXFBConfig cfg, GLXContext share) {
  int attribs[] = {GLX_CONTEXT_MAJOR_VERSION_ARB,
                   4,
                   GLX_CONTEXT_MINOR_VERSION_ARB,
                   3,
                   GLX_CONTEXT_PROFILE_MASK_ARB,
                   GLX_CONTEXT_CORE_PROFILE_BIT_ARB,
                   GLX_CONTEXT_FLAGS_ARB,
                   GLX_CONTEXT_FORWARD_COMPATIBLE_BIT_ARB,
                   0,
                   0};
  GLXContext c = pCreateContextAttribs(dpy, cfg, share, True, attribs);
  if (!c) { /* Some drivers cap lower; the design needs 4.3 but the probe
               should still report the rest rather than stop here. */
    attribs[1] = 3;
    attribs[3] = 3;
    c = pCreateContextAttribs(dpy, cfg, share, True, attribs);
    if (c)
      printf("NOTE fell back to a 3.3 core context (4.3 was refused)\n");
  }
  return c;
}

static int setup_base(void) {
  sokol_ctx = make_context(sokol_cfg, NULL);
  if (!sokol_ctx) {
    printf("FAIL setup: could not create the base context\n");
    return 0;
  }
  XVisualInfo *vi = glXGetVisualFromFBConfig(dpy, sokol_cfg);
  XSetWindowAttributes swa;
  memset(&swa, 0, sizeof swa);
  swa.colormap =
      XCreateColormap(dpy, RootWindow(dpy, vi->screen), vi->visual, AllocNone);
  swa.event_mask = StructureNotifyMask;
  Window w = XCreateWindow(dpy, RootWindow(dpy, vi->screen), 0, 0, 320, 240, 0,
                           vi->depth, InputOutput, vi->visual,
                           CWColormap | CWEventMask, &swa);
  glxwin = glXCreateWindow(dpy, sokol_cfg, w, NULL);
  XFree(vi);
  if (!glXMakeContextCurrent(dpy, glxwin, glxwin, sokol_ctx)) {
    printf("FAIL setup: could not make the base context current\n");
    return 0;
  }
  r_vendor = (const char *)glGetString(GL_VENDOR);
  r_renderer = (const char *)glGetString(GL_RENDERER);
  r_version = (const char *)glGetString(GL_VERSION);
  printf("INFO GL_VENDOR=%s\n", r_vendor);
  printf("INFO GL_RENDERER=%s\n", r_renderer);
  printf("INFO GL_VERSION=%s\n", r_version);
  printf("INFO GLX_VENDOR=%s\n",
         glXGetClientString(dpy, GLX_VENDOR));
  return 1;
}

/* Question A: everything here uses only what init_cb would have. */
static int setup_shared(void) {
  GLXContext cur = glXGetCurrentContext();
  if (cur != sokol_ctx) {
    printf("FAIL question A: glXGetCurrentContext() is not the base context\n");
    return 0;
  }
  int id = 0;
  if (glXQueryContext(dpy, cur, GLX_FBCONFIG_ID, &id) != Success) {
    printf("FAIL question A: glXQueryContext(GLX_FBCONFIG_ID) failed\n");
    return 0;
  }
  int idattrs[] = {GLX_FBCONFIG_ID, id, None};
  int n = 0;
  GLXFBConfig *found = glXChooseFBConfig(dpy, DefaultScreen(dpy), idattrs, &n);
  if (!found || !n) {
    printf("FAIL question A: config id 0x%x does not resolve to a handle\n", id);
    return 0;
  }
  printf("PASS question A-1: config id 0x%x resolved to a handle (%d match)\n",
         id, n);
  worker_ctx = make_context(found[0], sokol_ctx);
  XFree(found);
  r_shared_created = worker_ctx ? 1 : 0;
  if (!worker_ctx) {
    printf("FAIL question A-2: could not create a shared context\n");
    return 0;
  }
  r_shared_direct = glXIsDirect(dpy, worker_ctx) ? 1 : 0;
  printf("PASS question A-2: shared context created (%s)\n",
         r_shared_direct ? "direct" : "INDIRECT");

  int pbattrs[] = {GLX_PBUFFER_WIDTH, 1, GLX_PBUFFER_HEIGHT, 1, None};
  x_error_seen = 0;
  worker_pbuf = glXCreatePbuffer(dpy, sokol_cfg, pbattrs);
  XSync(dpy, False);
  if (!worker_pbuf || x_error_seen) {
    worker_pbuf = 0;
    printf("NOTE question C-2: no pbuffer from sokol's config\n");
  } else {
    printf("INFO question C-2: a pbuffer WAS created from sokol's config\n");
  }
  return 1;
}

/* Generate enough GPU work that the fence is not signalled instantly, so
   the polling measurement below means something. */
static void make_gpu_work(void) {
  GLuint fbo = 0, tex = 0;
  glGenTextures(1, &tex);
  glBindTexture(GL_TEXTURE_2D, tex);
  glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, 2048, 2048);
  glGenFramebuffers(1, &fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                         tex, 0);
  glViewport(0, 0, 2048, 2048);
  for (int i = 0; i < 200; i++) {
    glClearColor((float)(i % 16) / 16.0f, 0.2f, 0.4f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
  }
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

static void *worker_main(void *arg) {
  (void)arg;
  x_error_seen = 0;
  if (glXMakeContextCurrent(dpy, None, None, worker_ctx) && !x_error_seen) {
    r_surfaceless = 1;
    worker_current = 1;
    printf("PASS question B-1: shared context current on another thread, NO drawable\n");
  } else {
    r_surfaceless = 0;
    printf("NOTE question B-1: surfaceless make-current rejected\n");
    if (worker_pbuf) {
      x_error_seen = 0;
      if (glXMakeContextCurrent(dpy, worker_pbuf, worker_pbuf, worker_ctx) &&
          !x_error_seen) {
        r_pbuffer_current = 1;
        worker_current = 1;
        printf("PASS question B-2: shared context current with a 1x1 pbuffer\n");
      } else {
        r_pbuffer_current = 0;
        printf("FAIL question B-2: pbuffer make-current rejected too\n");
      }
    }
  }
  if (!worker_current)
    return NULL;

  /* Question E: a texture with known content. */
  glGenTextures(1, &worker_tex);
  glBindTexture(GL_TEXTURE_2D, worker_tex);
  glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, 4, 4);   /* immutable: required
                                                         for texture views */
  unsigned char px[4 * 4 * 4];
  for (int i = 0; i < 4 * 4; i++) {
    px[i * 4 + 0] = 0x11; px[i * 4 + 1] = 0x22;
    px[i * 4 + 2] = 0x33; px[i * 4 + 3] = 0xFF;
  }
  glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 4, 4, GL_RGBA, GL_UNSIGNED_BYTE, px);
  printf("INFO worker created texture %u (gl error 0x%x)\n", worker_tex,
         glGetError());

  /* Question D: real work, then a fence, then flush. The design's producer
     does exactly this before publishing. */
  make_gpu_work();
  worker_fence = pFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
  glFlush();
  printf("INFO worker inserted fence %p after ~200 full-screen clears\n",
         (void *)worker_fence);
  return NULL;
}

int main(void) {
  XInitThreads();
  XSetErrorHandler(x_error_handler);
  dpy = XOpenDisplay(NULL);
  if (!dpy) {
    printf("FAIL setup: no display. Run this in a real X session.\n");
    return 1;
  }
  pCreateContextAttribs = (PFNGLXCREATECONTEXTATTRIBSARBPROC_)
      glXGetProcAddressARB((const GLubyte *)"glXCreateContextAttribsARB");
  pFenceSync = (PFNGLFENCESYNCPROC_)glXGetProcAddressARB(
      (const GLubyte *)"glFenceSync");
  pClientWaitSync = (PFNGLCLIENTWAITSYNCPROC_)glXGetProcAddressARB(
      (const GLubyte *)"glClientWaitSync");
  pDeleteSync = (PFNGLDELETESYNCPROC_)glXGetProcAddressARB(
      (const GLubyte *)"glDeleteSync");
  if (!pCreateContextAttribs || !pFenceSync || !pClientWaitSync) {
    printf("FAIL setup: missing glXCreateContextAttribsARB or the sync API\n");
    return 1;
  }
  if (!pick_config() || !setup_base() || !setup_shared())
    return 1;

  pthread_t th;
  pthread_create(&th, NULL, worker_main, NULL);
  pthread_join(th, NULL);

  if (worker_current && worker_fence) {
    /* D-1: poll with timeout 0, which is what frame_cb does. */
    double t0 = now_ms();
    long iters = 0;
    GLenum r = GL_TIMEOUT_EXPIRED;
    while (now_ms() - t0 < 2000.0) {
      r = pClientWaitSync(worker_fence, 0, 0);
      iters++;
      if (r == GL_ALREADY_SIGNALED || r == GL_CONDITION_SATISFIED)
        break;
      if (r == GL_WAIT_FAILED)
        break;
    }
    r_poll_ms = now_ms() - t0;
    r_poll_iters = iters;
    r_fence_poll = (r == GL_ALREADY_SIGNALED || r == GL_CONDITION_SATISFIED);
    printf("%s question D-1: timeout-0 polling from the main thread -> 0x%x after %ld poll(s), %.2f ms\n",
           r_fence_poll ? "PASS" : "FAIL", r, iters, r_poll_ms);

    /* D-2: a blocking wait, for contrast. A fence visible only to this and
       not to the poll above would be useless to the design. */
    GLenum rb = pClientWaitSync(worker_fence, GL_SYNC_FLUSH_COMMANDS_BIT,
                                1000000000ull);
    r_fence_blocking = (rb == GL_ALREADY_SIGNALED || rb == GL_CONDITION_SATISFIED);
    printf("%s question D-2: blocking wait -> 0x%x\n",
           r_fence_blocking ? "PASS" : "FAIL", rb);
    if (pDeleteSync)
      pDeleteSync(worker_fence);
  }

  if (worker_current && worker_tex) {
    r_texture_visible = glIsTexture(worker_tex) ? 1 : 0;
    printf("%s question E-1: worker's texture %u %s visible here\n",
           r_texture_visible ? "PASS" : "FAIL", worker_tex,
           r_texture_visible ? "is" : "is NOT");
    if (r_texture_visible) {
      /* Read it back through an FBO: name visibility is not content. */
      GLuint fbo = 0;
      unsigned char out[4] = {0, 0, 0, 0};
      glGenFramebuffers(1, &fbo);
      glBindFramebuffer(GL_FRAMEBUFFER, fbo);
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                             GL_TEXTURE_2D, worker_tex, 0);
      GLenum fbs = glCheckFramebufferStatus(GL_FRAMEBUFFER);
      if (fbs == GL_FRAMEBUFFER_COMPLETE) {
        glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, out);
        r_texture_content =
            (out[0] == 0x11 && out[1] == 0x22 && out[2] == 0x33) ? 1 : 0;
        printf("%s question E-2: read back 0x%02x%02x%02x, expected 0x112233\n",
               r_texture_content ? "PASS" : "FAIL", out[0], out[1], out[2]);
      } else {
        printf("FAIL question E-2: framebuffer incomplete (0x%x)\n", fbs);
      }
      glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
  }

  printf("\n==== COPY THIS BLOCK INTO THE RESULTS NOTE ====\n");
  printf("vendor:            %s\n", r_vendor);
  printf("renderer:          %s\n", r_renderer);
  printf("gl_version:        %s\n", r_version);
  printf("config_has_pbuffer: %d   (C: 1 = llvmpipe-like, 0 = the case that matters)\n",
         r_cfg_has_pbuffer);
  printf("shared_created:    %d   direct: %d\n", r_shared_created,
         r_shared_direct);
  printf("surfaceless_ok:    %d   pbuffer_current_ok: %d\n", r_surfaceless,
         r_pbuffer_current);
  printf("fence_poll_ok:     %d   polls: %ld   ms: %.2f\n", r_fence_poll,
         r_poll_iters, r_poll_ms);
  printf("fence_blocking_ok: %d\n", r_fence_blocking);
  printf("texture_visible:   %d   content_ok: %d\n", r_texture_visible,
         r_texture_content);
  printf("==============================================\n");
  return 0;
}
