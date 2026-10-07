'use strict';

/**
 * Fixed-width text table rendering, the way an ISPF panel or a JCL listing
 * would present it. Used by passing dataset listing, the 3270 gateway and the
 * CLI tools.
 */

const DEFAULT_WIDTH = 18;

function asText(value) {
  if (value === null || value === undefined) return '';
  if (value instanceof Date) return value.toISOString();
  if (typeof value === 'object') return JSON.stringify(value);
  return String(value);
}

function truncate(value, width) {
  const text = asText(value);
  if (text.length <= width) return text;
  if (width <= 1) return text.slice(0, width);
  return `${text.slice(0, width - 1)}…`;
}

/**
 * Render rows as a fixed-width table.
 *
 * @param {object[]} rows           array of plain objects
 * @param {object}   [opts]
 * @param {string[]} [opts.columns] explicit column order
 * @param {object}   [opts.widths]  per-column width overrides
 * @param {string}   [opts.title]   optional heading
 * @param {number}   [opts.maxWidth] total panel width (default 132, a real 3270 line)
 * @param {number}   [opts.limit]   max rows to render
 */
function renderTable(rows, opts = {}) {
  const list = Array.isArray(rows) ? rows : [];
  const columns = opts.columns && opts.columns.length
    ? opts.columns
    : (list.length ? Object.keys(list[0]) : []);

  if (!columns.length) return '(no columns)';

  const widths = {};
  for (const col of columns) {
    const explicit = opts.widths && opts.widths[col];
    if (explicit) {
      widths[col] = explicit;
      continue;
    }
    const longest = list.reduce((max, row) => Math.max(max, asText(row[col]).length), col.length);
    widths[col] = Math.min(Math.max(longest, 4), DEFAULT_WIDTH + 8);
  }

  const totalWidth = columns.reduce((sum, c) => sum + widths[c], 0) + columns.length * 3 + 1;
  const maxWidth = opts.maxWidth || 132;

  const lines = [];
  if (opts.title) {
    lines.push(opts.title);
    lines.push('-'.repeat(Math.min(opts.title.length, maxWidth)));
  }

  const pad = (value, width) => {
    const text = truncate(value, width);
    return text + ' '.repeat(Math.max(0, width - text.length));
  };

  lines.push(columns.map((c) => pad(c.toUpperCase(), widths[c])).join(' | '));
  lines.push(columns.map((c) => '-'.repeat(widths[c])).join('-+-'));

  const limit = opts.limit && opts.limit > 0 ? opts.limit : list.length;
  const shown = list.slice(0, limit);
  for (const row of shown) {
    lines.push(columns.map((c) => pad(row[c], widths[c])).join(' | '));
  }

  if (shown.length < list.length) {
    lines.push(`... ${list.length - shown.length} more row(s) not shown`);
  }
  if (totalWidth > maxWidth) {
    lines.push(`(line width ${totalWidth} exceeds ${maxWidth} - consider --columns to narrow)`);
  }

  return lines.join('\n');
}

/**
 * Render a key/value panel in the mainframe style:
 *   SYSTEM NAME . . . : MAINFRAME-1
 */
function renderPanel(pairs, opts = {}) {
  const lines = [];
  if (opts.title) {
    lines.push(opts.title);
    lines.push('-'.repeat(Math.min(opts.title.length, opts.maxWidth || 79)));
  }
  const entries = Array.isArray(pairs) ? pairs : Object.entries(pairs);
  const labelWidth = entries.reduce((max, [k]) => Math.max(max, asText(k).length), 0);
  for (const [key, value] of entries) {
    const label = asText(key).toUpperCase().padEnd(labelWidth, ' ');
    lines.push(`${label} . . : ${asText(value)}`);
  }
  return lines.join('\n');
}

module.exports = { renderTable, renderPanel, truncate, asText };
