#!/usr/bin/env node
'use strict';

/**
 * db-query - run the ad-hoc SQL from sql/queries/*.sql, or an inline query.
 *
 * A query file may contain several statements, each introduced by a line of
 * the form:
 *
 *   -- name: something
 *
 * Statements are run one at a time, in order.
 *
 * Reads only, by default. The integrity file also carries repair statements
 * that modify data; those require --write, so looking at a repair can never
 * apply it by accident.
 *
 * Usage:
 *   node scripts/db-query.js --file sql/queries/reporting.sql
 *   node scripts/db-query.js --file sql/queries/capacity.sql --name largest_datasets
 *   node scripts/db-query.js --sql "SELECT COUNT(*) AS n FROM jobs"
 *   node scripts/db-query.js --list sql/queries/integrity.sql
 *   node scripts/db-query.js --file sql/queries/integrity.sql --name repair_counters --write
 */

const fs = require('fs');
const path = require('path');

const { loadConfig } = require('../src/config');
const { openDatabase } = require('../src/db/connection');
const { renderTable } = require('../src/util/table');

const ROOT = path.resolve(__dirname, '..');
const NAME_RE = /^\s*--\s*name:\s*(\S+)/i;

/**
 * Split a query file into named statements.
 * @returns {Array<{name: string|null, sql: string}>}
 */
function splitStatements(text) {
  const lines = text.split(/\r?\n/);
  const statements = [];
  let name = null;
  let buffer = [];

  /**
   * Strip whole-line comments, so a block of prose is not mistaken for SQL.
   *
   * A query file opens with a comment header; without this it became a
   * statement of its own, and running the file with no --name executed that
   * header and reported "contains no statements" - even though the named
   * queries below it were perfectly good.
   */
  const stripComments = (sql) => sql
    .split('\n')
    .filter((line) => !/^\s*--/.test(line))
    .join('\n')
    .trim();

  const flush = () => {
    const sql = buffer.join('\n').trim();
    // `sql` keeps the comments (they may be worth showing); `runnable` decides
    // whether there is anything here to execute.
    if (sql && stripComments(sql)) statements.push({ name, sql });
    buffer = [];
    name = null;
  };

  for (const line of lines) {
    const match = NAME_RE.exec(line);
    if (match) {
      flush();
      name = match[1];
      continue;
    }
    buffer.push(line);
  }
  flush();
  return statements;
}

function parseArgs(argv) {
  const opts = { file: null, sql: null, name: null, list: false, json: false, write: false };
  for (let i = 0; i < argv.length; i += 1) {
    switch (argv[i]) {
      case '--file': opts.file = argv[++i]; break;
      case '--sql': opts.sql = argv[++i]; break;
      case '--name': opts.name = argv[++i]; break;
      case '--list': opts.list = true; break;
      case '--json': opts.json = true; break;
      // Reads are the default; a write has to be asked for explicitly, so a
      // repair cannot be applied by someone who only meant to look.
      case '--write':
      case '--apply': opts.write = true; break;
      default: break;
    }
  }
  return opts;
}

function runStatement(db, statement, opts) {
  const prepared = db.prepare(statement.sql);

  // A statement that returns rows is a query; one that does not is a write.
  // `reader` is how better-sqlite3 reports which it is, and it is the only
  // reliable way to tell without parsing the SQL.
  if (!prepared.reader) {
    if (!opts.write) {
      throw new Error(
        `${statement.name || 'statement'} is a write. Re-run with --write to apply it.`,
      );
    }

    const info = prepared.run();
    if (opts.json) {
      process.stdout.write(`${JSON.stringify({
        name: statement.name, changes: info.changes, lastInsertRowid: info.lastInsertRowid,
      }, null, 2)}\n`);
    } else {
      process.stdout.write(`\n== ${statement.name || 'statement'} ==\n`);
      process.stdout.write(`${info.changes} row(s) changed\n`);
    }
    return;
  }

  const rows = prepared.all();
  if (opts.json) {
    process.stdout.write(`${JSON.stringify({ name: statement.name, rows }, null, 2)}\n`);
  } else {
    process.stdout.write(`\n== ${statement.name || 'query'} ==\n`);
    process.stdout.write(renderTable(rows));
    process.stdout.write('\n');
  }
}

function main() {
  const opts = parseArgs(process.argv.slice(2));
  if (!opts.file && !opts.sql) {
    throw new Error('Nothing to run. Pass --file <path> or --sql "<query>".');
  }

  let statements;
  if (opts.sql) {
    statements = [{ name: 'inline', sql: opts.sql }];
  } else {
    const file = path.isAbsolute(opts.file) ? opts.file : path.join(ROOT, opts.file);
    if (!fs.existsSync(file)) throw new Error(`Query file not found: ${file}`);
    statements = splitStatements(fs.readFileSync(file, 'utf8'));
  }

  if (opts.name) {
    const wanted = statements.filter((s) => s.name === opts.name);
    if (!wanted.length) {
      const available = statements.map((s) => s.name).filter(Boolean).join(', ');
      throw new Error(`No query named "${opts.name}". Available: ${available}`);
    }
    statements = wanted;
  }

  if (opts.list) {
    for (const s of statements) process.stdout.write(`${s.name || '(unnamed)'}\n`);
    return;
  }

  const config = loadConfig();
  const { db, close } = openDatabase(config, loggerQuiet());

  try {
    for (const statement of statements) {
      runStatement(db, statement, opts);
    }
  } finally {
    close();
  }
}

/** A logger that stays out of the way of the query output. */
function loggerQuiet() {
  return { info() { }, debug() { }, warn() { }, error() { } };
}

if (require.main === module) {
  try {
    main();
  } catch (err) {
    process.stderr.write(`${err.message}\n`);
    process.exit(1);
  }
}

module.exports = { splitStatements, parseArgs };
