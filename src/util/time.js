'use strict';

/**
 * Timestamp helpers. The database stores timestamps as SQLite-friendly
 * ISO-8601-ish strings ("YYYY-MM-DD HH:MM:SS") in UTC.
 */

function nowIso(date = new Date()) {
  return date.toISOString().replace('T', ' ').replace(/\.\d+Z$/, '');
}

function nowMs() {
  return Date.now();
}

/** SQLite datetime() expression offset by N days, e.g. -30 days. */
function sqliteOffset(days) {
  const n = Number(days);
  return `datetime('now', '${n >= 0 ? '+' : ''}${n} days')`;
}

function addMinutes(date, minutes) {
  return new Date(date.getTime() + minutes * 60 * 1000);
}

function minutesBetween(a, b = new Date()) {
  return (b.getTime() - new Date(a).getTime()) / 60000;
}

/** High-resolution stopwatch. */
function stopwatch() {
  const start = process.hrtime.bigint();
  return {
    elapsedMs() {
      return Number((process.hrtime.bigint() - start) / 1000000n);
    },
    reset() {
      return Number((process.hrtime.bigint() - start) / 1000000n);
    },
  };
}

/** Format a duration in ms as a human friendly string. */
function humanDuration(ms) {
  const n = Number(ms) || 0;
  if (n < 1000) return `${n}ms`;
  if (n < 60000) return `${(n / 1000).toFixed(2)}s`;
  if (n < 3600000) return `${(n / 60000).toFixed(2)}m`;
  return `${(n / 3600000).toFixed(2)}h`;
}

/** Sleep helper. */
function sleep(ms) {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

module.exports = {
  nowIso,
  nowMs,
  sqliteOffset,
  addMinutes,
  minutesBetween,
  stopwatch,
  humanDuration,
  sleep,
};
