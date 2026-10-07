'use strict';

/**
 * Tiny, dependency-free validation helpers used by the HTTP control plane and
 * the CLI scripts. Each validator returns the coerced value or throws a
 * ValidationError with a message suitable for a 400 response.
 */

class ValidationError extends Error {
  constructor(message, field) {
    super(message);
    this.name = 'ValidationError';
    this.statusCode = 400;
    this.field = field;
  }
}

const DATASET_NAME_RE = /^[A-Z0-9$#@][A-Z0-9$#@.-]{0,43}$/;
const USERNAME_RE = /^[A-Za-z0-9$#@._-]{1,32}$/;
const JOB_NAME_RE = /^[A-Z0-9$#@][A-Z0-9$#@-]{0,7}$/;

function isPlainObject(value) {
  return value !== null && typeof value === 'object' && !Array.isArray(value);
}

function requireString(value, field, opts = {}) {
  if (value === undefined || value === null || value === '') {
    throw new ValidationError(`${field} is required`, field);
  }
  if (typeof value !== 'string') {
    throw new ValidationError(`${field} must be a string`, field);
  }
  const trimmed = opts.trim === false ? value : value.trim();
  if (opts.minLength && trimmed.length < opts.minLength) {
    throw new ValidationError(`${field} must be at least ${opts.minLength} characters`, field);
  }
  if (opts.maxLength && trimmed.length > opts.maxLength) {
    throw new ValidationError(`${field} must be at most ${opts.maxLength} characters`, field);
  }
  if (opts.pattern && !opts.pattern.test(trimmed)) {
    throw new ValidationError(`${field} has an invalid format`, field);
  }
  if (opts.enum && !opts.enum.includes(trimmed)) {
    throw new ValidationError(`${field} must be one of: ${opts.enum.join(', ')}`, field);
  }
  return trimmed;
}

function optionalString(value, field, opts = {}) {
  if (value === undefined || value === null || value === '') return opts.default ?? null;
  return requireString(value, field, opts);
}

function toInteger(value, field, opts = {}) {
  if (value === undefined || value === null || value === '') {
    if (opts.default !== undefined) return opts.default;
    throw new ValidationError(`${field} is required`, field);
  }
  const n = Number(value);
  if (!Number.isInteger(n)) {
    throw new ValidationError(`${field} must be an integer`, field);
  }
  if (opts.min !== undefined && n < opts.min) {
    throw new ValidationError(`${field} must be >= ${opts.min}`, field);
  }
  if (opts.max !== undefined && n > opts.max) {
    throw new ValidationError(`${field} must be <= ${opts.max}`, field);
  }
  return n;
}

function toBoolean(value, field, defaultValue = false) {
  if (value === undefined || value === null || value === '') return defaultValue;
  if (typeof value === 'boolean') return value;
  const text = String(value).toLowerCase();
  if (['true', '1', 'yes', 'on'].includes(text)) return true;
  if (['false', '0', 'no', 'off'].includes(text)) return false;
  throw new ValidationError(`${field} must be a boolean`, field);
}

function isDatasetName(value) {
  return typeof value === 'string' && DATASET_NAME_RE.test(value.trim().toUpperCase());
}

function requireDatasetName(value, field = 'name') {
  const name = requireString(value, field, { maxLength: 44 }).toUpperCase();
  if (!DATASET_NAME_RE.test(name)) {
    throw new ValidationError(
      `${field} must be a valid dataset name such as MF1.PROD.CUSTOMER.MASTER`,
      field,
    );
  }
  return name;
}

function requireUsername(value, field = 'username') {
  return requireString(value, field, { maxLength: 32, pattern: USERNAME_RE }).toUpperCase();
}

function requireJobName(value, field = 'jobName') {
  return requireString(value, field, { maxLength: 8, pattern: JOB_NAME_RE }).toUpperCase();
}

/**
 * Validate an object against a simple schema description:
 *   { username: { required: true, type: 'string' },
 *     priority: { type: 'integer', min: 1, max: 15 } }
 */
function validateObject(input, schema, opts = {}) {
  const source = isPlainObject(input) ? input : {};
  const out = {};
  const errors = [];

  for (const [field, rule] of Object.entries(schema)) {
    const value = source[field];
    try {
      switch (rule.type) {
        case 'integer':
          out[field] = toInteger(value, field, rule);
          break;
        case 'boolean':
          out[field] = toBoolean(value, field, rule.default);
          break;
        case 'datasetName':
          out[field] = rule.required || value ? requireDatasetName(value, field) : null;
          break;
        case 'username':
          out[field] = rule.required || value ? requireUsername(value, field) : null;
          break;
        case 'jobName':
          out[field] = rule.required || value ? requireJobName(value, field) : null;
          break;
        default:
          out[field] = rule.required
            ? requireString(value, field, rule)
            : optionalString(value, field, rule);
      }
    } catch (err) {
      errors.push(err.message);
      out[field] = undefined;
    }
  }

  if (errors.length) {
    const err = new ValidationError(errors.join('; '));
    err.errors = errors;
    throw err;
  }

  if (opts.strict) {
    const allowed = new Set(Object.keys(schema));
    for (const key of Object.keys(source)) {
      if (!allowed.has(key)) throw new ValidationError(`Unknown field: ${key}`, key);
    }
  }

  return out;
}

/** Assert a password meets the configured policy. */
function validatePassword(password, policy = {}) {
  const errors = [];
  const text = String(password ?? '');
  const minLength = policy.minLength || 8;

  if (text.length < minLength) errors.push(`must be at least ${minLength} characters`);
  if (policy.requireDigit && !/\d/.test(text)) errors.push('must contain a digit');
  if (policy.requireUpper && !/[A-Z]/.test(text)) errors.push('must contain an uppercase letter');
  if (policy.requireLower && !/[a-z]/.test(text)) errors.push('must contain a lowercase letter');
  if (policy.requireSymbol && !/[^A-Za-z0-9]/.test(text)) errors.push('must contain a symbol');

  if (errors.length) {
    const err = new ValidationError(`Password ${errors.join(', ')}`, 'password');
    err.errors = errors;
    throw err;
  }
  return text;
}

module.exports = {
  ValidationError,
  isPlainObject,
  requireString,
  optionalString,
  toInteger,
  toBoolean,
  isDatasetName,
  requireDatasetName,
  requireUsername,
  requireJobName,
  validateObject,
  validatePassword,
  DATASET_NAME_RE,
  USERNAME_RE,
  JOB_NAME_RE,
};
