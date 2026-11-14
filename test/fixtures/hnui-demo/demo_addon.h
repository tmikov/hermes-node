/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/**
 * What the reference addon's source files share: one function per file
 * that adds that file's functions to the module's exports. The module init
 * itself, with the version check that must run before anything else, is in
 * demo_addon.c.
 *
 * Three files, by job: demo_visual.c is the demo proper and the worked
 * example for hnui.h; demo_conformance.c is the conformance mode; and
 * demo_addon.c holds the fixed pattern the capture tests draw plus the probes
 * the other UI tests have accumulated. Kept apart so that the file a person
 * copies from does not have to be read around a thousand lines of test
 * helpers.
 */

#ifndef HNUI_DEMO_ADDON_H
#define HNUI_DEMO_ADDON_H

#include <node_api.h>

void demoDefineVisualExports(napi_env env, napi_value exports);
void demoDefineConformanceExports(napi_env env, napi_value exports);

#endif /* HNUI_DEMO_ADDON_H */
