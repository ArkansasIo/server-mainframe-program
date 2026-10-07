#!/usr/bin/env node
'use strict';

/**
 * import-spreadsheet - load a CSV or XLSX file into a database table.
 *
 * Usage:
 *   node scripts/import-spreadsheet.js --file spreadsheets/USERS.csv --table users
 *   node scripts/import-spreadsheet.js --file spreadsheets/DATASETS.xlsx --table datasets --replace
 *
 * The first row of the file is treated as the column header. Cells are
 * coerced to numbers / booleans / NULL unless --no-coerce is given. Rows are
 * inserted with the columns found in the header, so the file need not list
 * every column in the table.
 */

const fs = require('fs');
const path = require('path');

const { loadConfig } = require('../src/config');
const { openDatabase } = require('../src/db/connection');
const { parseObjects, coerceCell } = require('../src/util/csv');
const logger = require('../src/core/logger');

function parseArgs(argv) {
  const opts = { file: null, table: null, replace: false, coerce: true, configPath: null };
  for (let i = 0; i < argv.length; i += 1) {
    switch (argv[i]) {
      case '--file': opts.file = argv[++i]; break;
      case '--table': opts.table = argv[++i]; break;
      case '--replace': opts.replace = true; break;
      case '--no-coerce': opts.coerce = false; break;
      case '--config': opts.configPath = argv[++i]; break;
      default: break;
    }
  }
  return opts;
}

/** Read a .csv/.txt file, or the first worksheet of an .xlsx file. */
async function readRows(file, opts) {
  const ext = path.extname(file).toLowerCase();

  if (ext === '.csv' || ext === '.txt') {
    return parseObjects(fs.readFileSync(file, 'utf8'));
  }

  if (ext === '.xlsx') {
    let ExcelJS;
    try {
      ExcelJS = require('exceljs');
    } catch (err) {
      throw new Error(
        'Reading .xlsx needs exceljs. Run "npm install" or export the data as CSV.',
      );
    }
    const workbook = new ExcelJS.Workbook();
    await workbook.xlsx.readFile(file);
    const sheet = workbook.worksheets[0];
    if (!sheet) return [];

    const headers = [];
    sheet.getRow(1).eachCell((cell, col) => { headers[col] = String(cell.value ?? '').trim(); });

    const rows = [];
    sheet.eachRow((row, index) => {
      if (index === 1) return; // header
      const obj = {};
      let hasValue = false;
      row.eachCell({ includeEmpty: true }, (cell, col) => {
        const key = headers[col];
        if (!key) return;
        let value = cell.value;
        if (value && typeof value === 'object' && 'result' in value) value = value.result;
        obj[key] = value === undefined ? '' : value;
        if (String(obj[key]).trim() !== '') hasValue = true;
      });
      if (hasValue) rows.push(obj);
    });
    return rows;
  }

  throw new Error(`Unsupported file type: ${ext || '(none)'}`);
}

function quoteIdent(name) {
  return `"${String(name).replace(/"/g, '""')}"`;
}

async function main() {
  const opts = parseArgs(process.argv.slice(2));
  if (!opts.file || !opts.table) {
    throw new Error('Both --file and --table are required.');
  }

  const file = path.isAbsolute(opts.file) ? opts.file : path.resolve(opts.file);
  if (!fs.existsSync(file)) throw new Error(`File not found: ${file}`);

  const config = loadConfig({ configPath: opts.configPath });
  const rows = await readRows(file, opts);
  if (!rows.length) {
    logger.warn('[import] no data rows found');
    return;
  }

  const columns = Object.keys(rows[0]);
  const { db, close } = openDatabase(config, logger);

  try {
    if (opts.table === 'audit_log' || opts.table === 'dataset_records') {
      logger.warn(`[import] ${opts.table} is normally written by triggers; importing anyway`);
    }

    if (opts.replace) {
      db.prepare(`DELETE FROM ${quoteIdent(opts.table)}`).run();
      logger.warn(`[import] cleared existing rows from ${opts.table}`);
    }

    const sql = `INSERT INTO ${quoteIdent(opts.table)} (${columns.map(quoteIdent).join(', ')})
                 VALUES (${columns.map(() => '?').join(', ')})`;
    const statement = db.prepare(sql);

    let inserted = 0;
    let failed = 0;
    const insertMany = db.transaction((batch) => {
      for (const row of batch) {
        const values = columns.map((c) => (opts.coerce ? coerceCell(row[c]) : row[c]));
        try {
          statement.run(...values);
          inserted += 1;
        } catch (err) {
          failed += 1;
          logger.warn(`[import] row rejected (${err.message})`);
        }
      }
    });

    insertMany(rows);

    logger.info(`[import] ${opts.table}: inserted ${inserted}, rejected ${failed} of ${rows.length}`);
    if (failed) process.exitCode = 1;
  } finally {
    close();
  }
}

if (require.main === module) {
  main().catch((err) => {
    process.stderr.write(`${err.message}\n`);
    process.exit(1);
  });
}

module.exports = { readRows, parseArgs };
