/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_NODE_COMPAT_COMPOSITOR_GPU_SOKOL_CONFIG_H
#define HERMES_NODE_COMPAT_COMPOSITOR_GPU_SOKOL_CONFIG_H

/// Backend selection and build-wide Sokol settings, in one place so the two
/// `sokol_gfx` translation units cannot be configured differently from each
/// other.
///
/// That is not a hypothetical tidiness concern: the two instances hand
/// `sg_image` and `sg_view` values back and forth, and several of Sokol's
/// public structs change shape with the backend define. Two copies compiled
/// under different backends would disagree about those layouts while linking
/// perfectly.
///
/// The backend itself is chosen by CMake (`SOKOL_GLCORE` on Linux), not
/// here, because it is also needed by `sokol_app`'s translation unit in
/// `tools/hermes-node-ui/` and a header in `lib/` cannot reach that. This
/// file only asserts that CMake did it -- a missing define would otherwise
/// fall into Sokol's own "no backend selected" `#error`, which names neither
/// this project nor the CMake variable to fix.

#if !defined(SOKOL_GLCORE) && !defined(SOKOL_METAL)
#error \
    "No Sokol backend selected. lib/compositor-gpu/CMakeLists.txt sets SOKOL_GLCORE on Linux; macOS Metal arrives with Task 16. v1 refuses EGL and GLES3 (spec: Global Constraints)."
#endif

/// The Metal backend's Sokol units must be compiled as Objective-C++:
/// sokol_gfx.h's Metal implementation is Objective-C, and the accessors in
/// sokol_gfx_ext.inc bridge an `id<MTLCommandBuffer>`. Compiled as plain
/// C++ the failure would be a cascade of Objective-C syntax errors from
/// inside a 20,000-line header; this names the fix instead. **And with
/// ARC**: Sokol's texture injection releases the texture it was handed
/// under manual reference counting, out from under its owner.
#if defined(SOKOL_METAL) && !defined(__OBJC__)
#error \
    "SOKOL_METAL requires Objective-C++: compile this file with the CMake source property LANGUAGE OBJCXX (set_source_files_properties(... PROPERTIES LANGUAGE OBJCXX)) and -fobjc-arc, as lib/compositor-gpu/CMakeLists.txt does for its Metal sources."
#endif
#if defined(SOKOL_METAL) && defined(__OBJC__) && !__has_feature(objc_arc)
#error \
    "SOKOL_METAL requires ARC: compile this file with -fobjc-arc. Without it Sokol's _SG_OBJC_RELEASE is a real release, and an injected texture is released out from under its owner."
#endif

/// Sokol's assert hook. Left as the default (`assert`) deliberately, and
/// recorded here because the alternative is tempting and wrong: Sokol's
/// asserts fire on genuine contract violations -- an exhausted resource
/// pool, a texture view over mutable storage -- and the design's answer to
/// those is to not commit them, not to survive them. Routing them to
/// something that returns would turn a stated contract into a silent
/// corruption. See the design's "Resource churn needs admission control".
///
/// One consequence worth knowing: `NDEBUG` compiles Sokol's asserts out, so
/// a Release build does not get these checks. Development happens in
/// `cmake-build-asan` (Debug), where it does.

#endif // HERMES_NODE_COMPAT_COMPOSITOR_GPU_SOKOL_CONFIG_H
