// Minimal cluster shim for standalone CLI (no worker support).
// The real cluster module requires child_process -> dgram -> udp_wrap,
// process_wrap, spawn_sync -- none of which are implemented yet.
// For standalone CLI, isPrimary = true is all net.js needs.
'use strict';

module.exports = {
  isPrimary: true,
  isWorker: false,
  isMaster: true,  // deprecated alias
  _getServer: function() {
    throw new Error('cluster._getServer is not supported in standalone mode');
  },
};
