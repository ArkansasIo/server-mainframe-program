'use strict';

/**
 * Print the fully resolved configuration (with secrets masked).
 * Usage: node src/config/show.js [--json]
 */

const { loadConfig } = require('./index');

const SECRET_KEYS = ['apikeys', 'password', 'secret', 'token'];

function mask(value, key = '') {
  const lowered = key.toLowerCase();
  if (SECRET_KEYS.some((s) => lowered.includes(s))) {
    if (Array.isArray(value)) return value.map(() => '***');
    if (typeof value === 'string' && value) return '***';
  }
  if (value && typeof value === 'object' && !Array.isArray(value)) {
    const out = {};
    for (const [k, v] of Object.entries(value)) out[k] = mask(v, k);
    return out;
  }
  if (Array.isArray(value)) return value.map((v) => mask(v, key));
  return value;
}

function main() {
  const config = loadConfig();
  const clean = mask(config);

  if (process.argv.includes('--json')) {
    process.stdout.write(`${JSON.stringify(clean, null, 2)}\n`);
    return;
  }

  process.stdout.write(`Resolved configuration\n`);
  process.stdout.write(`  source file : ${config.__configFile || '(defaults only)'}\n`);
  process.stdout.write(`  project root: ${config.__root}\n`);
  if (config.__warnings && config.__warnings.length) {
    process.stdout.write(`  warnings    : ${config.__warnings.join('; ')}\n`);
  }
  process.stdout.write(`${JSON.stringify(clean, null, 2)}\n`);
}

if (require.main === module) {
  try {
    main();
  } catch (err) {
    process.stderr.write(`${err.message}\n`);
    process.exit(1);
  }
}

module.exports = { mask };
