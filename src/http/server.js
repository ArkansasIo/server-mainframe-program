'use strict';

const path = require('path');
const { sendJson, sendError, escapeHtml } = require('./respond');
const { createStaticHandler } = require('./static');
const { createAuthPolicy, createAuthGuard } = require('./auth');
const { createRailwayRoutes } = require('./railway');
const { createRailwayCommandRoutes } = require('./railway-commands');

function allowedTables(config) {
  const fromConfig = (config.spreadsheet.tables || []).map((t) => String(t.name || '').toLowerCase());
  const operational = ['users', 'datasets', 'jobs', 'transactions', 'audit_log', 'dataset_records', 'volumes', 'system_parameters'];
  return fromConfig.concat(operational).map((name) => name.trim()).filter((name) => /^[a-z][a-z0-9_]*$/.test(name)).filter((name, index, all) => all.indexOf(name) === index).sort();
}
function safeCount(db, table) { try { return db.prepare(`SELECT COUNT(*) AS n FROM ${table}`).get().n; } catch { return 0; } }
function readRows(db, table, limit) { const size = Math.min(Math.max(Number(limit) || 500, 1), 5000); return db.prepare(`SELECT * FROM ${table} LIMIT ?`).all(size); }

function renderDashboard(config) {
  const panels = (config.spreadsheet.tables || []).map((t) => ({ id: t.name.toLowerCase(), table: t.name.toLowerCase(), label: t.name }));
  const bootstrap = { system: { name: config.system.name, sysplex: config.system.sysplex, region: config.system.region, version: config.system.version || '1.0.0' }, panels, httpPort: config.http.port, terminalPort: config.tcp.terminalPort, dataPort: config.tcp.dataPort, generatedAt: new Date().toISOString() };
  return `<!doctype html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1"><title>${escapeHtml(config.system.name)} - Operator Console</title><link rel="icon" href="/favicon.svg" type="image/svg+xml"><link rel="stylesheet" href="/dashboard.css"></head><body><div id="app"></div><script>window.MF_BOOTSTRAP = ${JSON.stringify(bootstrap).replace(/</g, '\\u003c')};</script><script src="/components.js"></script><script src="/keybinds.js"></script><script src="/views.js"></script><script src="/dashboard.js"></script><script src="/railway-api.js"></script><script src="/railway-control.js"></script><script src="/menu-features.js"></script></body></html>`;
}

function dashboardMenu(config) {
  const panelHref = (id, label) => ({ id, label, action: `nav.${id}` });
  return [
    { id: 'file', label: 'File', items: [{ id: 'refresh', label: 'Refresh All', action: 'app.refresh' }, { label: '-' }, { id: 'export', label: 'Export Panel', action: 'data.export' }] },
    { id: 'view', label: 'View', items: [{ id: 'panels', label: 'Go to Panel', children: [panelHref('status', 'Status'), panelHref('users', 'Users'), panelHref('datasets', 'Datasets'), panelHref('jobs', 'Jobs'), { id: 'railway', label: 'Railway / Trains', action: 'railway.open' }] }, { id: 'theme', label: 'Toggle Theme', action: 'app.toggleTheme' }] },
    { id: 'help', label: 'Help', items: [{ id: 'keys', label: 'Keyboard Shortcuts', action: 'app.help' }, { id: 'about', label: 'About', action: 'help.about' }] },
  ].map((menu) => ({ ...menu, title: menu.label, system: config.system.name }));
}

function createRoutes(ctx) {
  const { config, db } = ctx;
  const routes = [
    { method: 'GET', path: '/api/health', description: 'Liveness, identity and database state', handler: (req, res) => sendJson(res, 200, { status: 'ok', system: config.system.name, sysplex: config.system.sysplex, region: config.system.region, version: config.system.version || '1.0.0', database: { driver: config.database.driver, users: safeCount(db, 'users') }, uptimeSeconds: Math.round(process.uptime()) }) },
    { method: 'GET', path: '/health', description: 'Alias of /api/health', handler: (req, res) => sendJson(res, 200, { status: 'ok', system: config.system.name, version: config.system.version || '1.0.0', uptimeSeconds: Math.round(process.uptime()) }) },
    { method: 'GET', path: '/api/rows', description: 'Rows from one table (?table=&limit=)', handler: (req, res, url) => { const requested = (url.searchParams.get('table') || '').toLowerCase(); if (!allowedTables(config).includes(requested)) return sendError(res, 404, `Unknown resource: ${requested}`); try { const rows = readRows(db, requested, url.searchParams.get('limit')); return sendJson(res, 200, { table: requested, count: rows.length, rows }); } catch (err) { return sendError(res, 400, err.message); } } },
    { method: 'GET', path: '/api/tables', description: 'Every table the API exposes', handler: (req, res) => sendJson(res, 200, { tables: allowedTables(config) }) },
    { method: 'GET', path: '/api/menu', description: 'The dashboard menu tree', handler: (req, res) => sendJson(res, 200, { system: config.system.name, menus: dashboardMenu(config) }) },
    { method: 'GET', path: '/', description: 'Operator dashboard', handler: (req, res) => { res.writeHead(200, { 'content-type': 'text/html; charset=utf-8' }); res.end(renderDashboard(config)); } },
    { method: 'GET', path: '/index.html', description: 'Alias of /', handler: (req, res) => { res.writeHead(200, { 'content-type': 'text/html; charset=utf-8' }); res.end(renderDashboard(config)); } },
  ];
  return routes.concat(createRailwayRoutes({ sendJson, sendError },), createRailwayCommandRoutes({ sendJson }));
}

function startHttpServer(config, db, logger, deps = {}) {
  const http = deps.http || require('http');
  const routes = createRoutes({ config, db });
  const serveStatic = createStaticHandler({ root: path.resolve(__dirname, '..', '..'), logger });
  const policy = createAuthPolicy(config);
  const guard = createAuthGuard({ policy, onDenied: (req, reason) => logger.warn(`[http] refused ${req.method} ${req.url} from ${req.socket.remoteAddress}: ${reason}`) });
  if (policy.required) logger.info(`[http] API key auth ENABLED (${policy.keyCount} key(s), protecting /api/*)`);

  const server = http.createServer((req, res) => {
    let url;
    try { url = new URL(req.url, `http://${req.headers.host || 'localhost'}`); } catch { return sendError(res, 400, 'Malformed request URL'); }
    if (serveStatic(req, res, url)) return;
    if (guard(req, res, url)) { res.setHeader('www-authenticate', 'Bearer realm="mainframe"'); return sendError(res, 401, 'Unauthorized'); }
    const route = routes.find((r) => r.path === url.pathname && (r.method === req.method || req.method === 'HEAD'));
    if (route) {
      Promise.resolve().then(() => route.handler(req, res, url)).catch((err) => {
        logger.error(`[http] ${url.pathname} failed: ${err.message}`);
        if (!res.headersSent) sendError(res, 500, 'Internal error'); else res.destroy();
      });
      return;
    }
    if (url.pathname.startsWith('/api/')) return sendError(res, 404, `No such endpoint: ${url.pathname}`);
    return sendError(res, 404, 'Not found');
  });
  const { host, port } = config.http;
  server.listen(port, host, () => logger.info(`[http] listening on http://${host}:${port}`));
  server.on('error', (err) => logger.error(`[http] ${err.message}`));
  server.routes = routes;
  server.auth = policy;
  return server;
}

function describeRoutes(routes) { return routes.map((r) => ({ method: r.method, path: r.path, description: r.description || '' })); }
module.exports = { startHttpServer, createRoutes, renderDashboard, dashboardMenu, allowedTables, describeRoutes, safeCount, readRows };
