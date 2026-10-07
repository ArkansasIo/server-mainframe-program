'use strict';

const fs = require('fs');
const path = require('path');

/**
 * Structured logger.
 *
 * Emits coloured text or newline-delimited JSON to the console, and always
 * appends to <logging.directory>/<logging.file>. The file is rotated once
 * maxFileBytes is exceeded.
 *
 * Levels: trace < debug < info < warn < error. "silent" disables console
 * output but never file output.
 */

const LEVELS = { trace: 10, debug: 20, info: 30, warn: 40, error: 50, silent: 100 };
const COLORS = {
  trace: '\x1b[90m',
  debug: '\x1b[36m',
  info: '\x1b[32m',
  warn: '\x1b[33m',
  error: '\x1b[31m',
  reset: '\x1b[0m',
  dim: '\x1b[2m',
};

const SECRET_KEY_RE = /(password|passwd|secret|token|apikey|api_key|authorization|credential)/i;

function redact(value, depth = 0) {
  if (depth > 6) return '[deep]';
  if (value === null || value === undefined) return value;
  if (value instanceof Error) {
    return { name: value.name, message: value.message, code: value.code, stack: value.stack?.split('\n').slice(0, 4).join('\n') };
  }
  if (Array.isArray(value)) return value.map((v) => redact(v, depth + 1));
  if (typeof value === 'object') {
    const out = {};
    for (const [k, v] of Object.entries(value)) {
      out[k] = SECRET_KEY_RE.test(k) ? '***' : redact(v, depth + 1);
    }
    return out;
  }
  return value;
}

function formatTimestamp(date = new Date()) {
  return date.toISOString().replace('T', ' ').replace('Z', '');
}

class Logger {
  constructor(opts = {}) {
    this.level = opts.level || 'info';
    this.format = opts.format || 'text';
    this.consoleEnabled = opts.console !== false;
    this.directory = opts.directory || null;
    this.file = opts.file || 'mainframe.log';
    this.maxFileBytes = opts.maxFileBytes || 5 * 1024 * 1024;
    this.colorize = opts.colorize !== false && process.stdout.isTTY === true;
    this.bound = new Map();
    this.stream = null;
    this._bytesWritten = 0;
    this._bindStream();
  }

  static fromConfig(config, overrides = {}) {
    const logging = (config && config.logging) || {};
    return new Logger({ ...logging, ...overrides });
  }

  _bindStream() {
    if (!this.directory) return;
    try {
      fs.mkdirSync(this.directory, { recursive: true });
      const file = path.join(this.directory, this.file);
      this._rotateIfNeeded(file);
      this.stream = fs.createWriteStream(file, { flags: 'a' });
      this.stream.on('error', () => { this.stream = null; });
      try {
        this._bytesWritten = fs.statSync(file).size;
      } catch {
        this._bytesWritten = 0;
      }
    } catch {
      this.stream = null;
    }
  }

  _rotateIfNeeded(file) {
    try {
      const stat = fs.statSync(file);
      if (stat.size < this.maxFileBytes) return;
      const stamp = new Date().toISOString().replace(/[:.]/g, '-');
      const rotated = `${file}.${stamp}`;
      fs.renameSync(file, rotated);
      // Retain only the five most recent rotations.
      const dir = path.dirname(file);
      const base = path.basename(file);
      const siblings = fs.readdirSync(dir)
        .filter((f) => f.startsWith(`${base}.`))
        .sort()
        .reverse();
      for (const old of siblings.slice(5)) {
        try { fs.unlinkSync(path.join(dir, old)); } catch { /* ignore */ }
      }
    } catch { /* file may not exist yet */ }
  }

  setLevel(level) {
    if (LEVELS[level] !== undefined) this.level = level;
    return this;
  }

  enabled(level) {
    return LEVELS[level] >= (LEVELS[this.level] ?? LEVELS.info);
  }

  child(bindings) {
    const child = Object.create(this);
    child.bound = new Map([...this.bound, ...Object.entries(bindings || {})]);
    return child;
  }

  _write(level, message, meta) {
    if (!this.enabled(level)) return;

    const bound = Object.fromEntries(this.bound);
    const record = {
      ts: formatTimestamp(),
      level,
      message: String(message),
      ...(Object.keys(bound).length ? bound : {}),
      ...(meta && Object.keys(meta).length ? { meta: redact(meta) } : {}),
    };

    if (this.consoleEnabled && level !== 'silent') {
      const line = this.format === 'json'
        ? JSON.stringify(record)
        : this._textLine(record);
      const stream = level === 'error' || level === 'warn' ? process.stderr : process.stdout;
      stream.write(`${line}\n`);
    }

    if (this.stream) {
      const payload = `${JSON.stringify({ ...record, ts: formatTimestamp() })}\n`;
      this._bytesWritten += Buffer.byteLength(payload);
      try {
        if (this._bytesWritten > this.maxFileBytes) {
          const file = path.join(this.directory, this.file);
          this.stream.end();
          this.stream = null;
          this._rotateIfNeeded(file);
          this._bindStream();
        }
        this.stream?.write(payload);
      } catch { /* logging must never throw */ }
    }
  }

  _textLine(record) {
    const color = this.colorize && COLORS[record.level] ? COLORS[record.level] : '';
    const reset = this.colorize ? COLORS.reset : '';
    const dim = this.colorize ? COLORS.dim : '';
    const level = record.level.toUpperCase().padEnd(5);
    const context = [];
    for (const [k, v] of Object.entries(record)) {
      if (['ts', 'level', 'message', 'meta'].includes(k)) continue;
      context.push(`${k}=${v}`);
    }
    const ctx = context.length ? ` ${dim}(${context.join(' ')})${reset}` : '';
    const meta = record.meta ? ` ${dim}${JSON.stringify(record.meta)}${reset}` : '';
    return `${dim}${record.ts}${reset} ${color}${level}${reset} ${record.message}${ctx}${meta}`;
  }

  trace(message, meta) { this._write('trace', message, meta); }
  debug(message, meta) { this._write('debug', message, meta); }
  info(message, meta) { this._write('info', message, meta); }
  warn(message, meta) { this._write('warn', message, meta); }
  error(message, meta) { this._write('error', message, meta); }

  /** Express-style middleware logging one line per request. */
  middleware() {
    return (req, res, next) => {
      const start = process.hrtime.bigint();
      res.on('finish', () => {
        const ms = Number((process.hrtime.bigint() - start) / 1000000n);
        const level = res.statusCode >= 500 ? 'error' : res.statusCode >= 400 ? 'warn' : 'info';
        this._write(level, `${req.method} ${req.originalUrl || req.url} ${res.statusCode}`, {
          ms,
          ip: req.socket?.remoteAddress,
          requestId: req.requestId,
        });
      });
      next?.();
    };
  }

  async close() {
    if (!this.stream) return;
    await new Promise((resolve) => this.stream.end(resolve));
    this.stream = null;
  }
}

/** No-op logger for unit tests. */
function createSilentLogger() {
  const noop = () => { };
  return {
    level: 'silent',
    trace: noop, debug: noop, info: noop, warn: noop, error: noop,
    child() { return this; },
    setLevel() { return this; },
    middleware() { return (req, res, next) => next?.(); },
    async close() { },
  };
}

/**
 * A ready-to-use module-level logger for the CLI scripts.
 *
 * scripts/*.js do `const logger = require('../src/core/logger')` and then
 * call logger.info(...) directly, so the module itself has to behave like a
 * logger, not just export the class. Console-only (no file) and driven by
 * MF_LOG_LEVEL so a quiet run is possible.
 */
const defaultLogger = new Logger({
  level: process.env.MF_LOG_LEVEL || 'info',
  console: true,
  directory: null,
});

// Export the logger *instance* as the module, while attaching the named
// exports so `require('./logger').Logger` still works.
module.exports = defaultLogger;
module.exports.Logger = Logger;
module.exports.LEVELS = LEVELS;
module.exports.redact = redact;
module.exports.createSilentLogger = createSilentLogger;
module.exports.formatTimestamp = formatTimestamp;
