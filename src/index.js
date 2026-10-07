#!/usr/bin/env node
'use strict';

/**
 * Mainframe Server System - entrypoint.
 *
 * This file is the composition root: it resolves configuration, opens the
 * database, and starts whichever services the configuration enables. The
 * services themselves live in their own modules -
 *
 *   src/http/server.js   the REST control plane and the dashboard page
 *   src/tcp/terminal.js  the TN3270-style terminal gateway
 *   src/tcp/data.js      the dataset transfer port
 *
 * - so this file stays readable as a description of *what the system is*
 * rather than an implementation of it.
 *
 * Usage:
 *   npm start
 *   node src/index.js --tcp-only
 *   node src/index.js --port 9090 --log-level debug
 *
 * Named flags (--port, --db, --log-level, ...) are parsed by the config
 * loader; see src/config/index.js.
 */

const fs = require('fs');
const path = require('path');

const { loadConfig } = require('./config');
const { openDatabase } = require('./db/connection');
const { Logger } = require('./core/logger');

const { startHttpServer } = require('./http/server');
const { startTerminalGateway } = require('./tcp/terminal');
const { startDataPort } = require('./tcp/data');

const ROOT = path.resolve(__dirname, '..');

/** The banner written to stdout before any service starts. */
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

/** Count rows in a table, tolerating one that does not exist yet. */
function safeCount(db, table) {
  try {
    return db.prepare(`SELECT COUNT(*) AS n FROM ${table}`).get().n;
  } catch {
    return 0;
  }
}

/**
 * Start every service the configuration enables.
 *
 * @returns {{servers: object[], started: string[]}}
 */
function startServices(config, db, logger) {
  const servers = [];
  const started = [];

  if (config.http.enabled) {
    servers.push(startHttpServer(config, db, logger));
    started.push(`http://${config.http.host}:${config.http.port}`);
  }

  if (config.tcp.enabled) {
    servers.push(startTerminalGateway(config, db, logger));
    started.push(`terminal ${config.tcp.host}:${config.tcp.terminalPort}`);

    servers.push(startDataPort(config, db, logger));
    started.push(`transfer ${config.tcp.host}:${config.tcp.dataPort}`);
  }

  if (!servers.length) {
    logger.warn('[boot] no services enabled (http and tcp are both off)');
  }

  return { servers, started };
}

async function main() {
  const config = loadConfig();
  const logger = Logger.fromConfig(config);

  process.stdout.write(`${banner(config)}\n`);
  logger.info(`[boot] config: ${config.__configFile || '(defaults only)'}`);

  const database = initDatabase(config, logger);
  const { servers, started } = startServices(config, database.db, logger);

  const shutdown = async (signal) => {
    logger.info(`[boot] ${signal} received - shutting down`);
    for (const server of servers) server.close();
    database.close();
    await logger.close();
    process.exit(0);
  };

  process.on('SIGINT', () => shutdown('SIGINT'));
  process.on('SIGTERM', () => shutdown('SIGTERM'));

  logger.info(`[boot] ready (${started.join(', ')})`);
  return { config, database, servers, started };
}

if (require.main === module) {
  main().catch((err) => {
    process.stderr.write(`Fatal: ${err.message}\n`);
    process.exit(1);
  });
}

const httpServer = require('./http/server');
const terminal = require('./tcp/terminal');

module.exports = {
  main,
  initDatabase,
  startServices,
  banner,
  safeCount,
  // Re-exported so existing callers and tests keep working after the split.
  renderDashboard: httpServer.renderDashboard,
  dashboardMenu: httpServer.dashboardMenu,
  createRoutes: httpServer.createRoutes,
  allowedTables: httpServer.allowedTables,
  handleTerminalLine: terminal.handleLine,
};
