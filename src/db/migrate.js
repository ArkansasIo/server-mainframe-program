'use strict';

/**
 * Database migrator.
 *
 * Applies the SQL revisions in order and records each one in
 * schema_migrations. Shared by src/index.js (first boot) and
 * scripts/db-init.js. The revision list mirrors sql/migrations.sql and the
 * C++ migrator in src/db/migrations.cpp.
 */

const fs = require('fs');
const path = require('path');

const ROOT = path.resolve(__dirname, '..', '..');

const REVISIONS = [
  { id: '001_schema', file: 'sql/schema.sql', description: 'tables and core indexes' },
  { id: '002_seed', file: 'sql/seed.sql', description: 'demonstration data' },
  { id: '003_views', file: 'sql/views.sql', description: 'reporting views' },
  { id: '004_indexes', file: 'sql/indexes.sql', description: 'performance indexes' },
  { id: '005_triggers', file: 'sql/triggers.sql', description: 'audit and integrity triggers' },
  { id: '006_views_ops', file: 'sql/views_ops.sql', description: 'operational reporting views' },
];

function ensureMigrationsTable(db) {
  db.exec(`CREATE TABLE IF NOT EXISTS schema_migrations (
      revision    TEXT PRIMARY KEY,
      applied_at  TEXT NOT NULL DEFAULT (datetime('now')),
      description TEXT NOT NULL DEFAULT ''
  )`);
}

function isApplied(db, id) {
  ensureMigrationsTable(db);
  return Boolean(db.prepare('SELECT 1 FROM schema_migrations WHERE revision = ?').get(id));
}

function record(db, revision) {
  ensureMigrationsTable(db);
  db.prepare('INSERT OR IGNORE INTO schema_migrations (revision, description) VALUES (?, ?)')
    .run(revision.id, revision.description);
}

/**
 * Apply every revision that has not run yet.
 *
 * @param {object} db      an open better-sqlite3 handle
 * @param {object} [logger]
 * @returns {{applied: string[], skipped: string[]}}
 */
function migrate(db, logger = { info() { }, warn() { } }) {
  // Baseline: the migration-tracking table (and v_schema_state). Applied
  // before any revision so the first record() call has somewhere to write.
  const baseline = path.join(ROOT, 'sql', 'migrations.sql');
  if (fs.existsSync(baseline)) {
    db.exec(fs.readFileSync(baseline, 'utf8'));
  }

  ensureMigrationsTable(db);

  const applied = [];
  const skipped = [];

  for (const revision of REVISIONS) {
    if (isApplied(db, revision.id)) {
      skipped.push(revision.id);
      continue;
    }

    const file = path.join(ROOT, revision.file);
    if (!fs.existsSync(file)) {
      logger.warn?.(`[migrate] missing ${revision.file} - skipping ${revision.id}`);
      continue;
    }

    db.exec(fs.readFileSync(file, 'utf8'));
    record(db, revision);
    applied.push(revision.id);
    logger.info?.(`[migrate] applied ${revision.id} - ${revision.description}`);
  }

  return { applied, skipped };
}

/**
 * Apply every revision from scratch (used on a brand-new database file).
 * Equivalent to migrate() today, but kept separate so the intent is clear.
 */
function exec(db, logger) {
  return migrate(db, logger);
}

module.exports = { migrate, exec, REVISIONS, isApplied };
