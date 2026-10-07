'use strict';

/**
 * Static asset serving for the operator dashboard.
 *
 * Two roots are exposed:
 *
 *   public/        - the console's own files (dashboard.js, dashboard.css)
 *   src/ui/        - the component library, keybinds and view registry
 *
 * The second root exists because those modules are unit-tested in Node, so
 * they have to live with the source rather than being copied into public/.
 * They are exposed under bare names (/components.js) so the browsing context
 * and the test import resolve the same file.
 *
 * Path safety: a request is resolved against a root and then checked to still
 * be inside it, so `/../src/index.js` cannot read outside the asset roots. The
 * alias table is a fixed map, not a pattern, which is what keeps that check
 * meaningful.
 */

const fs = require('fs');
const path = require('path');

const CONTENT_TYPES = {
  '.html': 'text/html; charset=utf-8',
  '.js': 'text/javascript; charset=utf-8',
  '.css': 'text/css; charset=utf-8',
  '.json': 'application/json; charset=utf-8',
  '.svg': 'image/svg+xml',
  '.ico': 'image/x-icon',
  '.png': 'image/png',
  '.jpg': 'image/jpeg',
  '.webp': 'image/webp',
  '.woff2': 'font/woff2',
  '.map': 'application/json; charset=utf-8',
  '.txt': 'text/plain; charset=utf-8',
};

/** Extension -> content type, falling back to a binary type. */
function contentTypeFor(file) {
  return CONTENT_TYPES[path.extname(file).toLowerCase()] || 'application/octet-stream';
}

/**
 * Serve a file from disk. Returns true when the request was handled.
 *
 * @param {object} opts
 * @param {string} opts.file    absolute path to the file
 * @param {object} opts.req
 * @param {object} opts.res
 * @param {number} [opts.status=200]
 */
function sendFile({ file, req, res, status = 200 }) {
  const body = fs.readFileSync(file);
  res.writeHead(status, {
    'content-type': contentTypeFor(file),
    'content-length': body.length,
    // The console is edited live; a cached bundle is a stale-bug generator.
    'cache-control': 'no-cache',
  });
  res.end(req.method === 'HEAD' ? undefined : body);
  return true;
}

/**
 * Build a static handler for a set of roots.
 *
 * @param {object} opts
 * @param {string} opts.root      project root
 * @param {object} [opts.logger]
 * @returns {Function} (req, res, url) => boolean
 */
function createStaticHandler(opts) {
  const root = opts.root;
  const logger = opts.logger;
  const publicDir = path.join(root, 'public');
  const uiDir = path.join(root, 'src', 'ui');

  // Bare names for the modules that live beside the source so they stay
  // unit-testable. Fixed names only - never built from the request.
  const aliases = {
    '/components.js': path.join(uiDir, 'components.js'),
    '/keybinds.js': path.join(uiDir, 'keybinds.js'),
    '/views.js': path.join(uiDir, 'views.js'),
  };

  return function serveStatic(req, res, url) {
    const requested = url.pathname === '/' ? '/index.html' : url.pathname;

    let target = aliases[requested];
    if (!target) {
      target = path.resolve(publicDir, `.${requested}`);
      // Refuse anything that climbed out of the asset root.
      if (!target.startsWith(publicDir + path.sep)) return false;
    }

    let stat;
    try {
      stat = fs.statSync(target);
    } catch {
      return false;
    }
    if (!stat.isFile()) return false;

    if (logger) logger.debug(`[http] static ${requested}`);
    return sendFile({ file: target, req, res });
  };
}

module.exports = {
  createStaticHandler,
  sendFile,
  contentTypeFor,
  CONTENT_TYPES,
};
