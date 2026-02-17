'use strict';

// Stub for internal/perf/observe.
// dns.js imports hasObserver/startPerf/stopPerf for performance observation.
// Since we don't have internalBinding('performance'), provide no-ops.

module.exports = {
  PerformanceObserver: undefined,
  PerformanceObserverEntryList: undefined,
  enqueue() {},
  hasObserver() { return false; },
  clearEntriesFromBuffer() {},
  filterBufferMapByNameAndType() {},
  startPerf() {},
  stopPerf() {},
  bufferUserTiming() {},
  bufferResourceTiming() {},
  setResourceTimingBufferSize() {},
  setDispatchBufferFull() {},
};
