'use strict';

/**
 * Envelope used by every HTTP response in the control plane.
 *
 *   { ok: true,  data: <payload>, meta: { ... } }
 *   { ok: false, error: { code, message, details } }
 */

const ERROR_CATALOG = {
  BAD_REQUEST: 400,
  VALIDATION_ERROR: 400,
  UNAUTHORIZED: 401,
  FORBIDDEN: 403,
  NOT_FOUND: 404,
  METHOD_NOT_ALLOWED: 405,
  CONFLICT: 409,
  PAYLOAD_TOO_LARGE: 413,
  UNPROCESSABLE: 422,
  INTERNAL: 500,
  NOT_IMPLEMENTED: 501,
  SERVICE_UNAVAILABLE: 503,
};

class HttpError extends Error {
  constructor(code, message, details = null) {
    super(message || code);
    this.name = 'HttpError';
    this.code = code;
    this.statusCode = ERROR_CATALOG[code] || 500;
    this.details = details;
  }

  toJSON() {
    return {
      ok: false,
      error: {
        code: this.code,
        message: this.message,
        ...(this.details ? { details: this.details } : {}),
      },
    };
  }
}

function badRequest(message, details) { return new HttpError('BAD_REQUEST', message, details); }
function validationError(message, details) { return new HttpError('VALIDATION_ERROR', message, details); }
function unauthorized(message = 'Authentication required', details) { return new HttpError('UNAUTHORIZED', message, details); }
function forbidden(message = 'Insufficient privileges', details) { return new HttpError('FORBIDDEN', message, details); }
function notFound(message = 'Resource not found', details) { return new HttpError('NOT_FOUND', message, details); }
function conflict(message, details) { return new HttpError('CONFLICT', message, details); }
function internal(message = 'Internal server error', details) { return new HttpError('INTERNAL', message, details); }

function ok(data, meta) {
  return { ok: true, data, ...(meta ? { meta } : {}) };
}

function fail(code, message, details) {
  return new HttpError(code, message, details).toJSON();
}

/** Normalize any thrown value into an HttpError. */
function toHttpError(err) {
  if (err instanceof HttpError) return err;
  if (err && err.name === 'ValidationError') {
    return new HttpError('VALIDATION_ERROR', err.message, err.field ? { field: err.field } : null);
  }
  if (err && err.name === 'ConfigError') {
    return new HttpError('INTERNAL', err.message);
  }
  if (err && err.code === 'SQLITE_CONSTRAINT_UNIQUE') {
    return new HttpError('CONFLICT', 'Unique constraint violated', { sqlMessage: err.message });
  }
  if (err && typeof err.statusCode === 'number' && err.message) {
    return new HttpError(err.code || 'INTERNAL', err.message, err.details);
  }
  return new HttpError('INTERNAL', err && err.message ? err.message : 'Internal server error');
}

module.exports = {
  ERROR_CATALOG,
  HttpError,
  badRequest,
  validationError,
  unauthorized,
  forbidden,
  notFound,
  conflict,
  internal,
  ok,
  fail,
  toHttpError,
};
