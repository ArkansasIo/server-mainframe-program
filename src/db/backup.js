'use strict';

/**
 * Database backups.
 *
 * `database.backup` (`enabled`, `directory`, `retain`) has always been read and
 * resolved by the config loader, but no backup job existed. This is it.
 *
 * Why not just copy the file
 * --------------------------
 * The database runs in WAL mode with more than one connection. Copying
 * `mainframe.db` while a transaction is committing captures a torn state: the
 * main file plus a `-wal` sidecar that the copy does not include. SQLite
 * provides `VACUUM INTO`, which produces a single consistent snapshot file in
 * one statement. That is what this uses - not `fs.copyFileSync`.
 *
 * Retention
 * ---------
 * Backups are named `<basename>-<timestamp>.db`, so lexical order is
 * chronological and the newest is always last. Pruning deletes from the front,
 * which is what makes `retain` mean "keep the most recent N" without having to
 * stat every file.
 */

const fs = require('fs');
const path = require('path');

/** `2026-01-14T09-30-00` - sortable, and legal on Windows (no colons). */
function timestamp(date = new Date()) {
  return date.toISOString().slice(0, 19).replace(/:/g, '-');
}

/**
 * The backup filename for a database and a moment.
 * @returns {string} absolute path
 */
function backupPath(directory, databaseFile, date) {
  const base = path.basename(databaseFile, path.extname(databaseFile));
  return path.join(directory, `${base}-${timestamp(date)}.db`);
}

/** List existing backups for a database, oldest first. */
function listBackups(directory, databaseFile) {
  const base = path.basename(databaseFile, path.extname(databaseFile));

  let entries;
  try {
    entries = fs.readdirSync(directory);
  } catch {
    return [];
  }

  return entries
    .filter((name) => name.startsWith(`${base}-`) && name.endsWith('.db'))
    .sort() // lexical == chronological, given the timestamp format
    .map((name) => {
      const full = path.join(directory, name);
      let size = 0;
      try {
        size = fs.statSync(full).size;
      } catch { /* raced with a delete; report 0 */ }
      return { name, path: full, size };
    });
}

/**
 * Delete the oldest backups, keeping `retain`.
 *
 * @returns {string[]} names removed
 */
function pruneBackups(directory, databaseFile, retain) {
  if (!Number.isInteger(retain) || retain <= 0) return [];

  const backups = listBackups(directory, databaseFile);
  const excess = backups.length - retain;
  if (excess <= 0) return [];

  const removed = [];
  for (const backup of backups.slice(0, excess)) {
    try {
      fs.rmSync(backup.path);
      removed.push(backup.name);
    } catch {
      // A backup we could not delete is not worth failing the run over; the
      // next attempt will try again.
    }
  }
  return removed;
}

/**
 * Take a backup.
 *
 * @param {object} opts
 * @param {object} opts.db            better-sqlite3 handle
 * @param {object} opts.config        resolved configuration
 * @param {object} [opts.logger]
 * @param {Date}   [opts.now]         injectable for tests
 * @param {boolean} [opts.force]      run even when backup.enabled is false
 * @returns {{ok: boolean, path?: string, bytes?: number, pruned?: string[], reason?: string}}
 */
function backupDatabase(opts) {
  const { db, config } = opts;
  const logger = opts.logger || { info() { }, warn() { }, debug() { } };
  const settings = (config.database && config.database.backup) || {};

  if (!opts.force && settings.enabled === false) {
    return { ok: false, reason: 'backups are disabled (database.backup.enabled = false)' };
  }
  if (config.database.driver === 'memory') {
    return { ok: false, reason: 'the in-memory driver has nothing to back up' };
  }

  const directory = settings.directory
    || path.join(path.dirname(config.database.file), 'backups');
  const retain = Number.isInteger(settings.retain) ? settings.retain : 10;

  fs.mkdirSync(directory, { recursive: true });
  const target = backupPath(directory, config.database.file, opts.now);

  // VACUUM INTO writes a consistent snapshot in one statement, which a file
  // copy cannot do against a WAL database that is being written to.
  db.prepare('VACUUM INTO ?').run(target);

  const bytes = fs.statSync(target).size;
  logger.info(`[db:backup] wrote ${target} (${bytes} bytes)`);

  const pruned = pruneBackups(directory, config.database.file, retain);
  for (const name of pruned) {
    logger.info(`[db:backup] pruned ${name} (retaining ${retain})`);
  }

  return { ok: true, path: target, bytes, pruned };
}

/**
 * Verify a backup is a readable SQLite database with the expected tables.
 *
 * A backup that cannot be opened is worse than no backup, because it is
 * trusted. This opens the file and asks it what it contains.
 *
 * @returns {{ok: boolean, tables?: string[], reason?: string}}
 */
function verifyBackup(file) {
  if (!fs.existsSync(file)) return { ok: false, reason: `no such file: ${file}` };

  let Database;
  try {
    Database = require('better-sqlite3');
  } catch {
    // Without the native binding we cannot open it, so say so rather than
    // reporting a pass we did not establish.
    return { ok: false, reason: 'better-sqlite3 is not available to verify the file' };
  }

  let handle;
  try {
    handle = new Database(file, { readonly: true });
    const tables = handle
      .prepare("SELECT name FROM sqlite_master WHERE type = 'table' ORDER BY name")
      .all()
      .map((row) => row.name);

    if (!tables.length) return { ok: false, reason: 'the file contains no tables' };

    // The integrity check is the point: a file that opens but is corrupt would
    // otherwise look fine right up until it is needed.
    const integrity = handle.pragma('integrity_check');
    const verdict = Array.isArray(integrity) && integrity[0] && integrity[0].integrity_check;
    if (verdict && verdict !== 'ok') {
      return { ok: false, reason: `integrity check failed: ${verdict}` };
    }

    return { ok: true, tables };
  } catch (err) {
    return { ok: false, reason: err.message };
  } finally {
    if (handle) handle.close();
  }
}

module.exports = {
  backupDatabase,
  verifyBackup,
  listBackups,
  pruneBackups,
  backupPath,
  timestamp,
};
