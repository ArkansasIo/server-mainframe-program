'use strict';

/**
 * API key authentication for the control plane.
 *
 * The configuration has always described this (`http.auth.required`,
 * `http.auth.apiKeys`) and the loader has always validated it - warning when
 * `required` is true with no keys - but nothing read it, so every route was
 * open. This is the enforcement.
 *
 * Design notes
 * ------------
 *  - **Off by default.** `http.auth.required` is false in the shipped config,
 *    because the console is meant to run on a trusted network. Turning it on
 *    must not silently break the dashboard, so the check is skipped entirely
 *    when `required` is false rather than being "always on but permissive".
 *
 *  - **A comparison that does not leak.** Comparing keys with `===` returns as
 *    soon as two strings differ, so the time taken reveals how many leading
 *    characters were right. That is a real, practical attack against a key.
 *    `timingSafeEqual` compares in constant time instead.
 *
 *  - **Length is leaked, and that is accepted.** `timingSafeEqual` throws on
 *    differing lengths, and a length check must happen first. Key length is
 *    not secret in the way key content is, and pretending otherwise would cost
 *    more than it is worth.
 *
 *  - **The dashboard must still load.** A browser cannot send an
 *    Authorization header on a top-level navigation. So authentication applies
 *    to `/api/*` and the dashboard itself stays public - it renders no data
 *    until its client-side fetches succeed, and those carry the key. See
 *    `public/dashboard.js`, which reads `window.MF_BOOTSTRAP.apiKey` when the
 *    server injects one.
 */

const crypto = require('crypto');

/** Header names accepted, in priority order. */
const KEY_HEADERS = ['authorization', 'x-api-key'];

/**
 * Extract the presented key from a request.
 *
 * Accepts `Authorization: Bearer <key>`, a bare `Authorization: <key>`, and
 * `X-API-Key: <key>`. Also accepts `?api_key=` in the query string, which is
 * the only way a plain browser navigation can carry a credential - it is
 * intended for local console use, and documented as such.
 *
 * @param {object} req
 * @param {URL} url
 * @returns {string|null}
 */
function extractKey(req, url) {
  for (const header of KEY_HEADERS) {
    const raw = req.headers[header];
    if (!raw) continue;

    const value = String(raw).trim();
    if (!value) continue;

    // "Bearer abc123" -> "abc123"; "abc123" -> "abc123".
    const bearer = /^bearer\s+(.+)$/i.exec(value);
    return bearer ? bearer[1].trim() : value;
  }

  if (url && url.searchParams) {
    const query = url.searchParams.get('api_key');
    if (query) return query.trim();
  }

  return null;
}

/**
 * Constant-time comparison of two strings.
 *
 * Returns false for a missing or empty candidate without comparing, so an
 * unauthenticated request cannot be used as a timing oracle at all.
 *
 * @param {string|null} candidate
 * @param {string} expected
 * @returns {boolean}
 */
function safeEqual(candidate, expected) {
  if (typeof candidate !== 'string' || candidate === '') return false;
  if (typeof expected !== 'string' || expected === '') return false;

  const a = Buffer.from(candidate, 'utf8');
  const b = Buffer.from(expected, 'utf8');

  // Length is checked first because timingSafeEqual requires equal lengths.
  // Length is not treated as secret; see the note at the top.
  if (a.length !== b.length) return false;

  return crypto.timingSafeEqual(a, b);
}

/**
 * Build the authentication policy for a configuration.
 *
 * @param {object} config resolved configuration
 * @returns {{
 *   required: boolean,
 *   keyCount: number,
 *   check: (req, url) => {ok: boolean, reason?: string},
 *   shouldProtect: (pathname: string) => boolean
 * }}
 */
function createAuthPolicy(config) {
  const auth = (config.http && config.http.auth) || {};
  const required = Boolean(auth.required);
  const keys = Array.isArray(auth.apiKeys) ? auth.apiKeys.filter(Boolean) : [];

  /**
   * Which paths require a key.
   *
   * `/api/*` is the data surface, so it is protected. The dashboard page and
   * its assets are not: a browser navigation cannot set an Authorization
   * header, and the page contains no data until its own fetches run - and
   * those are API calls that will carry the key.
   */
  function shouldProtect(pathname) {
    if (!required) return false;
    return pathname.startsWith('/api/');
  }

  function check(req, url) {
    if (!required) return { ok: true };
    if (!keys.length) {
      // required with no keys is a fatal misconfiguration, and the loader
      // warns about it. Failing closed is the only safe reading: "nobody may
      // pass" rather than "everybody may pass".
      return { ok: false, reason: 'Authentication is required but no API keys are configured' };
    }

    const presented = extractKey(req, url);
    if (presented === null) {
      return { ok: false, reason: 'Missing API key' };
    }

    // Compare against every key without short-circuiting, so the number of
    // configured keys does not change the response time.
    let matched = false;
    for (const key of keys) {
      if (safeEqual(presented, String(key))) matched = true;
    }

    return matched ? { ok: true } : { ok: false, reason: 'Invalid API key' };
  }

  return { required, keyCount: keys.length, check, shouldProtect };
}

/**
 * Wrap a request handler with the auth policy.
 *
 * Returns a function of the same shape, so it composes with the rest of the
 * request path without the routes knowing about it.
 *
 * @param {object} opts
 * @param {ReturnType<createAuthPolicy>} opts.policy
 * @param {Function} opts.onDenied called with (req, reason) for logging
 * @returns {Function} (req, res, url, next) => boolean  true when denied
 */
function createAuthGuard(opts) {
  const policy = opts.policy;
  const onDenied = opts.onDenied || (() => { });

  return function guard(req, res, url) {
    if (!policy.shouldProtect(url.pathname)) return false;

    const result = policy.check(req, url);
    if (result.ok) return false;

    onDenied(req, result.reason);
    return true;
  };
}

module.exports = {
  createAuthPolicy,
  createAuthGuard,
  extractKey,
  safeEqual,
  KEY_HEADERS,
};
