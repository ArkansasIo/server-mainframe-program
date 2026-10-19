'use strict';

const { RailwaySimulator } = require('./simulator');

/**
 * Railway subsystem composition root.
 * The simulator is intentionally isolated so it can later be replaced by a
 * native RailwayControl IPC adapter without changing the HTTP API contract.
 */
function createRailwaySystem(options = {}) {
  const simulator = options.simulator || new RailwaySimulator(options);
  return {
    simulator,
    status() { return simulator.snapshot(); },
    command(command, args) { return simulator.command(command, args); },
  };
}

module.exports = { createRailwaySystem };
