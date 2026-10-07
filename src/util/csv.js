'use strict';

/**
 * Minimal RFC-4180-ish CSV reader/writer. Handles quoted fields, embedded
 * delimiters, embedded newlines and escaped quotes ("" -> ").
 */

function parse(text, opts = {}) {
  const delimiter = opts.delimiter || ',';
  const source = text.charCodeAt(0) === 0xfeff ? text.slice(1) : text; // strip BOM
  const rows = [];
  let row = [];
  let field = '';
  let inQuotes = false;

  for (let i = 0; i < source.length; i += 1) {
    const ch = source[i];

    if (inQuotes) {
      if (ch === '"') {
        if (source[i + 1] === '"') {
          field += '"';
          i += 1;
        } else {
          inQuotes = false;
        }
      } else {
        field += ch;
      }
      continue;
    }

    if (ch === '"') {
      inQuotes = true;
    } else if (ch === delimiter) {
      row.push(field);
      field = '';
    } else if (ch === '\r') {
      // swallow CR, LF terminates
    } else if (ch === '\n') {
      row.push(field);
      rows.push(row);
      row = [];
      field = '';
    } else {
      field += ch;
    }
  }

  // trailing field / row without a final newline
  if (field.length > 0 || row.length > 0) {
    row.push(field);
    rows.push(row);
  }

  return rows;
}

/** Parse CSV text into an array of objects using the first row as headers. */
function parseObjects(text, opts = {}) {
  const rows = parse(text, opts);
  if (!rows.length) return [];
  const headers = rows[0].map((h) => h.trim());
  return rows.slice(1)
    .filter((r) => r.some((v) => String(v).trim() !== ''))
    .map((r) => {
      const obj = {};
      headers.forEach((h, i) => { obj[h] = r[i] !== undefined ? r[i] : ''; });
      return obj;
    });
}

function escapeCell(value, delimiter) {
  const text = value === null || value === undefined ? '' : String(value);
  const needsQuotes = text.includes(delimiter) || text.includes('"') || /[\r\n]/.test(text);
  if (!needsQuotes) return text;
  return `"${text.replace(/"/g, '""')}"`;
}

/** Serialize an array of objects (or arrays) to CSV text. */
function stringify(rows, opts = {}) {
  const delimiter = opts.delimiter || ',';
  const columns = opts.columns
    || (rows && rows.length && !Array.isArray(rows[0]) ? Object.keys(rows[0]) : null);

  const out = [];
  if (opts.title) out.push(opts.title);

  if (columns && opts.header !== false) {
    out.push(columns.map((c) => escapeCell(c, delimiter)).join(delimiter));
  }

  for (const row of rows || []) {
    if (Array.isArray(row)) {
      out.push(row.map((v) => escapeCell(v, delimiter)).join(delimiter));
    } else {
      out.push(columns.map((c) => escapeCell(row[c], delimiter)).join(delimiter));
    }
  }

  return `${out.join(opts.eol || '\r\n')}\r\n`;
}

/** Coerce a CSV string cell into a number/boolean/null when it looks like one. */
function coerceCell(value) {
  if (value === null || value === undefined) return null;
  const text = String(value).trim();
  if (text === '') return null;
  if (/^(true|false)$/i.test(text)) return text.toLowerCase() === 'true';
  if (/^NULL$/i.test(text)) return null;
  if (/^-?\d+$/.test(text)) {
    const n = Number(text);
    return Number.isSafeInteger(n) ? n : text;
  }
  if (/^-?\d*\.\d+$/.test(text)) return Number(text);
  return text;
}

module.exports = { parse, parseObjects, stringify, coerceCell, escapeCell };
