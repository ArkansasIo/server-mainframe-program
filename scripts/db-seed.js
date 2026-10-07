#!/usr/bin/env node
'use strict';

/**
 * db-seed - re-apply the demonstration data in sql/seed.sql.
 *
 * Usage:
 *   node scripts/db-seed.js
 *
 * The seed file is idempotent (every statement is INSERT OR IGNORE on a
 * unique key), so this is safe to run against a populated database.
 */

const fs = require('fs');
const path = require('path');

const { loadConfig } = require('../src/config');
const { openDatabase } = require('../src/db/connection');
const logger = require('../src/core/logger');

const ROOT = path.resolve(__dirname, '..');

function main() {
  const config = loadConfig();
  const file = path.join(ROOT, 'sql', 'seed.sql');

  if (!fs.existsSync(file)) throw new Error(`Missing SQL file: ${file}`);

  logger.info(`[db:seed] seeding ${config.database.file}`);
  const { db, close } = openDatabase(config, logger);

  try {
    db.exec(fs.readFileSync(file, 'utf8'));
    const users = db.prepare('SELECT COUNT(*) AS n FROM users').get().n;
    const datasets = db.prepare('SELECT COUNT(*) AS n FROM datasets').get().n;
    logger.info(`[db:seed] done - users=${users} datasets=${datasets}`);
  } finally {
    close();
  }
}

if (require.main === module) {
  try {
    main();
  } catch (err) {
    process.stderr.write(`${err.message}\n`);
    process.exit(1);
  }
}
