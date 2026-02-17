// Copyright (c) Tzvetan Mikov.
// Stub for internal/process/signal.js.
// Signal handling via signal_wrap is not implemented.
// Provide no-op functions so is_main_thread.js can load.
'use strict';

module.exports = {
  startListeningIfSignal() {},
  stopListeningIfSignal() {},
};
