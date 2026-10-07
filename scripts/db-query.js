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
 * Usage:
 *   node scripts/db-query.js --file sql/queries/reporting.sql
 *   node scripts/db-query.js --file sql/queries/capacity.sql --name largest_datasets
 *   node scripts/db-query.js --sql "SELECT COUNT(*) AS n FROM jobs"
 *   node scripts/db-query.js --list sql/queries/integrity.sql
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

  const flush = () => {
    const sql = buffer.join('\n').trim();
    if (sql) statements.push({ name, sql });
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
  const opts = { file: null, sql: null, name: null, list: false, json: false };
  for (let i = 0; i < argv.length; i += 1) {
    switch (argv[i]) {
      case '--file': opts.file = argv[++i]; break;
      case '--sql': opts.sql = argv[++i]; break;
      case '--name': opts.name = argv[++i]; break;
      case '--list': opts.list = true; break;
      case '--json': opts.json = true; break;
      default: break;
    }
  }
  return opts;
}

function runStatement(db, statement, opts) {
  const rows = db.prepare(statement.sql).all();
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
