#!/usr/bin/env node
'use strict';

/**
 * export-spreadsheet - export configured database tables to spreadsheets.
 *
 * Usage:
 *   npm run db:export                       # all tables from config -> .xlsx
 *   node scripts/export-spreadsheet.js --engine csv --out data/spreadsheets
 *   node scripts/export-spreadsheet.js --table USERS
 *
 * Which tables are exported, and the SQL behind each one, comes from
 * spreadsheet.tables in config/default.json. The output directory defaults
 * to storage.spreadsheets.
 */

const fs = require('fs');
const path = require('path');

const { loadConfig } = require('../src/config');
const { openDatabase } = require('../src/db/connection');
const { stringify } = require('../src/util/csv');
const logger = require('../src/core/logger');

function parseArgs(argv) {
  const opts = { engine: null, out: null, table: null, configPath: null };
  for (let i = 0; i < argv.length; i += 1) {
    switch (argv[i]) {
      case '--engine': opts.engine = argv[++i]; break;
      case '--out': opts.out = argv[++i]; break;
      case '--table': opts.table = argv[++i]; break;
      case '--config': opts.configPath = argv[++i]; break;
      default: break;
    }
  }
  return opts;
}

/**
 * Load exceljs lazily: it is an optional dependency, and the CSV engine must
 * keep working when it is not installed.
 */
function loadExcel() {
  try {
    return require('exceljs');
  } catch (err) {
    throw new Error(
      'The "excel" engine needs exceljs. Run "npm install" or use --engine csv.',
    );
  }
}

function exportCsv(rows, file) {
  fs.mkdirSync(path.dirname(file), { recursive: true });
  fs.writeFileSync(file, stringify(rows), 'utf8');
}

async function exportExcel(rows, file, config) {
  const ExcelJS = loadExcel();
  const workbook = new ExcelJS.Workbook();
  workbook.creator = config.system.name;
  workbook.created = new Date();

  const sheet = workbook.addWorksheet(config.spreadsheet.defaultSheet || 'MAINFRAME');

  const columns = rows.length ? Object.keys(rows[0]) : [];
  sheet.columns = columns.map((key) => ({ header: key, key, width: Math.max(12, key.length + 2) }));

  for (const row of rows) sheet.addRow(row);

  const style = config.spreadsheet.headerStyle || {};
  const header = sheet.getRow(1);
  header.font = { bold: style.bold !== false, color: { argb: style.color || 'FFFFFFFF' } };
  if (style.fill) {
    header.fill = { type: 'pattern', pattern: 'solid', fgColor: { argb: `FF${style.fill}` } };
  }
  if (config.spreadsheet.export?.freezeHeaderRow) sheet.views = [{ state: 'frozen', ySplit: 1 }];

  fs.mkdirSync(path.dirname(file), { recursive: true });
  await workbook.xlsx.writeFile(file);
}

async function main() {
  const opts = parseArgs(process.argv.slice(2));
  const config = loadConfig({ configPath: opts.configPath });

  const engine = (opts.engine || config.spreadsheet.engine || 'excel').toLowerCase();
  const outDir = opts.out
    ? path.resolve(opts.out)
    : config.storage.spreadsheets;

  let tables = config.spreadsheet.tables || [];
  if (opts.table) {
    const wanted = opts.table.toUpperCase();
    tables = tables.filter((t) => t.name.toUpperCase() === wanted);
    if (!tables.length) throw new Error(`No configured table named "${opts.table}"`);
  }

  if (!tables.length) throw new Error('No spreadsheet.tables configured.');

  fs.mkdirSync(outDir, { recursive: true });

  // Read every table first, then close the connection before doing any async
  // file writing. better-sqlite3 is a native module: if a prepared statement
  // is still reachable when the connection is torn down (which can happen
  // across an await on Node 24), its destructor aborts the process. Doing all
  // reads synchronously and closing up front avoids that entirely.
  const datasets = [];
  const { db, close } = openDatabase(config, logger);
  try {
    for (const table of tables) {
      datasets.push({ table, rows: db.prepare(table.query).all() });
    }
  } finally {
    close();
  }

  for (const { table, rows } of datasets) {
    const ext = engine === 'csv' ? 'csv' : 'xlsx';
    const file = path.join(outDir, `${table.name}.${ext}`);

    if (engine === 'csv') {
      exportCsv(rows, file);
    } else {
      await exportExcel(rows, file, config);
    }

    logger.info(`[export] ${table.name}: ${rows.length} rows -> ${file}`);
  }

  logger.info(`[export] done (${engine} engine) -> ${outDir}`);
}

if (require.main === module) {
  main().catch((err) => {
    process.stderr.write(`${err.message}\n`);
    process.exit(1);
  });
}

module.exports = { exportCsv, exportExcel, parseArgs };
