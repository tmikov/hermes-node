/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_NODE_COMPAT_COMPOSITOR_GPU_PRESENT_CONSTANTS_H
#define HERMES_NODE_COMPAT_COMPOSITOR_GPU_PRESENT_CONSTANTS_H

/// Constants every presenter shares, whichever backend it is written for.
/// Kept apart from any backend's header because the tests assert them
/// against pixels: a second backend that chose its own value would make a
/// captured frame mean something different per platform.

namespace hermes {
namespace node_compat {

/// What the window shows while nothing has ever been published, in every
/// channel (alpha 1).
///
/// Deliberately a level nothing else in this system draws: the reference
/// addon clears its surface to blue and draws yellow, so a captured frame
/// that comes back this shade means the blit did not happen -- which is
/// exactly the mutation test/ui-present.js makes. A black or grey that some
/// other stage might also produce would make that failure ambiguous.
constexpr float kPresenterClearLevel = 32.0f / 255.0f;

} // namespace node_compat
} // namespace hermes

#endif // HERMES_NODE_COMPAT_COMPOSITOR_GPU_PRESENT_CONSTANTS_H
