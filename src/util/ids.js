'use strict';

const crypto = require('crypto');

/**
 * Monotonic-ish identifier generator. Produces short, sortable, readable ids
 * in a mainframe-ish style: <prefix>-<base36 time>-<random>.
 */
function makeId(prefix = 'id', randomBytes = 4) {
  const time = Date.now().toString(36).toUpperCase();
  const rand = crypto.randomBytes(randomBytes).toString('hex').toUpperCase();
  return `${prefix}-${time}-${rand}`;
}

const jobId = () => makeId('JOB');
const txnId = () => makeId('TXN');
const sessionId = () => makeId('SES');
const correlationId = () => makeId('COR');

/** Deterministic-ish UUID v4 (uses node crypto). */
function uuid() {
  if (typeof crypto.randomUUID === 'function') return crypto.randomUUID();
  const b = crypto.randomBytes(16);
  b[6] = (b[6] & 0x0f) | 0x40;
  b[8] = (b[8] & 0x3f) | 0x80;
  const hex = b.toString('hex');
  return `${hex.slice(0, 8)}-${hex.slice(8, 12)}-${hex.slice(12, 16)}-${hex.slice(16, 20)}-${hex.slice(20)}`;
}

module.exports = { makeId, jobId, txnId, sessionId, correlationId, uuid };
