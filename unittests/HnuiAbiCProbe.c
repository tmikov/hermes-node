/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/*
 * Compiled as C (-std=c99 -pedantic, see unittests/CMakeLists.txt),
 * deliberately mixed into HnuiAbiTest's sources. hnui_abi.h claims to be
 * plain C -- nothing enforced that until this file existed: a `namespace`,
 * a default member initializer, or an unguarded `_Static_assert` added to
 * the header would break THIS translation unit's build while every
 * existing (C++-only) test kept passing, which is the "test that cannot
 * fail" shape this project keeps finding and removing.
 *
 * Beyond compiling at all, every function below reports a size or offset
 * hnui_abi.h defines. HnuiAbiTest.cpp's CAndCxxCompilationsAgreeOnLayout
 * compares each one against what the SAME header produces under C++ in the
 * same binary -- the actual cross-language ABI-agreement question, which
 * "does this header compile as C" alone does not answer: two languages can
 * both accept a header and still disagree about a struct's layout.
 */

#include <hermes/node-compat/ui/hnui.h>
#include <hermes/node-compat/ui/hnui_abi.h>

#include <stddef.h>

size_t hnuiAbiCProbeSizeofEvent(void) {
  return sizeof(hnui_event);
}

size_t hnuiAbiCProbeSizeofWindowState(void) {
  return sizeof(hnui_window_state);
}

size_t hnuiAbiCProbeSizeofTarget(void) {
  return sizeof(hnui_target);
}

size_t hnuiAbiCProbeSizeofEventTypeField(void) {
  return sizeof(((hnui_event *)0)->type);
}

size_t hnuiAbiCProbeOffsetofEventText(void) {
  return offsetof(hnui_event, text);
}

size_t hnuiAbiCProbeOffsetofEventPaths(void) {
  return offsetof(hnui_event, paths);
}

/*
 * hnui.h includes node_api.h and defines HNUI_CHECK_VERSION, a macro
 * that must be plain C99 as well: it is used from addons written in C.
 * This is the macro exactly as an addon's init function uses it; the test
 * supplies stand-ins for hnui_api_version(), hnui_sokol_commit() and
 * napi_throw_error() so that both outcomes can be driven and the message
 * read back.
 */
const char *hnuiAbiCProbeSokolCommit(void) {
  return HNUI_SOKOL_COMMIT;
}

int hnuiAbiCProbeAbiVersion(void) {
  return HNUI_ABI_VERSION;
}

void *hnuiAbiCProbeCheckVersion(napi_env env) {
  HNUI_CHECK_VERSION(env);
  return (void *)env;
}
