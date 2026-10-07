#!/usr/bin/env node
'use strict';

/**
 * launch - the one-command launcher for the Mainframe Server System.
 *
 * It performs the whole boot sequence in order:
 *
 *   1. dependency check   - node version, better-sqlite3 bindings
 *   2. database           - create from sql/schema.sql + seed when missing
 *   3. servers            - HTTP control plane + TN3270 terminal gateway
 *   4. readiness           - wait for both ports to accept connections
 *   5. open the dashboard - optional browser hand-off (--no-open to skip)
 *
 * Usage:
 *   node scripts/launch.js
 *   node scripts/launch.js --no-open           # do not launch a browser
 *   node scripts/launch.js --tcp-only          # terminal gateway only
 *   node scripts/launch.js --db:init           # rebuild the database first
 *   node scripts/launch.js --port 9090 --log-level debug
 *
 * Configuration flags (--port, --db, --log-level, --tcp-only, --config, ...)
 * are understood by the config loader in src/config/index.js and forwarded
 * to the server unchanged.
 */

const fs = require('fs');
const path = require('path');
const net = require('net');
const { spawnSync, spawn } = require('child_process');

const ROOT = path.resolve(__dirname, '..');

// Flags that belong to the launcher itself. Everything else is passed
// through to src/index.js and interpreted by the config loader.
const LAUNCHER_FLAGS = new Set(['--no-open', '--db:init', '--rebuild', '--help', '-h']);

function parseLauncherArgs(argv) {
  const opts = { help: false, open: true, dbInit: false, passthrough: [] };

  for (const arg of argv) {
    switch (arg) {
      case '--no-open': opts.open = false; break;
      case '--db:init': opts.dbInit = true; break;
      case '--rebuild': opts.dbInit = true; break;
      case '--help':
      case '-h': opts.help = true; break;
      default: opts.passthrough.push(arg); break;
    }
  }

  return opts;
}

function usage() {
  return [
    'Mainframe Server System - launcher',
    '',
    'Usage: node scripts/launch.js [launcher flags] [config flags]',
    '',
    'Launcher flags:',
    '  --no-open       start the servers but do not open a browser',
    '  --db:init       rebuild the database from sql/schema.sql before booting',
    '  --help, -h      show this message',
    '',
    'Config flags (forwarded to the server):',
    '  --config <path> --port <n> --host <addr> --terminal-port <n>',
    '  --data-port <n> --db <path> --log-level <level> --tcp-only',
    '  --no-http --no-tcp --system-name <name>',
    '',
    'Environment variables (MF_HTTP_PORT, MF_DB_FILE, MF_LOG_LEVEL, ...)',
    'are honoured by the config loader - see src/config/index.js.',
  ].join('\n');
}

/** Print a section header in the same block style as the boot banner. */
function step(title) {
  process.stdout.write(`\n--- ${title} ${'-'.repeat(Math.max(0, 56 - title.length))}\n`);
}

function fail(message) {
  process.stderr.write(`\n[launch] ERROR: ${message}\n`);
  process.exit(1);
}

/** 1. Node version and native dependency check. */
function checkEnvironment() {
  step('environment');

  const major = Number(process.versions.node.split('.')[0]);
  if (major < 18) {
    fail(`Node.js 18 or newer is required (found ${process.versions.node}).`);
  }
  process.stdout.write(`[launch] node ${process.versions.node} OK\n`);

  const modules = path.join(ROOT, 'node_modules');
  if (!fs.existsSync(modules)) {
    fail('node_modules is missing - run "npm install" first.');
  }

  // better-sqlite3 ships a native binding; requiring it here turns an
  // obscure crash at boot into a clear message.
  try {
    require('better-sqlite3');
    process.stdout.write('[launch] better-sqlite3 OK\n');
  } catch (err) {
    fail(
      'better-sqlite3 could not be loaded - run "npm install" (or "npm rebuild better-sqlite3").\n'
      + `       ${err.message}`,
    );
  }
}

/** 2. Create / migrate / seed the database when it is not there yet. */
function prepareDatabase(passthrough, force) {
  const { loadConfig } = require('../src/config');
  const config = loadConfig({ argv: passthrough });
  const file = config.database.file;

  if (config.database.driver === 'memory') {
    process.stdout.write('[launch] in-memory database - nothing to prepare\n');
    return;
  }

  const exists = fs.existsSync(file);

  if (force && exists) {
    step('database (rebuild)');
    runDbInit(passthrough, true);
    return;
  }

  if (exists) {
    step('database');
    process.stdout.write(`[launch] existing database ${file}\n`);
    return;
  }

  step('database (first run)');
  process.stdout.write(`[launch] no database at ${file} - creating it\n`);
  runDbInit(passthrough, false);
}

function runDbInit(passthrough, force) {
  const args = [path.join(ROOT, 'scripts', 'db-init.js'), ...passthrough];
  if (force) args.push('--force');

  const result = spawnSync(process.execPath, args, { cwd: ROOT, stdio: 'inherit' });
  if (result.status !== 0) fail('database initialisation failed.');
}

/** 4. Wait until a TCP port accepts connections (or time out). */
function waitForPort(host, port, timeoutMs = 10000) {
  const address = (!host || host === '0.0.0.0') ? '127.0.0.1' : host;
  const deadline = Date.now() + timeoutMs;

  return new Promise((resolve, reject) => {
    const attempt = () => {
      const socket = net.connect({ host: address, port });
      socket.setTimeout(1000);

      socket.once('connect', () => { socket.destroy(); resolve(); });
      socket.once('timeout', () => { socket.destroy(); retry(); });
      socket.once('error', () => { socket.destroy(); retry(); });
    };

    const retry = () => {
      if (Date.now() > deadline) {
        reject(new Error(`port ${address}:${port} did not become ready in ${timeoutMs}ms`));
        return;
      }
      setTimeout(attempt, 200);
    };

    attempt();
  });
}

function openBrowser(url) {
  try {
    if (process.platform === 'win32') {
      // `start` is a cmd builtin, so it has to run through cmd.exe.
      spawn('cmd', ['/c', 'start', '', url], { detached: true, stdio: 'ignore' }).unref();
    } else if (process.platform === 'darwin') {
      spawn('open', [url], { detached: true, stdio: 'ignore' }).unref();
    } else {
      spawn('xdg-open', [url], { detached: true, stdio: 'ignore' }).unref();
    }
    process.stdout.write(`[launch] opened ${url} in your browser\n`);
  } catch {
    process.stdout.write(`[launch] could not open a browser - visit ${url}\n`);
  }
}

/**
 * 3-5. Boot the servers in-process (so the launcher owns the lifetime and
 * has the resolved config for the readiness check), then hand off.
 */
async function boot(passthrough, options) {
  // src/index.js reads the CLI flags once, at require time, through the
  // config loader - so mirror them onto process.argv before loading it.
  process.argv = [process.argv[0], path.join(ROOT, 'src', 'index.js'), ...passthrough];

  const { loadConfig } = require('../src/config');
  const config = loadConfig({ argv: passthrough });

  step('services');
  const { main } = require('../src/index');
  await main();

  const targets = [];
  if (config.http.enabled) targets.push({ name: 'http', host: config.http.host, port: config.http.port });
  if (config.tcp.enabled) {
    targets.push({ name: 'tcp', host: config.tcp.host, port: config.tcp.terminalPort });
  }

  if (targets.length) {
    step('readiness');
    const results = await Promise.allSettled(
      targets.map((t) => waitForPort(t.host, t.port)),
    );

    results.forEach((result, index) => {
      const target = targets[index];
      if (result.status === 'fulfilled') {
        process.stdout.write(`[launch] ${target.name} ready on ${target.host}:${target.port}\n`);
      } else {
        process.stdout.write(`[launch] ${target.name} NOT ready: ${result.reason.message}\n`);
        process.exitCode = 1;
      }
    });
  }

  if (config.http.enabled) {
    const address = (!config.http.host || config.http.host === '0.0.0.0')
      ? 'localhost'
      : config.http.host;
    const url = `http://${address}:${config.http.port}/`;

    process.stdout.write('\n');
    process.stdout.write('============================================================\n');
    process.stdout.write('  READY - operator dashboard: ' + url + '\n');
    if (config.tcp.enabled) {
      process.stdout.write(`  terminal gateway: telnet ${address} ${config.tcp.terminalPort}\n`);
    }
    process.stdout.write('  press Ctrl+C to stop\n');
    process.stdout.write('============================================================\n');

    if (options.open) openBrowser(url);
  }
}

async function run() {
  const opts = parseLauncherArgs(process.argv.slice(2));

  if (opts.help) {
    process.stdout.write(`${usage()}\n`);
    return;
  }

  checkEnvironment();
  prepareDatabase(opts.passthrough, opts.dbInit);
  await boot(opts.passthrough, opts);
}

if (require.main === module) {
  run().catch((err) => fail(err.message));
}

module.exports = { run, parseLauncherArgs, waitForPort, LAUNCHER_FLAGS };
