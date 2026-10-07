'use strict';

/**
 * CSV reader/writer tests: quoting, embedded delimiters, embedded newlines,
 * escaped quotes, BOM stripping and cell coercion.
 */

const test = require('node:test');
const assert = require('node:assert/strict');

const { parse, parseObjects, stringify, coerceCell, escapeCell } = require('../src/util/csv');

test('parse splits simple rows', () => {
  const rows = parse('a,b,c\n1,2,3\n');
  assert.deepEqual(rows, [['a', 'b', 'c'], ['1', '2', '3']]);
});

test('parse handles a row without a trailing newline', () => {
  const rows = parse('a,b\n1,2');
  assert.deepEqual(rows, [['a', 'b'], ['1', '2']]);
});

test('parse keeps delimiters inside quoted fields', () => {
  const rows = parse('a,"b,c",d\n');
  assert.deepEqual(rows, [['a', 'b,c', 'd']]);
});

test('parse unescapes doubled quotes', () => {
  const rows = parse('"he said ""hi""",x\n');
  assert.deepEqual(rows, [['he said "hi"', 'x']]);
});

test('parse handles embedded newlines inside quotes', () => {
  const rows = parse('"line1\nline2",x\n');
  assert.deepEqual(rows, [['line1\nline2', 'x']]);
});

test('parse strips a UTF-8 BOM', () => {
  const rows = parse('\uFEFFa,b\n');
  assert.deepEqual(rows, [['a', 'b']]);
});

test('parseObjects uses the first row as headers', () => {
  const rows = parseObjects('name,age\nAda,36\n');
  assert.deepEqual(rows, [{ name: 'Ada', age: '36' }]);
});

test('parseObjects skips fully blank rows', () => {
  const rows = parseObjects('a,b\n1,2\n,\n');
  assert.equal(rows.length, 1);
});

test('stringify quotes only when required', () => {
  const text = stringify([{ a: 'plain', b: 'has,comma' }]);
  assert.match(text, /plain,"has,comma"/);
});

test('stringify round-trips through parse', () => {
  const original = [{ a: 'x,y', b: 'he said "hi"' }, { a: 'z', b: 'w' }];
  const rows = parseObjects(stringify(original));
  assert.deepEqual(rows, original);
});

test('stringify renders null and undefined as empty', () => {
  const text = stringify([{ a: null, b: undefined }]);
  assert.match(text, /^a,b\r\n,\r\n$/);
});

test('escapeCell only quotes when needed', () => {
  assert.equal(escapeCell('plain', ','), 'plain');
  assert.equal(escapeCell('a,b', ','), '"a,b"');
  assert.equal(escapeCell('say "hi"', ','), '"say ""hi"""');
  assert.equal(escapeCell('line\nbreak', ','), '"line\nbreak"');
});

test('coerceCell turns numeric strings into numbers', () => {
  assert.equal(coerceCell('42'), 42);
  assert.equal(coerceCell('3.5'), 3.5);
  assert.equal(coerceCell('-7'), -7);
});

test('coerceCell maps empty and NULL to null', () => {
  assert.equal(coerceCell(''), null);
  assert.equal(coerceCell('NULL'), null);
  assert.equal(coerceCell('   '), null);
});

test('coerceCell understands booleans', () => {
  assert.equal(coerceCell('true'), true);
  assert.equal(coerceCell('FALSE'), false);
});

test('coerceCell leaves words alone', () => {
  assert.equal(coerceCell('IBMUSER'), 'IBMUSER');
});
