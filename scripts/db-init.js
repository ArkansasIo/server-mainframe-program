#!/usr/bin/env node
'use strict';

/**
 * db-init - build the SQLite database from the SQL files in sql/.
 *
 * Usage:
 *   node scripts/db-init.js                 # create ./data/mainframe.db
 *   node scripts/db-init.js --force         # delete an existing file first
 *   node scripts/db-init.js --db path.db    # explicit database file
 *   node scripts/db-init.js --config c.json # explicit config file
 *
 * The script applies the revisions in the order the migrator documents
 * (see sql/migrations.sql). Every file is written to be idempotent, so
 * running this twice on an existing database is safe.
 */

const fs = require('fs');
const path = require('path');

const { loadConfig } = require('../src/config');
const { openDatabase } = require('../src/db/connection');
const logger = require('../src/core/logger');

const ROOT = path.resolve(__dirname, '..');

/**
 * The revision list. Keep in step with sql/migrations.sql and with the
 * migrator in src/db/migrations.cpp.
 */
const REVISIONS = [
  { id: '001_schema', file: 'sql/schema.sql', description: 'tables and core indexes' },
  { id: '002_seed', file: 'sql/seed.sql', description: 'demonstration data' },
  { id: '003_views', file: 'sql/views.sql', description: 'reporting views' },
  { id: '004_indexes', file: 'sql/indexes.sql', description: 'performance indexes' },
  { id: '005_triggers', file: 'sql/triggers.sql', description: 'audit and integrity triggers' },
  { id: '006_views_ops', file: 'sql/views_ops.sql', description: 'operational reporting views' },
];

function parseArgs(argv) {
  const opts = { force: false, db: null, configPath: null, quiet: false };
  for (let i = 0; i < argv.length; i += 1) {
    switch (argv[i]) {
      case '--force': opts.force = true; break;
      case '--quiet': opts.quiet = true; break;
      case '--db': opts.db = argv[++i]; break;
      case '--config': opts.configPath = argv[++i]; break;
      default: break;
    }
  }
  return opts;
}

function readSql(relative) {
  const file = path.join(ROOT, relative);
  if (!fs.existsSync(file)) {
    throw new Error(`SQL file missing: ${file}`);
  }
  return fs.readFileSync(file, 'utf8');
}

/**
 * Record a revision in schema_migrations. The table is created by
 * sql/migrations.sql, but that file is not itself a revision, so make
 * sure it exists before the first row is written.
 *
 * Returns a status label for the log line:
 *   'applied' - the revision was not recorded before and is now
 *   'already' - the revision was already recorded (SQL still re-ran,
 *               which is safe because every file is idempotent)
 */
function recordRevision(db, revision) {
  db.exec(`CREATE TABLE IF NOT EXISTS schema_migrations (
      revision    TEXT PRIMARY KEY,
      applied_at  TEXT NOT NULL DEFAULT (datetime('now')),
      description TEXT NOT NULL DEFAULT ''
  )`);

  const existing = db
    .prepare('SELECT revision FROM schema_migrations WHERE revision = ?')
    .get(revision.id);

  if (existing) return 'already';

  db.prepare(
    'INSERT INTO schema_migrations (revision, description) VALUES (?, ?)',
  ).run(revision.id, revision.description);
  return 'applied';
}

function main() {
  const opts = parseArgs(process.argv.slice(2));
  const config = loadConfig({ configPath: opts.configPath });

  if (opts.db) config.database.file = path.resolve(opts.db);

  if (config.database.driver === 'memory') {
    throw new Error(
      'db:init needs a file-backed database. Set database.driver to "sqlite".',
    );
  }

  const file = config.database.file;

  if (opts.force && fs.existsSync(file)) {
    for (const suffix of ['', '-wal', '-shm']) {
      const target = `${file}${suffix}`;
      if (fs.existsSync(target)) fs.rmSync(target);
    }
    logger.info(`[db:init] removed existing database ${file}`);
  }

  logger.info(`[db:init] building ${file}`);
  const { db, close } = openDatabase(config, logger);

  let total = 0;
  try {
    // The migration-tracking table and its baseline rows come first so the
    // revisions applied below can be recorded against a known schema.
    db.exec(readSql('sql/migrations.sql'));

    for (const revision of REVISIONS) {
      const sql = readSql(revision.file);
      db.exec(sql);
      const state = recordRevision(db, revision);
      total += 1;
      if (!opts.quiet) {
        logger.info(`[db:init] ${revision.id} ${state} - ${revision.description}`);
      }
    }

    // Foreign keys are enforced at connect time; verify nothing slipped
    // through while the schema was being built.
    const violations = db.prepare('PRAGMA foreign_key_check').all();

    const counts = {
      systems: db.prepare('SELECT COUNT(*) AS n FROM systems').get().n,
      users: db.prepare('SELECT COUNT(*) AS n FROM users').get().n,
      datasets: db.prepare('SELECT COUNT(*) AS n FROM datasets').get().n,
      records: db.prepare('SELECT COUNT(*) AS n FROM dataset_records').get().n,
      jobs: db.prepare('SELECT COUNT(*) AS n FROM jobs').get().n,
      transactions: db.prepare('SELECT COUNT(*) AS n FROM transactions').get().n,
      volumes: db.prepare('SELECT COUNT(*) AS n FROM volumes').get().n,
      audit: db.prepare('SELECT COUNT(*) AS n FROM audit_log').get().n,
    };

    logger.info(`[db:init] applied ${total} revisions`);
    logger.info(
      `[db:init] rows: ${Object.entries(counts).map(([k, v]) => `${k}=${v}`).join(' ')}`,
    );

    if (violations.length) {
      logger.warn(`[db:init] ${violations.length} foreign-key violation(s) found`);
      process.exitCode = 1;
    } else {
      logger.info('[db:init] foreign keys OK');
    }

    logger.info(`[db:init] done -> ${file}`);
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

module.exports = { REVISIONS, parseArgs };
