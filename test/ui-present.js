// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// **Slice B's payoff: the first test whose gate is a picture.**
//
// The reference addon draws a fixed pattern into a compositor surface on
// the JS thread; the platform thread blits it into the window; the capture
// hook reads the presented pixels back; and this file compares them. Every
// layer built in Tasks 1-9 is on that path, so a break anywhere in it
// shows up here as a wrong picture rather than as nothing at all.
//
// Four things are asserted, and the third is the one that needed a new
// field in the protocol:
//
//  1. The presented pixels ARE the pattern (exact comparison; policy
//     below).
//  2. A capture is taken only when every agreement field matches, and it
//     reports the publication id it matched.
//  3. **A re-blit of the still-presenting surface does not satisfy a
//     request for the NEXT publication** (spec: capture "is keyed to a
//     publication id and acknowledged, never to an absolute frame number,
//     because blits can precede any publication"). Without this
//     assertion, `CaptureRequest::publicationId` is untested against the
//     only scenario it exists for -- and since an unwired id is 0 on both
//     sides, 0 == 0 would match and every other assertion here would stay
//     green.
//  4. Both completion modes work, asserted from HERMES_NODE_DEBUG_NATIVE
//     tracing rather than from timing (the suite runs 16-way parallel,
//     and dz 01a0d7d4-b666 records what a wall-clock assertion costs).
//
// --- THE TOLERANCE POLICY, and why it is exact ---------------------
//
// Interior samples are compared EXACTLY -- zero difference in R, G and B
// -- and no sample is taken within 8 pixels of a pattern edge. Both halves
// matter, and the second is what buys the first.
//
// **The policy is scoped to requests that name EQUAL target and
// framebuffer sizes, which is what every request in this file does.** The
// mechanism does not enforce equality: `maybeCapture()` reports the
// surface's own size and the window's framebuffer size as two separate
// fields and compares each against the request, so a request naming
// UNEQUAL sizes -- the transitional-stretching case spec:991 anticipates
// as its own separate assertion -- would legitimately match a stretched,
// linearly filtered frame, and none of the reasoning below would hold for
// it. That case is test/ui-resize-generation.js, which states its own
// policy; this file asks for equal sizes and therefore gets a 1:1 blit.
//
// Given that, the agreement rule means the frame that matched had
// targetWidth == framebufferWidth and targetHeight == framebufferHeight, so
// the blit is a 1:1 texel-to-pixel map; linear filtering at texel centres
// then reduces to a copy, weighting one texel by 1 and its neighbours by 0. The
// pattern is flat-shaded with no blending, no depth test and no sRGB
// conversion, and every channel it uses is 0.0 or 1.0, which land on 0 and
// 255 under any float-to-unorm8 rounding rule a driver might choose. So
// inside a solid region there is no mechanism that can legitimately
// produce a different byte, and any tolerance at all would only hide a
// real defect -- a wrong clear colour, a swapped channel, a blit that did
// not happen. Edges are the one place where rasterization and filtering
// legitimately produce in-between values, and they are EXCLUDED by
// construction rather than tolerated, which is what makes "exactly"
// affordable. (spec: "solid interior regions are compared exactly, and
// rasterized or filtered edges with a tolerance, with the pixel format
// and any colour conversion stated by the test".)
//
// REQUIRES: ui, ui-gpu
// REQUIRES: display

// --- Why the RUN lines look like this. ------------------------------
//
// Two runs, covering the two frame-driving modes AND the two completion
// modes in one pair rather than four: `js` drives frames from a
// JavaScript rAF loop with polling completion, `native` drives them from
// the addon's own hnui_request_animation_frame with the blocking
// fallback. Both modes of both mechanisms therefore have a caller, which
// is what the plan's Step 6 asks for.
//
// **The redirections are INSIDE `sh -c`, and that was load-bearing rather
// than a style choice under `xvfb-run`, which merges its child's stderr
// into stdout.** Measured, not assumed -- `xvfb-run -a sh -c 'echo A; echo
// B >&2'` with the two streams sent to different files put both lines in
// the stdout file and left the stderr file empty. Written the obvious way,
// with `> %t.out 2> %t.err` outside, the trace assertions below would have
// run against an EMPTY file and passed without reading anything, which is
// the silent-green failure this suite keeps being bitten by. `%with-display`
// (utils/with-display.py, over utils/with-xvfb.py on Linux, which replaced
// xvfb-run because its choice of display is not atomic) does not merge --
// but the shape is kept: with the
// files attached to the binary's own fds by the inner shell, no wrapper
// sitting between lit and the binary can empty them.
//
// `timeout` is load-bearing too, for the same reason it is in
// ui-create-window.js: the interesting mutations here do not print
// something wrong, they print nothing and wait forever on a capture that
// never matches. lit configures no per-test timeout (dz 01a0d797-e326),
// so without the bound such a mutation would stall a worker slot instead
// of failing. With it, a hang exits 124 and the EXITCODE check goes red.
//
// RUN: %with-display 300 sh -c 'HERMES_NODE_UI_TEST_HOOKS=1 HERMES_NODE_DEBUG_NATIVE=UI %gl-completion-poll %hermes-node-ui %s %hnui_demo_addon js %t.poll.ppm > %t.poll.out 2> %t.poll.err'; echo "EXITCODE $?" | %FileCheck --check-prefix=POLLCODE %s
// RUN: %FileCheck --check-prefix=POLL %s < %t.poll.out
// RUN: %FileCheck --implicit-check-not='[sapp:error' --implicit-check-not='[sokol-' --check-prefixes=POLLERR,POLLERR-%ui-backend %s < %t.poll.err

// RUN: %with-display 300 sh -c 'HERMES_NODE_UI_TEST_HOOKS=1 HERMES_NODE_DEBUG_NATIVE=UI %gl-completion-block %hermes-node-ui %s %hnui_demo_addon native %t.block.ppm > %t.block.out 2> %t.block.err'; echo "EXITCODE $?" | %FileCheck --check-prefix=BLOCKCODE %s
// RUN: %FileCheck --check-prefix=BLOCK %s < %t.block.out
// RUN: %FileCheck --implicit-check-not='[sapp:error' --implicit-check-not='[sokol-' --check-prefixes=BLOCKERR,BLOCKERR-%ui-backend %s < %t.block.err

// --- A third run with NO override, so the stage-2 PROBE runs. --------
//
// The two runs above both set HERMES_NODE_UI_GL_COMPLETION, which takes
// the early return in setupProducerInstance() and skips the probe
// entirely -- so without this run the probe, its handshake, its budget
// and its GL_WAIT_FAILED rule are executed by no assertion in the suite,
// while being exactly what a real user's run performs. This run leaves
// the variable unset and lets the probe decide.
//
// RUN: %with-display 300 sh -c 'HERMES_NODE_UI_TEST_HOOKS=1 HERMES_NODE_DEBUG_NATIVE=UI %hermes-node-ui %s %hnui_demo_addon js %t.probe.ppm > %t.probe.out 2> %t.probe.err'; echo "EXITCODE $?" | %FileCheck --check-prefix=PROBECODE %s
// RUN: %FileCheck --check-prefix=PROBE %s < %t.probe.out
// RUN: %FileCheck --implicit-check-not='[sapp:error' --implicit-check-not='[sokol-' --check-prefixes=PROBEERR,PROBEERR-%ui-backend %s < %t.probe.err

'use strict';

const fs = require('fs');
const ui = require('hermes-node:ui');

const addonPath = process.argv[2];
const mode = process.argv[3];
const capturePath = process.argv[4];

// How many presented frames the negative case waits before concluding
// that a request was not satisfied. A budget in FRAMES, never in
// milliseconds: the suite runs 16-way parallel and the plan forbids
// timing-based assertions, so "it did not happen" is established by
// letting a fixed number of real presented frames go past.
const REBLIT_FRAME_BUDGET = 240;

// How many presented frames the slot-recycling case (step 4) allows for
// two draws to be published. The same generous figure the other requests
// that are expected to match use -- the platform thread keeps presenting
// while a loaded machine starves the JS thread -- and it is also the whole
// cost of the failing case, which ends when the budget does.
const RECYCLE_FRAME_BUDGET = 900;

// The pattern the addon draws, as bytes. Every channel is 0 or 255; see
// the tolerance policy above for why that is a requirement and not a
// coincidence.
const BLUE = [0, 0, 255];
const YELLOW = [255, 255, 0];

/** Parse the binary PPM (P6) the capture hook wrote. */
function readPpm(path) {
  const buf = fs.readFileSync(path);
  // "P6\n<w> <h>\n255\n" -- written by writeCapturePpm(), so the shape is
  // fixed and a hand-rolled parse is honest here.
  let offset = 0;
  function token() {
    while (offset < buf.length && /\s/.test(String.fromCharCode(buf[offset]))) {
      offset++;
    }
    const start = offset;
    while (
        offset < buf.length && !/\s/.test(String.fromCharCode(buf[offset]))) {
      offset++;
    }
    return buf.toString('ascii', start, offset);
  }
  const magic = token();
  const width = parseInt(token(), 10);
  const height = parseInt(token(), 10);
  const maxValue = parseInt(token(), 10);
  offset++; // the single whitespace byte after the header
  if (magic !== 'P6' || maxValue !== 255) {
    throw new Error('not a P6/255 PPM: ' + magic + ' ' + maxValue);
  }
  return {width, height, data: buf.slice(offset)};
}

function pixelAt(image, x, y) {
  const i = (y * image.width + x) * 3;
  return [image.data[i], image.data[i + 1], image.data[i + 2]];
}

function sameColor(a, b) {
  return a[0] === b[0] && a[1] === b[1] && a[2] === b[2];
}

function describe(p) {
  return '(' + p[0] + ',' + p[1] + ',' + p[2] + ')';
}

/**
 * Check the four quadrant interiors.
 *
 * The pattern is asymmetric in BOTH axes on purpose, so a vertical flip, a
 * horizontal flip and a channel swap are three distinguishable failures
 * rather than one ambiguous one: only the top-left quadrant is yellow.
 */
function checkPattern(image) {
  // 8 px in from the quadrant boundaries and the image edges. The margin
  // is what lets the comparison be exact -- see the tolerance policy.
  const margin = 8;
  const midX = image.width >> 1;
  const midY = image.height >> 1;
  const samples = [
    {name: 'TOPLEFT', x: midX - margin, y: margin, want: YELLOW},
    {name: 'TOPRIGHT', x: midX + margin, y: margin, want: BLUE},
    {name: 'BOTTOMLEFT', x: margin, y: image.height - margin, want: BLUE},
    {
      name: 'BOTTOMRIGHT',
      x: image.width - margin,
      y: image.height - margin,
      want: BLUE,
    },
    // One more inside the yellow quadrant, away from both its edges, so
    // that a pattern which merely tinted a single corner pixel cannot
    // pass.
    {name: 'TOPLEFTDEEP', x: margin, y: midY - margin, want: YELLOW},
  ];

  let allOk = true;
  for (const sample of samples) {
    const got = pixelAt(image, sample.x, sample.y);
    const ok = sameColor(got, sample.want);
    allOk = allOk && ok;
    console.log(
        sample.name + ' ' +
        (ok ? 'ok' :
              'WRONG got=' + describe(got) + ' want=' + describe(sample.want)));
  }
  return allOk;
}

async function main() {
  const win = await ui.createWindow({
    width: 320,
    height: 240,
    title: 'hermes-node-ui present test',
  });
  // Loaded after the window exists: hnui_get_window(env) answers null
  // until then.
  const addon = require(addonPath);

  // The agreement fields every request below shares. The generation is 1
  // because nothing in this test resizes and SurfaceTable's ids start
  // there. It is stated rather than read back from the runtime, so that a
  // change in it is a test failure rather than a silently updated
  // expectation.
  const agreement = {
    generation: 1,
    targetWidth: win.framebufferWidth,
    targetHeight: win.framebufferHeight,
    framebufferWidth: win.framebufferWidth,
    framebufferHeight: win.framebufferHeight,
    dpiScale: win.dpiScale,
  };

  const capture = (publicationId, path, giveUpAfterFrames) =>
      ui.__captureFrame(Object.assign(
          {path, publicationId, giveUpAfterFrames}, agreement));

  /**
   * Draw exactly one frame, and resolve when the callback has run.
   *
   * **One frame at a time, not a self-perpetuating chain**, and that is
   * what makes the re-blit case below deterministic rather than hopeful.
   * A chain that re-registers itself leaves a callback in flight at every
   * moment, so "stop drawing" only means "stop eventually" -- measured:
   * an earlier version of this test set a flag to stop the chain and
   * still saw two further publications land during what was supposed to
   * be a quiet period, which made the negative assertion pass for the
   * wrong reason. With this, once the awaited callback has run, nothing
   * is registered and no publication is POSSIBLE.
   */
  const drawOnce = () => new Promise((resolve) => {
    ui.requestAnimationFrame(() => {
      addon.drawFixedPattern();
      resolve();
    });
  });

  // --- 1. Capture the first publication. ---------------------------
  //
  // The request is installed BEFORE anything draws. Installed afterwards,
  // publication 1 could already have been superseded by the time the
  // presenter was asked about it -- which would make this test pass or
  // fail depending on how fast the machine is, and it is the native mode
  // (a free-running loop) where that would bite first.
  const firstPending = capture(1, capturePath, 900);
  if (mode === 'native') {
    addon.startNativeFrames();
  } else {
    await drawOnce();
  }
  const first = await firstPending;
  console.log(
      'FIRST matched=' + first.matched +
      ' publicationId=' + first.publicationId);

  const image = readPpm(capturePath);
  console.log(
      'SIZE ' +
      (image.width === win.framebufferWidth &&
       image.height === win.framebufferHeight));
  console.log('PATTERN ' + (checkPattern(image) ? 'ok' : 'WRONG'));

  if (mode === 'js') {
    // --- 2. The re-blit case. ------------------------------------
    //
    // Nothing is registered at this point -- drawOnce() registered
    // exactly one callback and it has run -- so NO publication can
    // happen. The presenter nonetheless keeps blitting the surface that
    // is still presenting, once per frame callback, reporting the same
    // generation, the same sizes, the same DPI scale and the same input
    // serial as the request below. The one field that differs is the
    // publication id, and it is what must refuse every one of those
    // frames.
    const reblit =
        await capture(2, capturePath + '.reblit', REBLIT_FRAME_BUDGET);
    console.log(
        'REBLIT matched=' + reblit.matched +
        ' framesObserved=' + reblit.framesObserved);

    // --- 3. And the id is not merely inert. ----------------------
    //
    // Draw one more frame and the SAME request now matches. Without this,
    // "it never matched" would also be satisfied by a capture mechanism
    // that had stopped working altogether -- the way a test for a refusal
    // is also a test that the thing being refused still exists.
    const pending = capture(2, capturePath + '.after', 900);
    await drawOnce();
    const resumed = await pending;
    console.log(
        'RESUMED matched=' + resumed.matched +
        ' publicationId=' + resumed.publicationId);

    // --- 4. A presented surface's slot comes back. -----------------
    //
    // A generation has three slots, and publications 1 and 2 have used
    // two of them. Two more draws make publication 4 the first frame that
    // can only be drawn into a slot an earlier publication held -- and a
    // slot returns to FREE only once the backend has reported, through its
    // consumer-completion sink, that every blit which sampled it has
    // finished. A backend that never calls that sink presents three
    // frames correctly and then never draws again. No request above
    // notices: none needs a fourth frame.
    //
    // The request is installed before either draw, for the reason given at
    // step 1. And the second draw is registered from inside the first's
    // callback, with neither awaited: when no slot is free the callback is
    // simply not run, so awaiting one would turn "the slot never came
    // back" into a hang that ends at the RUN line's time limit. This way
    // the request gives up after its frame budget and the failure is one
    // line reading `matched=false`.
    const recycledPending =
        capture(4, capturePath + '.recycled', RECYCLE_FRAME_BUDGET);
    ui.requestAnimationFrame(() => {
      addon.drawFixedPattern();
      ui.requestAnimationFrame(() => {
        addon.drawFixedPattern();
      });
    });
    const recycled = await recycledPending;
    console.log(
        'RECYCLED matched=' + recycled.matched +
        ' publicationId=' + recycled.publicationId);
  }

  console.log('DONE');
  // Required, not tidy: createWindow takes a REFERENCED libuv handle that
  // only closing the window releases, so a script that simply returned
  // would keep running with its window open. Same note as
  // ui-create-window.js.
  process.exit(0);
}

main().catch((e) => {
  console.log('FAILED ' + (e && e.message));
  process.exit(1);
});

// --- The JS-driven, polling-completion run. --------------------------
//
// POLL: FIRST matched=true publicationId=1
// POLL-NEXT: SIZE true
// POLL-NEXT: TOPLEFT ok
// POLL-NEXT: TOPRIGHT ok
// POLL-NEXT: BOTTOMLEFT ok
// POLL-NEXT: BOTTOMRIGHT ok
// POLL-NEXT: TOPLEFTDEEP ok
// POLL-NEXT: PATTERN ok
//
// **The publication-id assertion.** `matched=false` after 240 presented
// frames is the whole point: every one of those frames re-blitted the
// surface that was still presenting and agreed with the request on
// generation, both sizes and DPI scale -- and was refused, because its
// publication id was the one already shown rather than the one asked for.
// Remove `publicationId` from CaptureRequest's comparison and this line
// reads `matched=true framesObserved=1`.
// POLL-NEXT: REBLIT matched=false framesObserved=240
// POLL-NEXT: RESUMED matched=true publicationId=2
//
// **The slot-recycling assertion.** Publication 4 is the first that needs
// a slot back from an earlier one. Stop the backend calling its
// consumer-completion sink and this line reads `matched=false
// publicationId=0`: the slots that were presented keep their read
// references, the fourth draw is never run, and the request gives up.
// POLL-NEXT: RECYCLED matched=true publicationId=4
// POLL-NEXT: DONE
// POLLCODE: EXITCODE 0
//
// The completion mode, from tracing rather than from timing.
//
// **Two lines, and the second is the one that proves the mode RAN.** The
// first is printed from `mode_` -- the SELECTION -- at the moment the probe
// or the override settles it, and nothing about it depends on a submission
// ever taking that path: delete the `if (mode == CompletionMode::kBlocking)`
// arm of submitAndTrackCompletion() and the BLOCK run below still selects
// blocking, still prints its mode line, still produces an exactly-correct
// picture, and still passes every check it has. The second line is traced
// from INSIDE the arm (once per run, not once per frame), so it is evidence
// about the code that executed rather than about the code that was chosen.
// Both mutations were run: each turns its own arm red and leaves the other
// green.
//
// A third layer is conceivable and is NOT closed here -- an arm that traced
// and then did none of its work would still pass. What stands against that
// is not this line: it is the picture the run has to produce, and, for the
// polling arm specifically, that a frame in that mode can only be completed
// by the platform thread, so the capture line beneath cannot appear unless
// the hand-off was picked up there.
// POLLERR-gl: [ui] completion mode: polling
// POLLERR-gl: [ui] completion: polling submission handed the fence to the platform thread
// On Metal there is one completion mechanism and no mode to choose, so the
// three runs make the same claims. Each `handler ran` line is printed
// from inside a Metal completion handler after it read the buffer's
// status, once per run: evidence that a handler executed on a real buffer,
// where the mode line only says which mechanism this build has. The
// staging line is printed from init_cb, before the worker starts, with
// framebufferOnly read back from Sokol's layer after it was cleared.
// POLLERR-metal: [ui] capture: staging copy enabled (framebufferOnly=0)
// POLLERR-metal: [ui] completion mode: handler (metal)
// POLLERR-metal: [ui] completion: producer handler ran (status=completed)
// POLLERR-metal: [ui] completion: consumer handler ran (status=completed)
// POLLERR: [ui] capture: written generation 1 publication 1
// POLLERR: [ui] capture: no match after 240 presented frame(s)
// POLLERR: [ui] capture: written generation 1 publication 4

// --- The addon-driven, blocking-completion run. ----------------------
//
// The re-blit case is deliberately absent here: the addon's native loop
// re-registers itself every tick, so this run never stops publishing and
// has no quiet period in which a request for the next publication could
// go unsatisfied. That case belongs to the mode that controls its own
// loop, and it is asserted there.
// BLOCK: FIRST matched=true publicationId=1
// BLOCK-NEXT: SIZE true
// BLOCK-NEXT: TOPLEFT ok
// BLOCK-NEXT: TOPRIGHT ok
// BLOCK-NEXT: BOTTOMLEFT ok
// BLOCK-NEXT: BOTTOMRIGHT ok
// BLOCK-NEXT: TOPLEFTDEEP ok
// BLOCK-NEXT: PATTERN ok
// BLOCK-NEXT: DONE
// BLOCKCODE: EXITCODE 0
//
// BLOCKERR-gl: [ui] completion mode: blocking
// BLOCKERR-gl: [ui] completion: blocking submission waited on the producer thread
// BLOCKERR-metal: [ui] capture: staging copy enabled (framebufferOnly=0)
// BLOCKERR-metal: [ui] completion mode: handler (metal)
// BLOCKERR-metal: [ui] completion: producer handler ran (status=completed)
// BLOCKERR-metal: [ui] completion: consumer handler ran (status=completed)
// BLOCKERR: [ui] capture: written generation 1 publication 1

// --- The probe run. -------------------------------------------------
//
// PROBE: FIRST matched=true publicationId=1
// PROBE-NEXT: SIZE true
// PROBE: PATTERN ok
// PROBE-NEXT: REBLIT matched=false framesObserved=240
// PROBE-NEXT: RESUMED matched=true publicationId=2
// PROBE-NEXT: RECYCLED matched=true publicationId=4
// PROBE-NEXT: DONE
// PROBECODE: EXITCODE 0
//
// **The probe's mode must match the probe's REASON**, which is the
// property worth pinning and the one a driver cannot change. Which answer
// a given driver gives is not asserted -- llvmpipe and radeonsi both
// poll, NVIDIA is unmeasured, and pinning "polling" would make this test
// a statement about the machine rather than about the code. What is
// pinned is the pairing: polling only ever follows a fence that
// SIGNALLED, and blocking only ever follows GL_WAIT_FAILED or a fence
// that did not signal in budget. Requiring "(probe:" at all is also what
// proves the probe ran rather than an override having short-circuited it.
//
// A mutation that selects blocking on a fence that signalled produces
// "blocking (probe: the producer's fence signalled", which matches
// neither alternative and fails here -- whereas a check for "(probe:"
// alone would have accepted it.
// PROBEERR-gl: [ui] completion mode: {{(polling \(probe: the producer's fence signalled|blocking \(probe: (GL_WAIT_FAILED|the producer's fence did not signal))}}
//
// And that the probe run really took one of the two submission paths. The
// alternation is required here for the same reason the line above needs one:
// which arm a given driver selects is not this test's business, and pinning
// either would make this a statement about the machine.
// PROBEERR-gl: [ui] completion: {{(polling submission handed the fence|blocking submission waited)}}
//
// Metal: no probe and no mode, so the probe run's claims are the other
// two runs' (see POLLERR-metal above).
// PROBEERR-metal: [ui] capture: staging copy enabled (framebufferOnly=0)
// PROBEERR-metal: [ui] completion mode: handler (metal)
// PROBEERR-metal: [ui] completion: producer handler ran (status=completed)
// PROBEERR-metal: [ui] completion: consumer handler ran (status=completed)
