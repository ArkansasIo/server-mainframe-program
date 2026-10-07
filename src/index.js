#!/usr/bin/env node
'use strict';

/**
 * Mainframe Server System - entrypoint.
 *
 * Boots the pieces the configuration asks for:
 *   - the database (created / migrated / seeded on first run)
 *   - the HTTP control plane (operator dashboard + REST API)
 *   - the TN3270-style terminal gateway
 *
 * Usage:
 *   npm start
 *   node src/index.js --tcp-only
 *   node src/index.js --port 9090 --log-level debug
 *
 * Named flags (--port, --db, --log-level, ...) are parsed by the config
 * loader; see src/config/index.js.
 */

const http = require('http');
const net = require('net');
const fs = require('fs');
const path = require('path');

const { loadConfig } = require('./config');
const { openDatabase } = require('./db/connection');
const { Logger } = require('./core/logger');

const ROOT = path.resolve(__dirname, '..');

function banner(config) {
  return [
    '============================================================',
    `  ${config.system.name}  (${config.system.sysplex} / ${config.system.region})`,
    `  version ${config.system.version || '1.0.0'}`,
    '============================================================',
  ].join('\n');
}

/**
 * Ensure the database exists and the schema is present. When the file is new
 * (or empty) the SQL revisions are applied; otherwise the existing file is
 * opened as-is.
 */
function initDatabase(config, logger) {
  const file = config.database.file;

  if (config.database.driver === 'memory') {
    logger.warn('[db] in-memory driver - data will not persist');
    return openDatabase(config, logger);
  }

  const handle = openDatabase(config, logger);

  if (config.database.autoMigrate !== false) {
    const { applied } = require('./db/migrate').migrate(handle.db, logger);
    if (applied.length) logger.info(`[db] applied ${applied.length} revision(s)`);
  }

  if (config.database.autoSeed !== false && config.database.seedOnEmpty) {
    const empty = safeCount(handle.db, 'users') === 0;
    if (empty) {
      const seedFile = path.join(ROOT, 'sql', 'seed.sql');
      if (fs.existsSync(seedFile)) {
        logger.info('[db] empty database - loading seed data');
        handle.db.exec(fs.readFileSync(seedFile, 'utf8'));
      }
    }
  }

  return handle;
}

function startHttp(config, db, logger) {
  const server = http.createServer((req, res) => {
    const url = new URL(req.url, `http://${req.headers.host || 'localhost'}`);

    // Static dashboard assets (public/) are served before the API so a file
    // named like a route can never shadow one.
    if (serveStatic(req, res, url, logger)) return;

    if (url.pathname === '/health' || url.pathname === '/api/health') {
      const state = safeCount(db, 'users');
      return sendJson(res, 200, {
        status: 'ok',
        system: config.system.name,
        sysplex: config.system.sysplex,
        version: config.system.version || '1.0.0',
        database: { driver: config.database.driver, users: state },
        uptimeSeconds: Math.round(process.uptime()),
      });
    }

    // Row endpoint used by the dashboard grid: /api/rows?table=users&limit=200
    if (url.pathname === '/api/rows') {
      const requested = (url.searchParams.get('table') || '').toLowerCase();
      const allowed = allowedTables(config);
      if (!allowed.includes(requested)) {
        return sendJson(res, 404, { error: `Unknown resource: ${requested}` });
      }

      const limit = Math.min(Math.max(parseInt(url.searchParams.get('limit') || '500', 10) || 500, 1), 5000);
      try {
        const rows = db.prepare(`SELECT * FROM ${requested} LIMIT ?`).all(limit);
        return sendJson(res, 200, { table: requested, count: rows.length, rows });
      } catch (err) {
        return sendJson(res, 400, { error: err.message });
      }
    }

    // The menu tree and key map, so the client builds its chrome from the
    // same configuration the server knows about.
    if (url.pathname === '/api/menu') {
      return sendJson(res, 200, {
        system: config.system.name,
        menus: dashboardMenu(config),
      });
    }

    if (url.pathname.startsWith('/api/')) {
      const table = url.pathname.slice('/api/'.length).replace(/[^a-z_]/gi, '');
      if (allowedTables(config).includes(table)) {
        try {
          const rows = db.prepare(`SELECT * FROM ${table} LIMIT 500`).all();
          return sendJson(res, 200, { table, count: rows.length, rows });
        } catch (err) {
          return sendJson(res, 400, { error: err.message });
        }
      }
      return sendJson(res, 404, { error: `Unknown resource: ${table}` });
    }

    if (url.pathname === '/' || url.pathname === '/index.html') {
      return sendHtml(res, 200, dashboard(config, db));
    }

    sendJson(res, 404, { error: 'Not found' });
  });

  const { host, port } = config.http;
  server.listen(port, host, () => {
    logger.info(`[http] listening on http://${host}:${port}`);
  });
  server.on('error', (err) => {
    logger.error(`[http] ${err.message}`);
  });
  return server;
}

function startTcp(config, db, logger) {
  const { host, terminalPort } = config.tcp;
  const server = net.createServer((socket) => {
    logger.debug(`[tcp] terminal connected from ${socket.remoteAddress}`);
    socket.setEncoding('utf8');
    socket.write(`${config.tcp.banner}\r\n`);
    socket.write(`${config.system.name} / ${config.system.sysplex} - READY\r\n> `);

    let buffer = '';
    socket.on('data', (chunk) => {
      buffer += chunk;
      let index;
      while ((index = buffer.indexOf('\n')) >= 0) {
        const line = buffer.slice(0, index).trim();
        buffer = buffer.slice(index + 1);
        handleTerminalLine(socket, line, config, db, logger);
        socket.write('> ');
      }
    });
    socket.on('error', (err) => logger.debug(`[tcp] socket error: ${err.message}`));
    socket.on('close', () => logger.debug('[tcp] terminal disconnected'));
  });

  server.listen(terminalPort, host, () => {
    logger.info(`[tcp] terminal gateway listening on ${host}:${terminalPort}`);
  });
  server.on('error', (err) => logger.error(`[tcp] ${err.message}`));
  return server;
}

function handleTerminalLine(socket, line, config, db, logger) {
  const command = line.toUpperCase();

  if (!command) return;

  if (command === 'LOGOFF' || command === 'EXIT' || command === 'QUIT') {
    socket.write('GOODBYE\r\n');
    socket.end();
    return;
  }

  if (command === 'HELP') {
    socket.write('COMMANDS: STATUS | USERS | DATASETS | JOBS | LOGOFF\r\n');
    return;
  }

  if (command === 'STATUS') {
    socket.write(`SYSTEM   : ${config.system.name}\r\n`);
    socket.write(`SYSPLEX  : ${config.system.sysplex}\r\n`);
    socket.write(`USERS    : ${safeCount(db, 'users')}\r\n`);
    socket.write(`DATASETS : ${safeCount(db, 'datasets')}\r\n`);
    socket.write(`JOBS     : ${safeCount(db, 'jobs')}\r\n`);
    return;
  }

  if (['USERS', 'DATASETS', 'JOBS'].includes(command)) {
    const table = command.toLowerCase();
    try {
      const rows = db.prepare(`SELECT * FROM ${table} LIMIT 20`).all();
      for (const row of rows) {
        socket.write(`${Object.values(row).slice(0, 6).join(' | ')}\r\n`);
      }
    } catch (err) {
      socket.write(`ERROR: ${err.message}\r\n`);
    }
    return;
  }

  logger.debug(`[tcp] unknown command: ${line}`);
  socket.write('UNKNOWN COMMAND - TYPE HELP\r\n');
}

function safeCount(db, table) {
  try {
    return db.prepare(`SELECT COUNT(*) AS n FROM ${table}`).get().n;
  } catch {
    return 0;
  }
}

function sendJson(res, status, body) {
  const payload = JSON.stringify(body, null, 2);
  res.writeHead(status, {
    'content-type': 'application/json; charset=utf-8',
    'content-length': Buffer.byteLength(payload),
  });
  res.end(payload);
}

function sendHtml(res, status, body) {
  res.writeHead(status, { 'content-type': 'text/html; charset=utf-8' });
  res.end(body);
}

/**
 * Tables the API will expose.
 *
 * The dashboard's panels are declared by table name, so the names must be the
 * real ones. An earlier version stripped a trailing "s" from the configured
 * spreadsheet names as a singularisation heuristic, which produced "user" for
 * USERS and left AUDIT_LOG untouched - neither of which is a table, so every
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

/**
 * Serve a file from public/. Returns true when the request was handled.
 *
 * The path is resolved and then checked to still live inside public/, so a
 * request for /../src/index.js cannot read files outside the asset root.
 */
function serveStatic(req, res, url, logger) {
  const publicDir = path.join(ROOT, 'public');
  const requested = url.pathname === '/' ? '/index.html' : url.pathname;

  // The component library and keybind registry live with the source so they
  // can be unit tested in Node; expose them under their bare names.
  const aliases = {
    '/components.js': path.join(ROOT, 'src', 'ui', 'components.js'),
    '/keybinds.js': path.join(ROOT, 'src', 'ui', 'keybinds.js'),
    '/views.js': path.join(ROOT, 'src', 'ui', 'views.js'),
  };

  let target = aliases[requested];
  if (!target) {
    target = path.resolve(publicDir, `.${requested}`);
    // Refuse anything that climbed out of the asset root
    // (/../src/index.js must not be readable).
    if (!target.startsWith(publicDir + path.sep)) return false;
  }

  if (!fs.existsSync(target) || !fs.statSync(target).isFile()) return false;

  const types = {
    '.html': 'text/html; charset=utf-8',
    '.js': 'text/javascript; charset=utf-8',
    '.css': 'text/css; charset=utf-8',
    '.json': 'application/json; charset=utf-8',
    '.svg': 'image/svg+xml',
    '.ico': 'image/x-icon',
    '.png': 'image/png',
  };

  const body = fs.readFileSync(target);
  res.writeHead(200, {
    'content-type': types[path.extname(target).toLowerCase()] || 'application/octet-stream',
    'content-length': body.length,
    'cache-control': 'no-cache',
  });
  res.end(req.method === 'HEAD' ? undefined : body);
  if (logger) logger.debug(`[http] static ${requested}`);
  return true;
}

/**
 * The operator dashboard shell.
 *
 * The page is intentionally thin: it embeds the resolved system identity and
 * the panel list, then hands off to public/dashboard.js, which builds the menu
 * bar, submenus, toolbar buttons, tabs, grid and command palette. Rendering
 * data client-side is what makes the console interactive (sorting, filtering,
 * keyboard navigation) without a round trip per keystroke.
 */
function dashboard(config, db) {
  const panels = (config.spreadsheet.tables || []).map((t) => ({
    id: t.name.toLowerCase().replace(/s$/, ''),
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
    generatedAt: new Date().toISOString(),
  };

  return `<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>${escapeHtml(config.system.name)} - Operator Console</title>
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

function escapeHtml(value) {
  if (value === null || value === undefined) return '';
  return String(value)
    .replace(/&/g, '&amp;')
    .replace(/</g, '&lt;')
    .replace(/>/g, '&gt;');
}

async function main() {
  const config = loadConfig();
  const logger = Logger.fromConfig(config);

  process.stdout.write(`${banner(config)}\n`);
  logger.info(`[boot] config: ${config.__configFile || '(defaults only)'}`);

  const database = initDatabase(config, logger);

  const servers = [];
  if (config.http.enabled) servers.push(startHttp(config, database.db, logger));
  if (config.tcp.enabled) servers.push(startTcp(config, database.db, logger));
  if (!servers.length) logger.warn('[boot] no services enabled (http and tcp are both off)');

  const shutdown = async (signal) => {
    logger.info(`[boot] ${signal} received - shutting down`);
    for (const server of servers) server.close();
    database.close();
    await logger.close();
    process.exit(0);
  };

  process.on('SIGINT', () => shutdown('SIGINT'));
  process.on('SIGTERM', () => shutdown('SIGTERM'));

  logger.info('[boot] ready');
}

if (require.main === module) {
  main().catch((err) => {
    process.stderr.write(`Fatal: ${err.message}\n`);
    process.exit(1);
  });
}

module.exports = { main, dashboard, initDatabase };
