'use strict';

/**
 * Config loader tests: deep merge, coercion, env overrides, CLI flags and
 * validation. Uses loadConfig's injectable env/argv so the real environment
 * does not leak in.
 */

const test = require('node:test');
const assert = require('node:assert/strict');
const path = require('node:path');
const fs = require('node:fs');

const {
  loadConfig,
  deepMerge,
  coerce,
  setPath,
  validate,
  envOverrides,
  cliOverrides,
  ROOT,
} = require('../src/config');

test('deepMerge replaces arrays and merges nested objects', () => {
  const base = { a: 1, b: { c: 2, d: 3 }, list: [1, 2] };
  const merged = deepMerge(base, { b: { d: 4 }, list: [9] });
  assert.equal(merged.a, 1);
  assert.equal(merged.b.c, 2);
  assert.equal(merged.b.d, 4);
  assert.deepEqual(merged.list, [9]);
});

test('deepMerge keeps the base when the override is null', () => {
  assert.equal(deepMerge({ a: 1 }, { a: null }).a, 1);
});

test('coerce understands booleans, ints, floats and lists', () => {
  assert.equal(coerce('true'), true);
  assert.equal(coerce('false'), false);
  assert.equal(coerce('42'), 42);
  assert.equal(coerce('3.5'), 3.5);
  assert.deepEqual(coerce('a,b,c'), ['a', 'b', 'c']);
  assert.equal(coerce('plain'), 'plain');
});

test('setPath writes a dotted path, creating parents', () => {
  const obj = {};
  setPath(obj, 'a.b.c', 7);
  assert.deepEqual(obj, { a: { b: { c: 7 } } });
});

test('envOverrides maps MF_* variables onto config paths', () => {
  const out = envOverrides({ MF_HTTP_PORT: '9999', MF_SYSTEM_NAME: 'TEST' });
  assert.equal(out.http.port, 9999);
  assert.equal(out.system.name, 'TEST');
});

test('envOverrides ignores empty values', () => {
  const out = envOverrides({ MF_HTTP_PORT: '' });
  assert.deepEqual(out, {});
});

test('cliOverrides parses the documented flags', () => {
  const { overrides, configPath } = cliOverrides([
    '--port', '1234',
    '--db', 'x.db',
    '--log-level', 'debug',
    '--config', 'c.json',
  ]);
  assert.equal(overrides.http.port, 1234);
  assert.equal(overrides.database.file, 'x.db');
  assert.equal(overrides.logging.level, 'debug');
  assert.equal(configPath, 'c.json');
});

test('cliOverrides --tcp-only disables http', () => {
  const { overrides } = cliOverrides(['--tcp-only']);
  assert.equal(overrides.http.enabled, false);
  assert.equal(overrides.__tcpOnly, true);
});

test('validate rejects a bad port', () => {
  const config = loadConfig({ env: {}, argv: [], overrides: {} });
  const broken = JSON.parse(JSON.stringify(config));
  broken.http.port = 70000;
  assert.throws(() => validate(broken), /http.port/);
});

test('validate rejects identical tcp ports', () => {
  const config = loadConfig({ env: {}, argv: [], overrides: {} });
  const broken = JSON.parse(JSON.stringify(config));
  broken.tcp.terminalPort = broken.tcp.dataPort;
  assert.throws(() => validate(broken), /must differ/);
});

test('validate rejects an unknown database driver', () => {
  const config = loadConfig({ env: {}, argv: [], overrides: {} });
  const broken = JSON.parse(JSON.stringify(config));
  broken.database.driver = 'oracle';
  assert.throws(() => validate(broken), /database.driver/);
});

test('loadConfig resolves filesystem paths to absolute', () => {
  const config = loadConfig({ env: {}, argv: [], overrides: {} });
  assert.ok(path.isAbsolute(config.database.file));
  assert.ok(path.isAbsolute(config.logging.directory));
  assert.ok(path.isAbsolute(config.storage.spreadsheets));
});

test('loadConfig layers programmatic overrides on top of the defaults', () => {
  const config = loadConfig({
    env: {},
    argv: [],
    overrides: { system: { name: 'OVERRIDDEN' } },
  });
  assert.equal(config.system.name, 'OVERRIDDEN');
});

test('loadConfig reads the base config from config/default.json', () => {
  assert.ok(fs.existsSync(path.join(ROOT, 'config', 'default.json')));
  const config = loadConfig({ env: {}, argv: [], overrides: {} });
  assert.ok(config.system.name);
  assert.ok(Array.isArray(config.spreadsheet.tables));
});
