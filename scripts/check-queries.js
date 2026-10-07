'use strict';

/**
 * Run every statement in sql/queries/*.sql and report what breaks.
 *
 * The point is to exercise the whole query library against the real schema,
 * because a query file nobody runs is a query file that quietly rots. It also
 * checks the read/write guard: a write must be refused without --write and
 * must succeed with it.
 *
 * Kept as a script rather than a test because it shells out per statement and
 * would make `npm test` slow for a check that is about the SQL, not the code.
 */

const { execFileSync } = require('node:child_process');
const fs = require('node:fs');
const path = require('node:path');

const { loadConfig } = require('../src/config');
const { openDatabase } = require('../src/db/connection');
const { splitStatements } = require('./db-query.js');

const ROOT = path.resolve(__dirname, '..');
const QUERY_DIR = path.join(ROOT, 'sql', 'queries');

/**
 * Is this statement a write?
 *
 * Asked of SQLite rather than guessed from the leading keyword: `PRAGMA`,
 * `VACUUM` and `ANALYZE` are writes too, and a keyword regex misses them -
 * which is exactly how the first version of this script produced two false
 * failures.
 */
function isWrite(db, sql) {
  try {
    return !db.prepare(sql).reader;
  } catch {
    return false; // a statement that will not prepare is a different failure
  }
}

function run(args) {
  return execFileSync('node', [path.join(ROOT, 'scripts', 'db-query.js'), ...args], {
    stdio: 'pipe',
    cwd: ROOT,
  });
}

function main() {
  const config = loadConfig({ argv: [] });
  const { db, close } = openDatabase(config, { info() { }, warn() { }, debug() { }, error() { } });

  let passed = 0;
  let failed = 0;
  let guarded = 0;
  let applied = 0;
  const failures = [];

  try {
    const files = fs.readdirSync(QUERY_DIR).filter((n) => n.endsWith('.sql')).sort();

    for (const file of files) {
      const text = fs.readFileSync(path.join(QUERY_DIR, file), 'utf8');
      const statements = splitStatements(text).filter((s) => s.name);

      for (const statement of statements) {
        const rel = path.join('sql', 'queries', file);
        const base = ['--file', rel, '--name', statement.name];

        if (isWrite(db, statement.sql)) {
          // A write must be refused without --write...
          let refused = false;
          try {
            run(base);
          } catch (err) {
            refused = /Re-run with --write/.test(String(err.stderr || err.stdout || ''));
          }

          if (!refused) {
            failed += 1;
            failures.push(`${file} :: ${statement.name} - a write ran without --write`);
            continue;
          }
          guarded += 1;

          // ...and must succeed with it.
          try {
            run([...base, '--write']);
            passed += 1;
            applied += 1;
          } catch (err) {
            failed += 1;
            const line = String(err.stderr || err.stdout || '').split('\n').filter((l) => l.trim())[0];
            failures.push(`${file} :: ${statement.name} - ${line}`);
          }
          continue;
        }

        try {
          run(base);
          passed += 1;
        } catch (err) {
          failed += 1;
          const line = String(err.stderr || err.stdout || '').split('\n').filter((l) => l.trim())[0];
          failures.push(`${file} :: ${statement.name} - ${line}`);
        }
      }
    }
  } finally {
    close();
  }

  for (const failure of failures) process.stdout.write(`FAIL  ${failure}\n`);
  process.stdout.write(`\npassed: ${passed}   failed: ${failed}`);
  process.stdout.write(`   (${guarded} write(s) guarded, ${applied} applied)\n`);
  if (failed) process.exitCode = 1;
}

if (require.main === module) main();

module.exports = { isWrite };
