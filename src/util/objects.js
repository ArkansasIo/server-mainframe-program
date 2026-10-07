'use strict';

const fs = require('fs');
const path = require('path');

/**
 * Plain-object helpers (get/set/copy/omit) used by the config layer and the
 * repositories.
 */

function getPath(obj, dottedPath, fallback) {
  const parts = String(dottedPath).split('.');
  let cursor = obj;
  for (const part of parts) {
    if (cursor === null || cursor === undefined || typeof cursor !== 'object') return fallback;
    cursor = cursor[part];
  }
  return cursor === undefined ? fallback : cursor;
}

function setPath(obj, dottedPath, value) {
  const parts = String(dottedPath).split('.');
  let cursor = obj;
  for (let i = 0; i < parts.length - 1; i += 1) {
    const key = parts[i];
    if (typeof cursor[key] !== 'object' || cursor[key] === null) cursor[key] = {};
    cursor = cursor[key];
  }
  cursor[parts[parts.length - 1]] = value;
  return obj;
}

function pick(obj, keys) {
  const out = {};
  for (const key of keys) {
    if (obj && Object.prototype.hasOwnProperty.call(obj, key)) out[key] = obj[key];
  }
  return out;
}

function omit(obj, keys) {
  const blocked = new Set(Array.isArray(keys) ? keys : [keys]);
  const out = {};
  for (const [k, v] of Object.entries(obj || {})) {
    if (!blocked.has(k)) out[k] = v;
  }
  return out;
}

function isEmpty(value) {
  if (value === null || value === undefined) return true;
  if (Array.isArray(value) || typeof value === 'string') return value.length === 0;
  if (value instanceof Map || value instanceof Set) return value.size === 0;
  if (typeof value === 'object') return Object.keys(value).length === 0;
  return false;
}

/** Read a JSON file, returning `fallback` when missing or malformed. */
function readJsonFile(file, fallback = null) {
  try {
    return JSON.parse(fs.readFileSync(file, 'utf8'));
  } catch {
    return fallback;
  }
}

/** Write a JSON file, creating parent directories as needed. */
function writeJsonFile(file, value, { pretty = true } = {}) {
  fs.mkdirSync(path.dirname(file), { recursive: true });
  fs.writeFileSync(file, `${JSON.stringify(value, null, pretty ? 2 : 0)}\n`, 'utf8');
  return file;
}

function clamp(n, min, max) {
  return Math.min(Math.max(n, min), max);
}

function unique(values) {
  return Array.from(new Set(values));
}

function chunk(array, size) {
  const out = [];
  for (let i = 0; i < array.length; i += size) out.push(array.slice(i, i + size));
  return out;
}

module.exports = {
  getPath,
  setPath,
  pick,
  omit,
  isEmpty,
  readJsonFile,
  writeJsonFile,
  clamp,
  unique,
  chunk,
};
