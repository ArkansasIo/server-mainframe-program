'use strict';

const COMMANDS = Object.freeze({
  STATUS: { description: 'Return complete simulator state' },
  LIST_TRAINS: { description: 'List simulated trains' },
  DISPATCH: { description: 'Set a guarded route for a train' },
  RUN: { description: 'Run a train at a bounded simulated speed' },
  HOLD: { description: 'Hold a train at zero speed' },
  ESTOP: { description: 'Activate simulation emergency stop' },
  RESET_ESTOP: { description: 'Reset simulation emergency stop' },
  CLEAR_ALARMS: { description: 'Clear simulation alarms' },
});

function listCommands() {
  return Object.entries(COMMANDS).map(([command, meta]) => ({ command, ...meta }));
}

function isKnownCommand(command) {
  return Object.prototype.hasOwnProperty.call(COMMANDS, String(command || '').trim().toUpperCase());
}

module.exports = { COMMANDS, listCommands, isKnownCommand };
