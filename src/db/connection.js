'use strict';

const fs = require('fs');
const path = require('path');

/**
 * Small in-memory SQL engine shim used when driver = "memory".
 * It supports the subset of SQL this project actually issues through the
 * repository layer (simple INSERT / SELECT / UPDATE / DELETE). It exists so
 * the server can boot without a native SQLite binding - not as a general
 * purpose database.
 */
class MemoryDatabase {
  constructor() {
    this.tables = new Map();
    this._seq = new Map();
  }

  table(name) {
    if (!this.tables.has(name)) this.tables.set(name, []);
    return this.tables.get(name);
  }

  nextId(name) {
    const next = (this._seq.get(name) || 0) + 1;
    this._seq.set(name, next);
    return next;
  }

  pragma() { return []; }

  exec() { /* no-op: DDL is not tracked in memory mode */ }

  prepare(sql) {
    const self = this;
    const normalized = sql.replace(/\s+/g, ' ').trim();
    return {
      all(...params) { return self._run(normalized, params); },
      get(...params) { return self._run(normalized, params)[0]; },
      run(...params) {
        const rows = self._run(normalized, params);
        return { changes: rows.changes || 0, lastInsertRowid: rows.lastInsertRowid || 0 };
      },
    };
  }

  _run(sql, params) {
    const insert = /^insert\s+into\s+([a-z_]+)/i.exec(sql);
    if (insert) {
      const table = insert[1].toLowerCase();
      const cols = /\(([^)]+)\)\s*values/i.exec(sql);
      const columns = cols ? cols[1].split(',').map((c) => c.trim().replace(/["'`]/g, '')) : [];
      const row = { rowid: this.nextId(table) };
      columns.forEach((c, i) => { row[c] = params[i] !== undefined ? params[i] : null; });
      if (row[`${table.slice(0, -1)}_id`] === undefined) row[`${table.slice(0, -1)}_id`] = row.rowid;
      this.table(table).push(row);
      const out = [row];
      out.changes = 1;
      out.lastInsertRowid = row.rowid;
      return out;
    }

    const select = /^select\s+(.*?)\s+from\s+([a-z_]+)/i.exec(sql);
    if (select) {
      const table = select[2].toLowerCase();
      let rows = this.table(table).slice();
      const where = /where\s+([a-z_]+)\s*=\s*\?/i.exec(sql);
      if (where) {
        const col = where[1];
        rows = rows.filter((r) => String(r[col]) === String(params[0]));
      }
      const limit = /limit\s+(\d+)/i.exec(sql);
      if (limit) rows = rows.slice(0, parseInt(limit[1], 10));
      return rows;
    }

    return [];
  }

  close() { this.tables.clear(); }
}

/**
 * Open a database handle based on configuration.
 *
 * @param {object} config resolved configuration object
 * @param {object} [logger]
 * @returns {{ db: object, driver: string, close: Function, raw: boolean }}
 */
function openDatabase(config, logger = console) {
  const driver = config.database.driver;

  if (driver === 'memory') {
    logger.info?.('[db] using in-memory driver');
    const db = new MemoryDatabase();
    return { db, driver, raw: false, close: () => db.close() };
  }

  const file = config.database.file;
  fs.mkdirSync(path.dirname(file), { recursive: true });

  let BetterSqlite3;
  try {
    BetterSqlite3 = require('better-sqlite3');
  } catch (err) {
    throw new Error(
      'better-sqlite3 is not installed. Run "npm install" or set database.driver to "memory".',
    );
  }

  const db = new BetterSqlite3(file);
  const pragmas = config.database.pragmas || {};
  for (const [key, value] of Object.entries(pragmas)) {
    db.pragma(`${key} = ${value}`);
  }

  logger.info?.(`[db] opened ${file}`);
  return {
    db,
    driver: 'sqlite',
    raw: true,
    file,
    close: () => db.close(),
  };
}

module.exports = { openDatabase, MemoryDatabase };
