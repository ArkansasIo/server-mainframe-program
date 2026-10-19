'use strict';

const { RailwaySimulator } = require('../railway/simulator');
const simulator = new RailwaySimulator();

function readJson(req, maxBytes = 64 * 1024) {
  return new Promise((resolve, reject) => {
    let body = '';
    req.setEncoding('utf8');
    req.on('data', (chunk) => {
      body += chunk;
      if (Buffer.byteLength(body, 'utf8') > maxBytes) {
        reject(new Error('Request body too large'));
        req.destroy();
      }
    });
    req.on('end', () => {
      if (!body.trim()) return resolve({});
      try { return resolve(JSON.parse(body)); } catch { return reject(new Error('Invalid JSON body')); }
    });
    req.on('error', reject);
  });
}

function createRailwayRoutes({ sendJson, sendError }) {
  return [
    { method: 'GET', path: '/api/railway/status', description: 'RailwayControl simulator status and full state snapshot', handler: (req, res) => sendJson(res, 200, simulator.snapshot()) },
    { method: 'GET', path: '/api/railway/trains', description: 'Current simulated trains', handler: (req, res) => sendJson(res, 200, { trains: simulator.snapshot().trains }) },
    { method: 'GET', path: '/api/railway/layout', description: 'Track sections, points, signals and routes', handler: (req, res) => { const s = simulator.snapshot(); sendJson(res, 200, { sections: s.sections, points: s.points, signals: s.signals, routes: s.routes }); } },
    { method: 'GET', path: '/api/railway/events', description: 'Recent RailwayControl simulator event log', handler: (req, res, url) => { const limit = Math.min(Math.max(Number(url.searchParams.get('limit')) || 50, 1), 250); sendJson(res, 200, { events: simulator.snapshot().events.slice(0, limit) }); } },
    { method: 'GET', path: '/api/railway/alarms', description: 'Current simulator alarms', handler: (req, res) => sendJson(res, 200, { alarms: simulator.snapshot().alarms }) },
    { method: 'POST', path: '/api/railway/command', description: 'Execute a guarded RailwayControl simulator command', handler: async (req, res) => { try { const body = await readJson(req); const result = simulator.command(body.command, body.args || {}); return sendJson(res, result.ok ? 200 : 409, result); } catch (err) { return sendError(res, 400, err.message); } } },
  ];
}

module.exports = { createRailwayRoutes, simulator };
