// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// Canvas: an animation drawn with the draw list into a child window, which
// is what clips it (the draw list's own clip-rect stack is not bound).
// Orbiting circles with trails, a rotating hexagon, and a Bezier curve
// whose control points move; a speed slider, a pause checkbox and the
// centre circle's colour. Dragging in the canvas moves the centre.
//
// Every position is a function of the frame's time (ctx.time, seconds),
// never of a frame count, so the speed does not depend on the frame rate.
// orbitPosition(t) is that function for the orbiting circle, used by the
// drawing and by test/ui-imgui-app.js alike.

'use strict';

/** Radians per second of the orbiting circle at speed 1. */
const OMEGA = Math.PI / 2;
/** The orbit's radius, as a fraction of the canvas's shorter side. */
const ORBIT_FRACTION = 0.35;
const ORBIT_DOT = 10;
const CENTER_DOT = 16;
const TRAIL = 12;
const TRAIL_STEP = 0.04; // seconds between trail dots

const speed = [1];
const paused = [false];
const centerColor = [1, 0.5, 0];
const orbitColor = [0, 1, 1, 1];
const innerColor = [1, 0, 1, 1];
const shapeColor = [0.9, 0.9, 0.9, 1];
const curveColor = [0.4, 0.7, 1, 1];

// The animation's angle is `phase` at time `phaseTime`, advancing at the
// speed in force since then; a change of speed or of pause starts a new
// segment at the angle reached, so the circle never jumps.
let phase = 0;
let phaseTime = 0;
let appliedSpeed = speed[0];
let appliedPaused = paused[0];

// The last drawn geometry, in logical display coordinates.
const center = [0, 0];
let radius = 0;
const offset = [0, 0];
const dragBase = [0, 0];

const scratch = {
  origin: [0, 0],
  avail: [0, 0],
  drag: [0, 0],
  size: [1, 1],
  hexagon: new Float32Array(12),
};

function angleAt(t) {
  return appliedPaused ? phase : phase + (t - phaseTime) * appliedSpeed * OMEGA;
}

/**
 * The orbiting circle's centre at time `t` (seconds), in logical units
 * relative to the display, for the canvas as last drawn.
 */
function orbitPosition(t) {
  const a = angleAt(t);
  return [center[0] + radius * Math.cos(a), center[1] + radius * Math.sin(a)];
}

function rebase(t) {
  if (speed[0] === appliedSpeed && paused[0] === appliedPaused)
    return;
  phase = angleAt(t);
  phaseTime = t;
  appliedSpeed = speed[0];
  appliedPaused = paused[0];
}

function withAlpha(rgba, a) {
  return [rgba[0], rgba[1], rgba[2], rgba[3] * a];
}

function draw(ctx) {
  const {imgui} = ctx;
  const t = ctx.time;

  imgui.setNextItemWidth(120);
  imgui.sliderFloat('speed', speed, 0, 4, '%.2f');
  imgui.sameLine();
  imgui.checkbox('pause', paused);
  imgui.sameLine();
  imgui.colorEdit3('center', centerColor, imgui.ColorEditFlags_NoInputs);
  rebase(t);

  if (imgui.beginChild('area', [0, 0], imgui.ChildFlags_Borders,
          imgui.WindowFlags_NoScrollbar |
              imgui.WindowFlags_NoScrollWithMouse)) {
    const {origin, avail, drag, size, hexagon} = scratch;
    imgui.getCursorScreenPos(origin);
    imgui.getContentRegionAvail(avail);
    // An invisible button over the whole area takes the drag; ImGui needs
    // a size above zero, which a collapsed window does not give.
    size[0] = Math.max(avail[0], 1);
    size[1] = Math.max(avail[1], 1);
    imgui.invisibleButton('canvas', size);
    if (imgui.isItemActive()) {
      imgui.getMouseDragDelta(0, 0, drag);
      offset[0] = dragBase[0] + drag[0];
      offset[1] = dragBase[1] + drag[1];
    } else {
      dragBase[0] = offset[0];
      dragBase[1] = offset[1];
    }
    center[0] = origin[0] + size[0] / 2 + offset[0];
    center[1] = origin[1] + size[1] / 2 + offset[1];
    radius = ORBIT_FRACTION * Math.min(size[0], size[1]);

    const dl = imgui.drawList;
    const u32 = imgui.colorConvertFloat4ToU32;
    const a = angleAt(t);
    const [cx, cy] = center;

    // A hexagon turning at half the orbit's rate.
    for (let i = 0; i < 6; ++i) {
      const ha = a / 2 + i * Math.PI / 3;
      hexagon[2 * i] = cx + 0.75 * radius * Math.cos(ha);
      hexagon[2 * i + 1] = cy + 0.75 * radius * Math.sin(ha);
    }
    dl.addPolyline(hexagon, u32(shapeColor), 2, imgui.DrawFlags_Closed);

    // A curve below the centre whose inner control points swing.
    const swing = 0.5 * radius * Math.sin(a);
    dl.addBezierCubic(
        [cx - 1.2 * radius, cy + 0.9 * radius],
        [cx - 0.4 * radius, cy + 0.9 * radius + swing],
        [cx + 0.4 * radius, cy + 0.9 * radius - swing],
        [cx + 1.2 * radius, cy + 0.9 * radius], u32(curveColor), 3);

    // An inner circle orbiting the other way, twice as fast.
    const ia = Math.PI - 2 * a;
    dl.addCircleFilled(
        [cx + 0.5 * radius * Math.cos(ia), cy + 0.5 * radius * Math.sin(ia)],
        8, u32(innerColor));

    // The orbiting circle's trail, then the circle, then the centre, each
    // over what came before.
    for (let k = TRAIL; k >= 1; --k) {
      const fade = 1 - k / (TRAIL + 1);
      dl.addCircleFilled(orbitPosition(t - k * TRAIL_STEP),
          ORBIT_DOT * (0.3 + 0.7 * fade),
          u32(withAlpha(orbitColor, 0.6 * fade)));
    }
    dl.addCircleFilled(orbitPosition(t), ORBIT_DOT, u32(orbitColor));
    dl.addCircleFilled(center, CENTER_DOT,
        u32([centerColor[0], centerColor[1], centerColor[2], 1]));
    dl.addCircle(center, CENTER_DOT + 4, u32(shapeColor), 0, 1.5);
  }
  imgui.endChild();
}

module.exports = {
  name: 'Canvas',
  open: [true],
  windowFlags: 0,
  orbitPosition,
  center,
  centerColor,
  orbitColor,
  draw,
};
