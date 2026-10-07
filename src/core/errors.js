'use strict';

/**
 * Application error vocabulary shared by core, db, http and tcp layers.
 * Every error carries a machine-readable `code` and an HTTP status where one
 * applies, so layers can translate without guessing.
 */

class MainframeError extends Error {
  constructor(message, opts = {}) {
    super(message);
    this.name = 'MainframeError';
    this.code = opts.code || 'MAINFRAME_ERROR';
    this.statusCode = opts.statusCode || 500;
    this.details = opts.details || null;
    this.cause = opts.cause || undefined;
    Error.captureStackTrace?.(this, this.constructor);
  }

  toJSON() {
    return {
      code: this.code,
      message: this.message,
      ...(this.details ? { details: this.details } : {}),
    };
  }
}

class NotFoundError extends MainframeError {
  constructor(resource, details) {
    super(`${resource} not found`, { code: 'NOT_FOUND', statusCode: 404, details });
    this.name = 'NotFoundError';
    this.resource = resource;
  }
}

class ValidationError extends MainframeError {
  constructor(message, details) {
    super(message, { code: 'VALIDATION_ERROR', statusCode: 400, details });
    this.name = 'ValidationError';
  }
}

class ConflictError extends MainframeError {
  constructor(message, details) {
    super(message, { code: 'CONFLICT', statusCode: 409, details });
    this.name = 'ConflictError';
  }
}

class AuthError extends MainframeError {
  constructor(message, details) {
    super(message, { code: 'UNAUTHORIZED', statusCode: 401, details });
    this.name = 'AuthError';
  }
}

class ForbiddenError extends MainframeError {
  constructor(message, details) {
    super(message, { code: 'FORBIDDEN', statusCode: 403, details });
    this.name = 'ForbiddenError';
  }
}

class DatabaseError extends MainframeError {
  constructor(message, cause) {
    super(message, { code: 'DATABASE_ERROR', statusCode: 500, cause });
    this.name = 'DatabaseError';
  }
}

class ConfigError extends MainframeError {
  constructor(message, details) {
    super(message, { code: 'CONFIG_ERROR', statusCode: 500, details });
    this.name = 'ConfigError';
  }
}

/** Run `fn` and wrap anything that isn't already a MainframeError. */
function wrap(fn, { message = 'Operation failed', code = 'MAINFRAME_ERROR', statusCode = 500 } = {}) {
  try {
    return fn();
  } catch (err) {
    if (err instanceof MainframeError) throw err;
    throw new MainframeError(err.message || message, { code, statusCode, cause: err });
  }
}

function isMainframeError(err) {
  return err instanceof MainframeError;
}

module.exports = {
  MainframeError,
  NotFoundError,
  ValidationError,
  ConflictError,
  AuthError,
  ForbiddenError,
  DatabaseError,
  ConfigError,
  wrap,
  isMainframeError,
};
