'use strict';

/**
 * The REST control plane.
 *
 * Routes are declared as data and matched against the request, rather than
 * being a chain of `if (url.pathname === ...)` tests. The chain worked while
 * there were four endpoints; it stops working the moment you want to reason
 * about the API as a whole - which endpoints exist, which are public, which
 * expose a table - because that answer is spread across the function body.
 *
 * A route is:
 *
 *   { method, path, handler, description }
 *
 * `path` is an exact match. `/api/rows` takes its table from the query string
 * rather than a path segment, so the table name never has to be decoded out of
 * a URL path.
 */

const path = require('path');

const { sendJson, sendError, escapeHtml } = require('./respond');
const { createStaticHandler } = require('./static');

/* --------------------------------------------------------------------------
 * Table access
 * -------------------------------------------------------------------------- */

/**
 * Tables the API will expose.
 *
 * The dashboard's panels are declared by table name, so the names must be the
 * real ones. An earlier version stripped a trailing "s" from the configured
 * spreadsheet names as a singularisation heuristic, which produced "user" for
 * USERS and left AUDIT_LOG alone - neither of which is a table, so every
 * request 404'd.
 */
function allowedTables(config) {
  const fromConfig = (config.spreadsheet.tables || [])
    .map((t) => String(t.name || '').toLowerCase());

  const operational = [
    'users', 'datasets', 'jobs', 'transactions', 'audit_log',
    'dataset_records', 'volumes', 'system_parameters',
  ];

  return fromConfig
    .concat(operational)
    .map((name) => name.trim())
    .filter((name) => /^[a-z][a-z0-9_]*$/.test(name))
    .filter((name, index, all) => all.indexOf(name) === index)
    .sort();
}

/** Count rows in a table, tolerating a table that does not exist yet. */
function safeCount(db, table) {
  try {
    return db.prepare(`SELECT COUNT(*) AS n FROM ${table}`).get().n;
  } catch {
    return 0;
  }
}

/** Read rows from a table, clamped to a sane page size. */
function readRows(db, table, limit) {
  const size = Math.min(Math.max(Number(limit) || 500, 1), 5000);
  return db.prepare(`SELECT * FROM ${table} LIMIT ?`).all(size);
}

/* --------------------------------------------------------------------------
 * The dashboard page
 * -------------------------------------------------------------------------- */

/**
 * The operator dashboard shell.
 *
 * The page is intentionally thin: it embeds the resolved system identity and
 * the panel list, then hands off to the client modules, which build the menu
 * bar, submenus, toolbar buttons, tabs, grid, window manager and command
 * palette. Rendering data client-side is what makes the console interactive
 * (sorting, filtering, keyboard navigation) without a round trip per keystroke.
 */
function renderDashboard(config) {
  const panels = (config.spreadsheet.tables || []).map((t) => ({
    id: t.name.toLowerCase(),
    // The panel's table is the configured name; its id is the lower-case form
    // used for lookups. Keeping both avoids the client having to guess.
    table: t.name.toLowerCase(),
    label: t.name,
  }));

  const bootstrap = {
    system: {
      name: config.system.name,
      sysplex: config.system.sysplex,
      region: config.system.region,
      version: config.system.version || '1.0.0',
    },
    panels,
    httpPort: config.http.port,
    terminalPort: config.tcp.terminalPort,
    dataPort: config.tcp.dataPort,
    generatedAt: new Date().toISOString(),
  };

  return `<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>${escapeHtml(config.system.name)} - Operator Console</title>
<link rel="icon" href="/favicon.svg" type="image/svg+xml">
<link rel="stylesheet" href="/dashboard.css">
</head>
<body>
<div id="app"></div>
<script>window.MF_BOOTSTRAP = ${JSON.stringify(bootstrap).replace(/</g, '\\u003c')};</script>
<script src="/components.js"></script>
<script src="/keybinds.js"></script>
<script src="/views.js"></script>
<script src="/dashboard.js"></script>
</body>
</html>`;
}

/**
 * The dashboard's menu tree, served so the client chrome is described by the
 * server rather than hard-coded twice.
 */
function dashboardMenu(config) {
  const panelHref = (id, label) => ({ id, label, action: `nav.${id}` });

  return [
    {
      id: 'file',
      label: 'File',
      items: [
        { id: 'refresh', label: 'Refresh All', action: 'app.refresh' },
        { label: '-' },
        { id: 'export', label: 'Export Panel', action: 'data.export' },
      ],
    },
    {
      id: 'view',
      label: 'View',
      items: [
        {
          id: 'panels',
          label: 'Go to Panel',
          children: [
            panelHref('status', 'Status'),
            panelHref('users', 'Users'),
            panelHref('datasets', 'Datasets'),
            panelHref('jobs', 'Jobs'),
          ],
        },
        { id: 'theme', label: 'Toggle Theme', action: 'app.toggleTheme' },
      ],
    },
    {
      id: 'help',
      label: 'Help',
      items: [
        { id: 'keys', label: 'Keyboard Shortcuts', action: 'app.help' },
        { id: 'about', label: 'About', action: 'help.about' },
      ],
    },
  ].map((menu) => ({ ...menu, title: menu.label, system: config.system.name }));
}

/* --------------------------------------------------------------------------
 * Route table
 * -------------------------------------------------------------------------- */

/**
 * Build the route table for a resolved configuration.
 *
 * The handlers close over `{ config, db }`, so a route cannot reach for state
 * it was not given.
 *
 * @param {object} ctx
 * @param {object} ctx.config
 * @param {object} ctx.db
 * @returns {object[]}
 */
function createRoutes(ctx) {
  const { config, db } = ctx;

  return [
    {
      method: 'GET',
      path: '/api/health',
      description: 'Liveness, identity and database state',
      handler: (req, res) => {
        sendJson(res, 200, {
          status: 'ok',
          system: config.system.name,
          sysplex: config.system.sysplex,
          region: config.system.region,
          version: config.system.version || '1.0.0',
          database: { driver: config.database.driver, users: safeCount(db, 'users') },
          uptimeSeconds: Math.round(process.uptime()),
        });
      },
    },

    {
      method: 'GET',
      path: '/health',
      // The bare alias exists because health checks are frequently pointed at
      // /health by infrastructure that has no idea about the /api prefix.
      description: 'Alias of /api/health',
      handler: (req, res) => {
        sendJson(res, 200, {
          status: 'ok',
          system: config.system.name,
          version: config.system.version || '1.0.0',
          uptimeSeconds: Math.round(process.uptime()),
        });
      },
    },

    {
      method: 'GET',
      path: '/api/rows',
      description: 'Rows from one table (?table=&limit=)',
      handler: (req, res, url) => {
        const requested = (url.searchParams.get('table') || '').toLowerCase();
        if (!allowedTables(config).includes(requested)) {
          return sendError(res, 404, `Unknown resource: ${requested}`);
        }

        try {
          const rows = readRows(db, requested, url.searchParams.get('limit'));
          return sendJson(res, 200, { table: requested, count: rows.length, rows });
        } catch (err) {
          return sendError(res, 400, err.message);
        }
      },
    },

    {
      method: 'GET',
      path: '/api/tables',
      description: 'Every table the API exposes',
      handler: (req, res) => {
        sendJson(res, 200, { tables: allowedTables(config) });
      },
    },

    {
      method: 'GET',
      path: '/api/menu',
      description: 'The dashboard menu tree',
      handler: (req, res) => {
        sendJson(res, 200, {
          system: config.system.name,
          menus: dashboardMenu(config),
        });
      },
    },

    {
      method: 'GET',
      path: '/',
      description: 'Operator dashboard',
      handler: (req, res) => {
        res.writeHead(200, { 'content-type': 'text/html; charset=utf-8' });
        res.end(renderDashboard(config));
      },
    },

    {
      method: 'GET',
      path: '/index.html',
      description: 'Alias of /',
      handler: (req, res) => {
        res.writeHead(200, { 'content-type': 'text/html; charset=utf-8' });
        res.end(renderDashboard(config));
      },
    },
  ];
}

/* --------------------------------------------------------------------------
 * The server
 * -------------------------------------------------------------------------- */

/**
 * Start the HTTP control plane.
 *
 * @param {object} config
 * @param {object} db
 * @param {object} logger
 * @param {object} [deps]
 * @param {Function} [deps.createServer] injectable for tests
 * @returns {object} the node http.Server
 */
function startHttpServer(config, db, logger, deps = {}) {
  const http = deps.http || require('http');
  const routes = createRoutes({ config, db });
  const serveStatic = createStaticHandler({
    root: path.resolve(__dirname, '..', '..'),
    logger,
  });

  // A Map would be tidier, but a linear scan over a handful of routes keeps
  // the declared order visible, which is what the two aliases depend on.
  const server = http.createServer((req, res) => {
    let url;
    try {
      url = new URL(req.url, `http://${req.headers.host || 'localhost'}`);
    } catch {
      return sendError(res, 400, 'Malformed request URL');
    }

    // Static assets first, so a file can never be shadowed by a route.
    if (serveStatic(req, res, url)) return;

    const route = routes.find(
      (r) => r.path === url.pathname && (r.method === req.method || req.method === 'HEAD'),
    );
    if (route) {
      try {
        return route.handler(req, res, url);
      } catch (err) {
        logger.error(`[http] ${url.pathname} failed: ${err.message}`);
        return sendError(res, 500, 'Internal error');
      }
    }

    // A genuinely unknown path. /api/* is always JSON so a client can parse
    // the failure without guessing at the content type.
    if (url.pathname.startsWith('/api/')) {
      return sendError(res, 404, `No such endpoint: ${url.pathname}`);
    }
    return sendError(res, 404, 'Not found');
  });

  const { host, port } = config.http;
  server.listen(port, host, () => {
    logger.info(`[http] listening on http://${host}:${port}`);
  });
  server.on('error', (err) => {
    logger.error(`[http] ${err.message}`);
  });

  server.routes = routes;
  return server;
}

/**
 * Write an OpenAPI-ish listing of the routes, for docs and for the API test.
 * @returns {Array<{method:string, path:string, description:string}>}
 */
function describeRoutes(routes) {
  return routes.map((r) => ({
    method: r.method,
    path: r.path,
    description: r.description || '',
  }));
}

module.exports = {
  startHttpServer,
  createRoutes,
  renderDashboard,
  dashboardMenu,
  allowedTables,
  describeRoutes,
  safeCount,
  readRows,
};
