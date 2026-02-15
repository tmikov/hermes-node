/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/// Standalone definition of hermes_napi_event_loop.
///
/// This must match the struct defined in hermes/API/napi/hermes_napi.h
/// (lines 269-300). It is extracted here so that code which only needs
/// the event loop interface can avoid including all of Hermes VM internals.

#ifndef HERMES_NODE_COMPAT_HERMES_NAPI_EVENT_LOOP_H
#define HERMES_NODE_COMPAT_HERMES_NAPI_EVENT_LOOP_H

#include <js_native_api_types.h>

struct hermes_napi_event_loop {
  void (*post_work)(
      void *loop_data,
      void *work_data,
      void (*execute)(void *work_data),
      void (*complete)(void *work_data, napi_status status));

  bool (*cancel_work)(void *loop_data, void *work_data);

  void (*post_task)(
      void *loop_data,
      void *task_data,
      void (*callback)(void *task_data));

  void *data;
};

#endif // HERMES_NODE_COMPAT_HERMES_NAPI_EVENT_LOOP_H
