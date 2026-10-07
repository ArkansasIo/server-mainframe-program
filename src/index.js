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

    if (url.pathname.startsWith('/api/')) {
      const table = url.pathname.slice('/api/'.length).replace(/[^a-z_]/gi, '');
      const allowed = (config.spreadsheet.tables || [])
        .map((t) => t.name.toLowerCase().replace(/s$/, ''))
        .concat(['users', 'datasets', 'jobs', 'transactions', 'audit_log']);
      if (allowed.includes(table)) {
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

/** A minimal operator dashboard, rendered from the live database. */
function dashboard(config, db) {
  const sections = (config.spreadsheet.tables || [])
    .map((t) => {
      let rows = [];
      try {
        rows = db.prepare(`${t.query} LIMIT 25`).all();
      } catch { /* ignore */ }
      if (!rows.length) return `<h2>${t.name}</h2><p>(no rows)</p>`;
      const columns = Object.keys(rows[0]);
      const head = columns.map((c) => `<th>${c}</th>`).join('');
      const body = rows.map((r) => `<tr>${columns.map((c) => `<td>${escapeHtml(r[c])}</td>`).join('')}</tr>`).join('');
      return `<h2>${t.name}</h2><table><thead><tr>${head}</tr></thead><tbody>${body}</tbody></table>`;
    })
    .join('\n');

  return `<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>${config.system.name} - Operator Dashboard</title>
<style>
  body { font-family: Consolas, monospace; background: #0b0f14; color: #d7e3ee; margin: 24px; }
  h1 { font-size: 18px; } h2 { font-size: 14px; color: #7fc6ff; margin-top: 24px; }
  table { border-collapse: collapse; font-size: 12px; }
  th, td { border: 1px solid #263345; padding: 3px 8px; text-align: left; }
  th { background: #16202e; }
  .meta { color: #7d8fa3; font-size: 12px; }
</style>
</head>
<body>
<h1>${config.system.name} - ${config.system.sysplex} / ${config.system.region}</h1>
<p class="meta">version ${config.system.version || '1.0.0'} &middot; ${new Date().toISOString()} &middot; <a href="/api/health">/api/health</a></p>
${sections}
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
