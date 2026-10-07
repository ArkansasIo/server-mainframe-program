'use strict';

const fs = require('fs');
const path = require('path');

const ROOT = path.resolve(__dirname, '..', '..');

/**
 * Deep-merge plain objects. Arrays are replaced, not concatenated.
 */
function deepMerge(base, override) {
  if (override === undefined) return base;
  if (Array.isArray(base) || Array.isArray(override)) return override;
  if (
    base && override &&
    typeof base === 'object' && typeof override === 'object'
  ) {
    const out = { ...base };
    for (const key of Object.keys(override)) {
      out[key] = deepMerge(base[key], override[key]);
    }
    return out;
  }
  return override === null ? base : override;
}

function readJson(file) {
  const raw = fs.readFileSync(file, 'utf8');
  try {
    return JSON.parse(raw);
  } catch (err) {
    throw new Error(`Invalid JSON in config file ${file}: ${err.message}`);
  }
}

function readJsonSafe(file) {
  if (!fs.existsSync(file)) return {};
  return readJson(file);
}

/**
 * Map of environment variables -> dotted config path.
 * Add new entries here when you add new configuration keys.
 */
const ENV_MAP = {
  MF_SYSTEM_NAME: 'system.name',
  MF_SYSPLEX: 'system.sysplex',
  MF_REGION: 'system.region',
  MF_TIMEZONE: 'system.timezone',
  MF_MAX_JOBS: 'system.maxConcurrentJobs',

  MF_HTTP_ENABLED: 'http.enabled',
  MF_HTTP_HOST: 'http.host',
  MF_HTTP_PORT: 'http.port',
  MF_HTTP_BODY_LIMIT: 'http.bodyLimitBytes',
  MF_HTTP_AUTH_REQUIRED: 'http.auth.required',
  MF_API_KEYS: 'http.auth.apiKeys',

  MF_TCP_ENABLED: 'tcp.enabled',
  MF_TCP_HOST: 'tcp.host',
  MF_TCP_TERMINAL_PORT: 'tcp.terminalPort',
  MF_TCP_DATA_PORT: 'tcp.dataPort',

  MF_DB_DRIVER: 'database.driver',
  MF_DB_FILE: 'database.file',
  MF_DB_AUTO_MIGRATE: 'database.autoMigrate',
  MF_DB_AUTO_SEED: 'database.autoSeed',

  MF_SPREADSHEET_ENGINE: 'spreadsheet.engine',
  MF_STORAGE_DATASETS: 'storage.datasets',
  MF_STORAGE_SPREADSHEETS: 'storage.spreadsheets',
  MF_STORAGE_UPLOADS: 'storage.uploads',

  MF_LOG_LEVEL: 'logging.level',
  MF_LOG_FORMAT: 'logging.format',
  MF_LOG_DIRECTORY: 'logging.directory',
  MF_LOG_CONSOLE: 'logging.console',

  MF_SESSION_TTL: 'security.session.ttlMinutes',
  MF_PASSWORD_MIN_LENGTH: 'security.passwordPolicy.minLength',
};

function coerce(value) {
  if (typeof value !== 'string') return value;
  const trimmed = value.trim();
  if (trimmed === 'true') return true;
  if (trimmed === 'false') return false;
  if (trimmed === 'null') return null;
  if (/^-?\d+$/.test(trimmed)) return parseInt(trimmed, 10);
  if (/^-?\d*\.\d+$/.test(trimmed)) return parseFloat(trimmed);
  if (trimmed.startsWith('[')) {
    try { return JSON.parse(trimmed); } catch { /* fall through */ }
  }
  if (trimmed.includes(',') && !trimmed.includes(' ')) {
    return trimmed.split(',').map((s) => s.trim()).filter(Boolean);
  }
  return trimmed;
}

function setPath(obj, dottedPath, value) {
  const parts = dottedPath.split('.');
  let cursor = obj;
  for (let i = 0; i < parts.length - 1; i += 1) {
    const key = parts[i];
    if (typeof cursor[key] !== 'object' || cursor[key] === null) cursor[key] = {};
    cursor = cursor[key];
  }
  cursor[parts[parts.length - 1]] = value;
  return obj;
}

function envOverrides(env = process.env) {
  const out = {};
  for (const [envKey, configPath] of Object.entries(ENV_MAP)) {
    if (env[envKey] !== undefined && env[envKey] !== '') {
      setPath(out, configPath, coerce(env[envKey]));
    }
  }
  return out;
}

/**
 * Parse the CLI arguments relevant to configuration.
 * Returns { overrides, configPath }.
 */
function cliOverrides(argv = process.argv.slice(2)) {
  const overrides = {};
  let configPath = null;
  let tcpOnly = false;

  for (let i = 0; i < argv.length; i += 1) {
    const arg = argv[i];
    const next = () => argv[++i];
    switch (arg) {
      case '--config': configPath = next(); break;
      case '--port': setPath(overrides, 'http.port', coerce(next())); break;
      case '--host': setPath(overrides, 'http.host', next()); break;
      case '--terminal-port': setPath(overrides, 'tcp.terminalPort', coerce(next())); break;
      case '--data-port': setPath(overrides, 'tcp.dataPort', coerce(next())); break;
      case '--db': setPath(overrides, 'database.file', next()); break;
      case '--driver': setPath(overrides, 'database.driver', next()); break;
      case '--log-level': setPath(overrides, 'logging.level', next()); break;
      case '--log-format': setPath(overrides, 'logging.format', next()); break;
      case '--no-http': setPath(overrides, 'http.enabled', false); break;
      case '--no-tcp': setPath(overrides, 'tcp.enabled', false); break;
      case '--tcp-only':
        tcpOnly = true;
        setPath(overrides, 'http.enabled', false);
        break;
      case '--system-name': setPath(overrides, 'system.name', next()); break;
      default:
        break;
    }
  }

  if (tcpOnly) overrides.__tcpOnly = true;
  return { overrides, configPath };
}

/**
 * Validate the resolved configuration. Throws on fatal problems and
 * collects non-fatal warnings on the returned object.
 */
function validate(config) {
  const errors = [];
  const warnings = [];

  const isPort = (n) => Number.isInteger(n) && n >= 0 && n <= 65535;

  if (!config.system || !config.system.name) {
    errors.push('system.name is required');
  }
  if (config.http && config.http.enabled && !isPort(config.http.port)) {
    errors.push(`http.port must be an integer 0-65535 (got ${config.http.port})`);
  }
  if (config.tcp && config.tcp.enabled) {
    if (!isPort(config.tcp.terminalPort)) {
      errors.push(`tcp.terminalPort must be an integer 1-65535 (got ${config.tcp.terminalPort})`);
    }
    if (!isPort(config.tcp.dataPort)) {
      errors.push(`tcp.dataPort must be an integer 1-65535 (got ${config.tcp.dataPort})`);
    }
    if (config.tcp.terminalPort === config.tcp.dataPort) {
      errors.push('tcp.terminalPort and tcp.dataPort must differ');
    }
  }
  if (!['sqlite', 'memory'].includes(config.database.driver)) {
    errors.push(`database.driver must be "sqlite" or "memory" (got ${config.database.driver})`);
  }
  if (!['trace', 'debug', 'info', 'warn', 'error'].includes(config.logging.level)) {
    errors.push(`logging.level invalid (got ${config.logging.level})`);
  }
  if (config.http.auth.required && (!config.http.auth.apiKeys || !config.http.auth.apiKeys.length)) {
    warnings.push('http.auth.required is true but no apiKeys configured - all requests will be rejected');
  }
  if (config.security.session.ttlMinutes <= 0) {
    errors.push('security.session.ttlMinutes must be > 0');
  }

  if (errors.length) {
    const err = new Error(`Configuration invalid:\n  - ${errors.join('\n  - ')}`);
    err.name = 'ConfigError';
    err.warnings = warnings;
    throw err;
  }

  config.__warnings = warnings;
  return config;
}

/**
 * Resolve an absolute path for a config-relative location.
 */
function resolvePath(p) {
  if (!p) return p;
  return path.isAbsolute(p) ? p : path.resolve(ROOT, p);
}

/**
 * Load, merge validate configuration.
 *
 * @param {object} [opts]
 * @param {string} [opts.configPath] explicit config file to layer on top
 * @param {object} [opts.env]        environment source (defaults to process.env)
 * @param {string[]} [opts.argv]     cli arguments (defaults to process.argv)
 * @param {object} [opts.overrides]  programmatic overrides (highest priority)
 */
function loadConfig(opts = {}) {
  const argv = opts.argv || process.argv.slice(2);
  const { overrides: cli, configPath: cliConfigPath } = cliOverrides(argv);

  const defaultFile = path.join(ROOT, 'config', 'default.json');
  if (!fs.existsSync(defaultFile)) {
    throw new Error(`Missing base configuration file: ${defaultFile}`);
  }

  let config = readJson(defaultFile);

  const explicit = opts.configPath || cliConfigPath;
  if (explicit) {
    const abs = resolvePath(explicit);
    if (!fs.existsSync(abs)) {
      throw new Error(`Config file not found: ${abs}`);
    }
    config = deepMerge(config, readJson(abs));
    config.__configFile = abs;
  } else {
    const localFile = path.join(ROOT, 'config', 'local.json');
    if (fs.existsSync(localFile)) {
      config = deepMerge(config, readJsonSafe(localFile));
      config.__configFile = localFile;
    }
  }

  config = deepMerge(config, envOverrides(opts.env || process.env));
  config = deepMerge(config, cli);
  if (opts.overrides) config = deepMerge(config, opts.overrides);

  config = validate(config);

  // Absolute paths for everything that touches the filesystem.
  config.__root = ROOT;
  config.database.file = resolvePath(config.database.file);
  config.database.backup.directory = resolvePath(config.database.backup.directory);
  config.storage.datasets = resolvePath(config.storage.datasets);
  config.storage.temp = resolvePath(config.storage.temp);
  config.storage.spreadsheets = resolvePath(config.storage.spreadsheets);
  config.storage.uploads = resolvePath(config.storage.uploads);
  config.logging.directory = resolvePath(config.logging.directory);

  return config;
}

module.exports = {
  loadConfig,
  deepMerge,
  coerce,
  setPath,
  validate,
  resolvePath,
  envOverrides,
  cliOverrides,
  ENV_MAP,
  ROOT,
};
