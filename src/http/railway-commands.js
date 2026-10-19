'use strict';

const { listCommands } = require('../railway/commands');

function createRailwayCommandRoutes({ sendJson }) {
  return [
    {
      method: 'GET',
      path: '/api/railway/commands',
      description: 'List supported RailwayControl simulator commands',
      handler: (req, res) => sendJson(res, 200, { commands: listCommands() }),
    },
  ];
}

module.exports = { createRailwayCommandRoutes };
