'use strict';

/**
 * Response helpers for the control plane.
 *
 * Every route answers through one of these, so the headers a client sees are
 * decided in one place. That matters more than it looks: a route that builds
 * its own Content-Length, or forgets the charset, produces responses that
 * differ in ways nobody notices until something reads them as Latin-1.
 */

/** Send a JSON body with the right headers. */
function sendJson(res, status, body) {
  const payload = JSON.stringify(body, null, 2);
  res.writeHead(status, {
    'content-type': 'application/json; charset=utf-8',
    'content-length': Buffer.byteLength(payload),
    'cache-control': 'no-store',
  });
  res.end(payload);
}

/** Send an HTML body. */
function sendHtml(res, status, body) {
  res.writeHead(status, {
    'content-type': 'text/html; charset=utf-8',
    'content-length': Buffer.byteLength(body),
    'cache-control': 'no-store',
  });
  res.end(body);
}

/** Send a plain-text body. */
function sendText(res, status, body) {
  res.writeHead(status, {
    'content-type': 'text/plain; charset=utf-8',
    'content-length': Buffer.byteLength(body),
  });
  res.end(body);
}

/** A JSON error in the shape every client already parses. */
function sendError(res, status, message) {
  sendJson(res, status, { error: message, status });
}

/**
 * Escape a value for interpolation into HTML.
 *
 * Anything that reaches a template from configuration or the database goes
 * through this. The dashboard renders table names and system identity, both
 * of which are operator-controlled strings.
 */
function escapeHtml(value) {
  if (value === null || value === undefined) return '';
  return String(value)
    .replace(/&/g, '&amp;')
    .replace(/</g, '&lt;')
    .replace(/>/g, '&gt;')
    .replace(/"/g, '&quot;')
    .replace(/'/g, '&#39;');
}

module.exports = { sendJson, sendHtml, sendText, sendError, escapeHtml };
