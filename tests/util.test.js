'use strict';

/**
 * Utility tests: table rendering, the logger, and the CLI argument parsers
 * exposed by the scripts.
 */

const test = require('node:test');
const assert = require('node:assert/strict');

const { renderTable, renderPanel, truncate, asText } = require('../src/util/table');
const { Logger, LEVELS, createSilentLogger, redact } = require('../src/core/logger');

test('renderTable draws a header and underline', () => {
  const text = renderTable([{ a: '1', b: '2' }], { columns: ['a', 'b'] });
  const lines = text.split('\n');
  assert.match(lines[0], /A\s+\|\s+B/);
  assert.match(lines[1], /-+\+-+/);
  assert.match(lines[2], /1\s+\|\s+2/);
});

test('renderTable handles no rows', () => {
  assert.equal(renderTable([]), '(no columns)');
});

test('renderTable honours the limit and says how many are hidden', () => {
  const rows = Array.from({ length: 5 }, (_, i) => ({ n: String(i) }));
  const text = renderTable(rows, { columns: ['n'], limit: 2 });
  assert.match(text, /3 more row\(s\) not shown/);
});

test('renderTable prints an explicit title', () => {
  const text = renderTable([{ a: 1 }], { title: 'OPERATORS' });
  assert.ok(text.startsWith('OPERATORS'));
});

test('renderPanel renders the key/value style', () => {
  const text = renderPanel([['SYSTEM NAME', 'MAINFRAME-1']]);
  assert.match(text, /SYSTEM NAME \. \. : MAINFRAME-1/);
});

test('truncate shortens long text with an ellipsis', () => {
  assert.equal(truncate('abcdefghij', 5), 'abcd…');
  assert.equal(truncate('abc', 5), 'abc');
});

test('asText renders objects and dates', () => {
  assert.equal(asText(null), '');
  assert.equal(asText({ a: 1 }), '{"a":1}');
  assert.match(asText(new Date('2024-01-15T00:00:00Z')), /^2024-01-15/);
});

test('logger level ordering', () => {
  const logger = new Logger({ level: 'warn', console: false });
  assert.equal(logger.enabled('error'), true);
  assert.equal(logger.enabled('warn'), true);
  assert.equal(logger.enabled('info'), false);
  assert.equal(logger.enabled('debug'), false);
});

test('logger setLevel changes the threshold', () => {
  const logger = new Logger({ level: 'error', console: false });
  logger.setLevel('debug');
  assert.equal(logger.enabled('debug'), true);
});

test('logger child carries bound context without throwing', () => {
  const logger = new Logger({ level: 'info', console: false });
  const child = logger.child({ requestId: 'abc' });
  assert.doesNotThrow(() => child.info('hello'));
});

test('redact masks secret-looking keys', () => {
  const out = redact({ password: 'hunter2', apiKey: 'k', username: 'IBMUSER' });
  assert.equal(out.password, '***');
  assert.equal(out.apiKey, '***');
  assert.equal(out.username, 'IBMUSER');
});

test('redact recurses into nested objects and arrays', () => {
  const out = redact({ a: { token: 'x' }, list: [{ secret: 'y' }] });
  assert.equal(out.a.token, '***');
  assert.equal(out.list[0].secret, '***');
});

test('createSilentLogger never writes', () => {
  const logger = createSilentLogger();
  assert.doesNotThrow(() => logger.info('nothing'));
  assert.equal(logger.level, 'silent');
});

test('LEVELS includes every documented level', () => {
  for (const level of ['trace', 'debug', 'info', 'warn', 'error']) {
    assert.ok(level in LEVELS);
  }
});

// --- script argument parsers -------------------------------------------------

test('db-init parseArgs understands --force and --db', () => {
  const { parseArgs } = require('../scripts/db-init');
  const opts = parseArgs(['--force', '--db', 'x.db', '--quiet']);
  assert.equal(opts.force, true);
  assert.equal(opts.db, 'x.db');
  assert.equal(opts.quiet, true);
});

test('db-query splitStatements names statements from -- name: comments', () => {
  const { splitStatements } = require('../scripts/db-query');
  const statements = splitStatements(
    '-- name: first\nSELECT 1;\n\n-- name: second\nSELECT 2;\n',
  );
  assert.equal(statements.length, 2);
  assert.equal(statements[0].name, 'first');
  assert.equal(statements[1].name, 'second');
  assert.match(statements[1].sql, /SELECT 2/);
});

test('db-query parseArgs understands --file and --name', () => {
  const { parseArgs } = require('../scripts/db-query');
  const opts = parseArgs(['--file', 'q.sql', '--name', 'foo', '--json']);
  assert.equal(opts.file, 'q.sql');
  assert.equal(opts.name, 'foo');
  assert.equal(opts.json, true);
});

test('export-spreadsheet parseArgs understands --engine and --out', () => {
  const { parseArgs } = require('../scripts/export-spreadsheet');
  const opts = parseArgs(['--engine', 'csv', '--out', 'dir']);
  assert.equal(opts.engine, 'csv');
  assert.equal(opts.out, 'dir');
});

test('import-spreadsheet parseArgs understands --replace and --no-coerce', () => {
  const { parseArgs } = require('../scripts/import-spreadsheet');
  const opts = parseArgs(['--file', 'f.csv', '--table', 'users', '--replace', '--no-coerce']);
  assert.equal(opts.file, 'f.csv');
  assert.equal(opts.table, 'users');
  assert.equal(opts.replace, true);
  assert.equal(opts.coerce, false);
});
