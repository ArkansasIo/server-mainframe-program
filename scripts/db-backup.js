#!/usr/bin/env node
'use strict';

/**
 * db-backup - take a consistent snapshot of the database.
 *
 * Usage:
 *   node scripts/db-backup.js                 # uses database.backup settings
 *   node scripts/db-backup.js --out <dir>     # override the destination
 *   node scripts/db-backup.js --retain 5      # override the retention count
 *   node scripts/db-backup.js --force         # run even if backups are disabled
 *   node scripts/db-backup.js --list          # show existing backups
 *   node scripts/db-backup.js --verify <file> # check a backup is readable
 *
 * The snapshot is taken with SQLite's VACUUM INTO rather than by copying the
 * file: the database runs in WAL mode, and a copy taken while a transaction is
 * committing can capture a torn state.
 */

const path = require('path');

const { loadConfig } = require('../src/config');
const { openDatabase } = require('../src/db/connection');
const { backupDatabase, verifyBackup, listBackups } = require('../src/db/backup');
const logger = require('../src/core/logger');

function parseArgs(argv) {
  const opts = { out: null, retain: null, force: false, list: false, verify: null, quiet: false };

  for (let i = 0; i < argv.length; i += 1) {
    switch (argv[i]) {
      case '--out': opts.out = argv[++i]; break;
      case '--retain': opts.retain = parseInt(argv[++i], 10); break;
      case '--force': opts.force = true; break;
      case '--list': opts.list = true; break;
      case '--verify': opts.verify = argv[++i]; break;
      case '--config': opts.configPath = argv[++i]; break;
      case '--quiet': opts.quiet = true; break;
      default: break;
    }
  }

  return opts;
}

/** Human-readable size, for the listing. */
function size(bytes) {
  if (bytes < 1024) return `${bytes} B`;
  if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KiB`;
  return `${(bytes / (1024 * 1024)).toFixed(1)} MiB`;
}

function main() {
  const opts = parseArgs(process.argv.slice(2));
  const config = loadConfig({ configPath: opts.configPath });

  // --verify works on a file alone and needs no database connection.
  if (opts.verify) {
    const file = path.resolve(opts.verify);
    const result = verifyBackup(file);

    if (!result.ok) {
      logger.error(`[db:backup] ${file} is NOT usable: ${result.reason}`);
      process.exitCode = 1;
      return;
    }
    logger.info(`[db:backup] ${file} OK - ${result.tables.length} table(s): ${result.tables.join(', ')}`);
    return;
  }

  if (opts.retain !== null) config.database.backup.retain = opts.retain;
  if (opts.out !== null) config.database.backup.directory = path.resolve(opts.out);

  if (opts.list) {
    const directory = config.database.backup.directory;
    const backups = listBackups(directory, config.database.file);

    if (!backups.length) {
      logger.info(`[db:backup] no backups in ${directory}`);
      return;
    }
    logger.info(`[db:backup] ${backups.length} backup(s) in ${directory}`);
    for (const backup of backups) {
      logger.info(`[db:backup]   ${backup.name}  ${size(backup.size)}`);
    }
    return;
  }

  if (config.database.driver === 'memory') {
    logger.error('[db:backup] the in-memory driver has nothing to back up');
    process.exitCode = 1;
    return;
  }

  const { db, close } = openDatabase(config, logger);
  try {
    const result = backupDatabase({ db, config, logger, force: opts.force });

    if (!result.ok) {
      logger.warn(`[db:backup] skipped: ${result.reason}`);
      if (!opts.force) process.exitCode = 1;
      return;
    }

    // Verify what we just wrote: a backup that cannot be opened is worse than
    // none, because it is trusted.
    const check = verifyBackup(result.path);
    if (!check.ok) {
      logger.error(`[db:backup] the backup just written failed verification: ${check.reason}`);
      process.exitCode = 1;
      return;
    }

    logger.info(`[db:backup] verified - ${check.tables.length} table(s)`);
    logger.info(`[db:backup] done -> ${result.path} (${size(result.bytes)})`);
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

module.exports = { parseArgs };
