'use strict';

/**
 * Database tests: schema, seed data, triggers, views and the migrator.
 *
 * Each test opens its own database in a temp directory so the suite never
 * touches the developer's ./data/mainframe.db.
 */

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');

const { openDatabase } = require('../src/db/connection');

let driverAvailable = true;
try {
  require('better-sqlite3');
} catch {
  driverAvailable = false;
}

// A single shared connection for the whole file.
//
// better-sqlite3 is a native module: destroying a prepared Statement after the
// connection's environment has been torn down trips a native assertion
// ("(env) != nullptr") and aborts the process. Opening and closing a database
// per test leaves many live Statement objects at exit, so instead we open one
// database, roll each test back to a clean state with a fresh schema, and
// close exactly once at process exit.
let shared = null;

function freshDatabase() {
  if (!driverAvailable) return null;
  if (!shared) {
    const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'mf-test-'));
    const file = path.join(dir, 'test.db');

    const config = {
      database: {
        driver: 'sqlite',
        file,
        pragmas: { journal_mode: 'WAL', foreign_keys: 'ON', busy_timeout: 5000 },
      },
    };

    const handle = openDatabase(config, { info() { }, warn() { }, error() { } });
    const { migrate } = require('../src/db/migrate');
    migrate(handle.db, { info() { }, warn() { } });

    shared = { ...handle, dir };

    // Deliberately do NOT close the connection.
    //
    // better-sqlite3 v11 on Node 24 aborts during process teardown if a
    // prepared Statement is destroyed after the connection environment is
    // gone ("Assertion failed: (env) != nullptr"). Closing here, from an
    // 'exit' handler, races that teardown. Leaving the handle open lets
    // SQLite flush and the process exit normally; the temp directory is
    // reclaimed by the OS.
  }
  return shared;
}

/**
 * Restore the seed baseline between tests. Re-running the seed + refresh
 * statements is cheap and leaves every test with the same starting rows.
 */
function resetData(handle) {
  const dataTables = [
    'dataset_records', 'jobs', 'transactions', 'audit_log',
    'datasets', 'volumes', 'users', 'systems',
  ];
  handle.db.exec('PRAGMA foreign_keys = OFF');
  for (const table of dataTables) {
    handle.db.exec(`DELETE FROM ${table}`);
  }
  handle.db.exec('PRAGMA foreign_keys = ON');
  handle.db.exec('DELETE FROM sqlite_sequence WHERE name IS NOT NULL');

  const ROOT = path.resolve(__dirname, '..');
  handle.db.exec(fs.readFileSync(path.join(ROOT, 'sql', 'seed.sql'), 'utf8'));
}

/**
 * Between-test cleanup. Named `cleanup` because the tests call it in a
 * finally block; it restores the seed baseline rather than closing the
 * shared connection (which happens once, at process exit).
 */
const cleanup = resetData;

const skip = driverAvailable ? false : 'better-sqlite3 is not installed';

test('migrator creates every table', { skip }, () => {
  const handle = freshDatabase();
  try {
    const tables = handle.db
      .prepare("SELECT name FROM sqlite_master WHERE type = 'table'")
      .all()
      .map((r) => r.name);

    for (const expected of [
      'systems', 'users', 'datasets', 'dataset_records',
      'jobs', 'transactions', 'audit_log', 'volumes',
      'system_parameters', 'schema_migrations',
    ]) {
      assert.ok(tables.includes(expected), `missing table: ${expected}`);
    }
  } finally {
    cleanup(handle);
  }
});

test('migrator records all six revisions', { skip }, () => {
  const handle = freshDatabase();
  try {
    const revisions = handle.db
      .prepare('SELECT revision FROM schema_migrations ORDER BY revision')
      .all()
      .map((r) => r.revision);

    assert.deepEqual(revisions, [
      '001_schema', '002_seed', '003_views',
      '004_indexes', '005_triggers', '006_views_ops',
    ]);
  } finally {
    cleanup(handle);
  }
});

test('migrator is idempotent - a second run applies nothing', { skip }, () => {
  const handle = freshDatabase();
  try {
    const { migrate } = require('../src/db/migrate');
    const result = migrate(handle.db, { info() { }, warn() { } });
    assert.equal(result.applied.length, 0);
    assert.equal(result.skipped.length, 6);
  } finally {
    cleanup(handle);
  }
});

test('seed data is loaded', { skip }, () => {
  const handle = freshDatabase();
  try {
    assert.equal(handle.db.prepare('SELECT COUNT(*) n FROM systems').get().n, 3);
    assert.equal(handle.db.prepare('SELECT COUNT(*) n FROM users').get().n, 6);
    assert.equal(handle.db.prepare('SELECT COUNT(*) n FROM datasets').get().n, 6);
    assert.equal(handle.db.prepare('SELECT COUNT(*) n FROM jobs').get().n, 5);
    assert.equal(handle.db.prepare('SELECT COUNT(*) n FROM volumes').get().n, 4);
  } finally {
    cleanup(handle);
  }
});

test('foreign keys are enforced', { skip }, () => {
  const handle = freshDatabase();
  try {
    assert.throws(
      () => handle.db
        .prepare('INSERT INTO dataset_records (dataset_id, sequence, payload) VALUES (?, ?, ?)')
        .run(99999, 1, 'orphan'),
      /FOREIGN KEY/,
    );
  } finally {
    cleanup(handle);
  }
});

test('unique username constraint is enforced', { skip }, () => {
  const handle = freshDatabase();
  try {
    assert.throws(
      () => handle.db
        .prepare('INSERT INTO users (username) VALUES (?)')
        .run('IBMUSER'),
      /UNIQUE/,
    );
  } finally {
    cleanup(handle);
  }
});

test('status CHECK constraint rejects an invalid value', { skip }, () => {
  const handle = freshDatabase();
  try {
    assert.throws(
      () => handle.db
        .prepare('INSERT INTO users (username, status) VALUES (?, ?)')
        .run('BADSTATUS', 'NONSENSE'),
      /CHECK/,
    );
  } finally {
    cleanup(handle);
  }
});

test('record insert trigger keeps dataset counters in step', { skip }, () => {
  const handle = freshDatabase();
  try {
    const before = handle.db
      .prepare('SELECT record_count, bytes_used FROM datasets WHERE dataset_id = 5')
      .get();

    handle.db
      .prepare('INSERT INTO dataset_records (dataset_id, sequence, payload) VALUES (?, ?, ?)')
      .run(5, 99, 'ABCDEFGHIJ');

    const after = handle.db
      .prepare('SELECT record_count, bytes_used FROM datasets WHERE dataset_id = 5')
      .get();

    assert.equal(after.record_count, before.record_count + 1);
    assert.equal(after.bytes_used, before.bytes_used + 10);
  } finally {
    cleanup(handle);
  }
});

test('record delete trigger decrements the counters', { skip }, () => {
  const handle = freshDatabase();
  try {
    const before = handle.db
      .prepare('SELECT record_count FROM datasets WHERE dataset_id = 5')
      .get().record_count;

    const recordId = handle.db
      .prepare('SELECT record_id FROM dataset_records WHERE dataset_id = 5 LIMIT 1')
      .get().record_id;

    handle.db.prepare('DELETE FROM dataset_records WHERE record_id = ?').run(recordId);

    const after = handle.db
      .prepare('SELECT record_count FROM datasets WHERE dataset_id = 5')
      .get().record_count;

    assert.equal(after, before - 1);
  } finally {
    cleanup(handle);
  }
});

test('dataset insert is journalled into audit_log by a trigger', { skip }, () => {
  const handle = freshDatabase();
  try {
    const before = handle.db
      .prepare("SELECT COUNT(*) n FROM audit_log WHERE event_type = 'DATASET'")
      .get().n;

    handle.db
      .prepare('INSERT INTO datasets (name, volume, owner_user_id) VALUES (?, ?, ?)')
      .run('MF1.TEST.TRIGGER.CHECK', 'MFVOL1', 1);

    const after = handle.db
      .prepare("SELECT COUNT(*) n FROM audit_log WHERE event_type = 'DATASET'")
      .get().n;

    assert.ok(after > before, 'expected a new DATASET audit row');
  } finally {
    cleanup(handle);
  }
});

test('updated_at trigger fires on a dataset update', { skip }, () => {
  const handle = freshDatabase();
  try {
    handle.db.prepare('UPDATE datasets SET lrecl = 999 WHERE dataset_id = 5').run();
    const row = handle.db
      .prepare('SELECT lrecl, updated_at FROM datasets WHERE dataset_id = 5')
      .get();
    assert.equal(row.lrecl, 999);
    assert.ok(row.updated_at);
  } finally {
    cleanup(handle);
  }
});

test('reporting views exist and are queryable', { skip }, () => {
  const handle = freshDatabase();
  try {
    for (const view of [
      'v_job_summary', 'v_dataset_usage', 'v_transaction_activity',
      'v_user_activity', 'v_audit_recent', 'v_system_health',
      'v_open_alerts', 'v_job_queue_by_class', 'v_storage_pressure',
      'v_dataset_catalog', 'v_schema_state',
    ]) {
      assert.doesNotThrow(
        () => handle.db.prepare(`SELECT * FROM ${view} LIMIT 1`).all(),
        `view not queryable: ${view}`,
      );
    }
  } finally {
    cleanup(handle);
  }
});

test('catalog starts free of counter drift', { skip }, () => {
  const handle = freshDatabase();
  try {
    const drift = handle.db.prepare('SELECT * FROM v_counter_drift').all();
    assert.deepEqual(drift, []);
  } finally {
    cleanup(handle);
  }
});

test('catalog starts free of orphaned records', { skip }, () => {
  const handle = freshDatabase();
  try {
    const orphans = handle.db.prepare('SELECT * FROM v_orphaned_records').all();
    assert.deepEqual(orphans, []);
  } finally {
    cleanup(handle);
  }
});

test('foreign_key_check reports no violations on a fresh database', { skip }, () => {
  const handle = freshDatabase();
  try {
    const violations = handle.db.prepare('PRAGMA foreign_key_check').all();
    assert.deepEqual(violations, []);
  } finally {
    cleanup(handle);
  }
});

test('v_schema_state lists the applied revisions', { skip }, () => {
  const handle = freshDatabase();
  try {
    const rows = handle.db
      .prepare('SELECT revision FROM v_schema_state ORDER BY revision')
      .all();
    assert.equal(rows.length, 6);
  } finally {
    cleanup(handle);
  }
});

test('integrity_check passes', { skip }, () => {
  const handle = freshDatabase();
  try {
    const result = handle.db.prepare('PRAGMA integrity_check').get();
    assert.equal(result.integrity_check, 'ok');
  } finally {
    cleanup(handle);
  }
});
