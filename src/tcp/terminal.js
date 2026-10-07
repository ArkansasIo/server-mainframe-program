'use strict';

/**
 * The TN3270-style terminal gateway.
 *
 * A line-oriented terminal: the client connects, gets a banner and a prompt,
 * and types commands terminated by a newline. That is deliberately not a byte
 * stream - it mirrors the way a 3270 session works, where the operator submits
 * a screen at a time rather than typing into a live cursor.
 *
 * Commands are declared in a table rather than a chain of `if` tests, so the
 * HELP reply is generated from the same list that handles the command. A
 * command can never exist without appearing in HELP, and HELP can never
 * advertise something that is not wired up.
 *
 * A command:
 *   { name, aliases, usage, description, run(ctx, args) }
 *
 * `run` receives a context with the socket, the resolved config, the database
 * handle and the logger, so nothing reaches for a global.
 */

const net = require('net');

/** Columns to show for each table, chosen so a row fits an 80-column screen. */
const TABLE_COLUMNS = {
  users: ['user_id', 'username', 'full_name', 'department', 'role', 'status'],
  datasets: ['dataset_id', 'name', 'dsorg', 'recfm', 'lrecl', 'volume', 'status'],
  jobs: ['job_id', 'job_name', 'job_class', 'status', 'return_code', 'submitted_at'],
  transactions: ['txn_id', 'txn_code', 'terminal', 'status', 'rows_read', 'elapsed_ms'],
  audit_log: ['log_id', 'event_type', 'severity', 'actor', 'message'],
  volumes: ['volume_id', 'serial', 'device_type', 'capacity_mb', 'used_mb', 'status'],
};

/** Count rows in a table, tolerating one that does not exist yet. */
function safeCount(db, table) {
  try {
    return db.prepare(`SELECT COUNT(*) AS n FROM ${table}`).get().n;
  } catch {
    return 0;
  }
}

/** Pad a value to a column width for the aligned table output. */
function pad(value, width) {
  const text = value === null || value === undefined ? '' : String(value);
  return text.length > width ? `${text.slice(0, width - 1)}\u2026` : text.padEnd(width, ' ');
}

/**
 * Render rows as a fixed-width table, the way a terminal report would.
 */
function renderRows(rows, columns, widths) {
  const lines = [];
  lines.push(columns.map((c, i) => pad(c.toUpperCase(), widths[i])).join(' '));
  lines.push(widths.map((w) => '-'.repeat(w)).join(' '));

  for (const row of rows) {
    lines.push(columns.map((c, i) => pad(row[c], widths[i])).join(' '));
  }
  return lines;
}

/** Default column widths, derived from the header unless told otherwise. */
function widthsFor(columns) {
  return columns.map((c) => Math.min(Math.max(c.length, 8), 22));
}

/* --------------------------------------------------------------------------
 * Commands
 * -------------------------------------------------------------------------- */

const COMMANDS = [
  {
    name: 'HELP',
    aliases: ['?'],
    usage: 'HELP [command]',
    description: 'List the available commands',
    run(ctx, args) {
      const wanted = (args[0] || '').toUpperCase();

      if (wanted) {
        const command = findCommand(wanted);
        if (!command) {
          ctx.write(`NO SUCH COMMAND: ${wanted}\r\n`);
          return;
        }
        ctx.write(`${command.usage}\r\n  ${command.description}\r\n`);
        return;
      }

      ctx.write('COMMANDS\r\n');
      for (const command of COMMANDS) {
        ctx.write(`  ${pad(command.usage, 26)}${command.description}\r\n`);
      }
      ctx.write('\r\nCommands are case-insensitive. LOGOFF ends the session.\r\n');
    },
  },

  {
    name: 'STATUS',
    usage: 'STATUS',
    description: 'System identity and record counts',
    run(ctx) {
      const { config, db } = ctx;
      ctx.write(`SYSTEM   : ${config.system.name}\r\n`);
      ctx.write(`SYSPLEX  : ${config.system.sysplex}\r\n`);
      ctx.write(`REGION   : ${config.system.region}\r\n`);
      ctx.write(`VERSION  : ${config.system.version || '1.0.0'}\r\n`);
      ctx.write('\r\n');
      for (const table of Object.keys(TABLE_COLUMNS)) {
        ctx.write(`${pad(table.toUpperCase(), 14)}: ${safeCount(db, table)}\r\n`);
      }
      ctx.write(`\r\nUPTIME   : ${Math.round(process.uptime())} s\r\n`);
    },
  },

  {
    name: 'LIST',
    usage: 'LIST <table> [limit]',
    description: 'Show rows from a table',
    run(ctx, args) {
      const table = (args[0] || '').toLowerCase();
      if (!table) {
        ctx.write(`USAGE: LIST <table> [limit]\r\n`);
        ctx.write(`TABLES: ${Object.keys(TABLE_COLUMNS).join(', ')}\r\n`);
        return;
      }
      if (!TABLE_COLUMNS[table]) {
        ctx.write(`UNKNOWN TABLE: ${table}\r\n`);
        ctx.write(`TABLES: ${Object.keys(TABLE_COLUMNS).join(', ')}\r\n`);
        return;
      }

      const limit = Math.min(Math.max(parseInt(args[1], 10) || 20, 1), 200);
      const columns = TABLE_COLUMNS[table];

      try {
        const rows = ctx.db.prepare(`SELECT * FROM ${table} LIMIT ?`).all(limit);
        if (!rows.length) {
          ctx.write(`(no rows in ${table})\r\n`);
          return;
        }
        for (const line of renderRows(rows, columns, widthsFor(columns))) {
          ctx.write(`${line}\r\n`);
        }
        ctx.write(`\r\n${rows.length} row(s) shown.\r\n`);
      } catch (err) {
        ctx.write(`ERROR: ${err.message}\r\n`);
      }
    },
  },

  {
    name: 'FIND',
    usage: 'FIND <table> <text>',
    description: 'Search every column of a table for text',
    run(ctx, args) {
      const table = (args[0] || '').toLowerCase();
      const needle = args.slice(1).join(' ').toLowerCase();

      if (!table || !needle) {
        ctx.write('USAGE: FIND <table> <text>\r\n');
        return;
      }
      if (!TABLE_COLUMNS[table]) {
        ctx.write(`UNKNOWN TABLE: ${table}\r\n`);
        return;
      }

      const columns = TABLE_COLUMNS[table];
      try {
        const rows = ctx.db.prepare(`SELECT * FROM ${table} LIMIT 1000`).all();
        const hits = rows.filter((row) => Object.values(row)
          .some((value) => String(value ?? '').toLowerCase().includes(needle)));

        if (!hits.length) {
          ctx.write(`NO MATCHES for "${needle}" in ${table}\r\n`);
          return;
        }
        for (const line of renderRows(hits.slice(0, 30), columns, widthsFor(columns))) {
          ctx.write(`${line}\r\n`);
        }
        ctx.write(`\r\n${hits.length} match(es).\r\n`);
      } catch (err) {
        ctx.write(`ERROR: ${err.message}\r\n`);
      }
    },
  },

  {
    name: 'TABLES',
    usage: 'TABLES',
    description: 'List the tables the terminal can read',
    run(ctx) {
      for (const table of Object.keys(TABLE_COLUMNS)) {
        ctx.write(`${pad(table, 16)}${safeCount(ctx.db, table)} row(s)\r\n`);
      }
    },
  },

  {
    name: 'CONFIG',
    usage: 'CONFIG',
    description: 'Show the resolved runtime configuration',
    run(ctx) {
      const { config } = ctx;
      ctx.write(`SYSTEM.NAME          : ${config.system.name}\r\n`);
      ctx.write(`SYSTEM.SYSPLEX       : ${config.system.sysplex}\r\n`);
      ctx.write(`HTTP                 : ${config.http.enabled ? `${config.http.host}:${config.http.port}` : 'disabled'}\r\n`);
      ctx.write(`TCP TERMINAL         : ${config.tcp.enabled ? `${config.tcp.host}:${config.tcp.terminalPort}` : 'disabled'}\r\n`);
      ctx.write(`TCP DATA             : ${config.tcp.host}:${config.tcp.dataPort}\r\n`);
      ctx.write(`DATABASE.DRIVER      : ${config.database.driver}\r\n`);
      ctx.write(`DATABASE.FILE        : ${config.database.file}\r\n`);
      ctx.write(`LOGGING.LEVEL        : ${config.logging.level}\r\n`);
      if (config.__configFile) {
        ctx.write(`CONFIG FILE          : ${config.__configFile}\r\n`);
      }
    },
  },

  {
    name: 'LOGOFF',
    aliases: ['EXIT', 'QUIT', 'BYE'],
    usage: 'LOGOFF',
    description: 'End the terminal session',
    run(ctx) {
      ctx.write('GOODBYE\r\n');
      ctx.end();
    },
  },
];

/** Find a command by name or alias, case-insensitively. */
function findCommand(name) {
  const wanted = String(name || '').toUpperCase();
  return COMMANDS.find((command) => command.name === wanted
    || (command.aliases || []).includes(wanted)) || null;
}

/** Tokenise a submitted line into arguments, honouring double quotes. */
function tokenize(line) {
  const args = [];
  let current = '';
  let quoted = false;

  for (const ch of String(line)) {
    if (ch === '"') { quoted = !quoted; continue; }
    if (!quoted && /\s/.test(ch)) {
      if (current) { args.push(current); current = ''; }
      continue;
    }
    current += ch;
  }
  if (current) args.push(current);
  return args;
}

/**
 * Handle one submitted line. Exported so the behaviour can be tested without a
 * socket.
 *
 * @returns {boolean} true when the line was understood
 */
function handleLine(ctx, line) {
  const args = tokenize(line);
  if (!args.length) return false;

  const command = findCommand(args[0]);
  if (!command) {
    ctx.write(`UNKNOWN COMMAND: ${args[0]}\r\n`);
    ctx.write('TYPE HELP FOR A LIST\r\n');
    return false;
  }

  command.run(ctx, args.slice(1));
  return true;
}

/* --------------------------------------------------------------------------
 * The server
 * -------------------------------------------------------------------------- */

/**
 * Start the terminal gateway.
 *
 * @param {object} config
 * @param {object} db
 * @param {object} logger
 * @param {object} [deps]
 * @param {object} [deps.net] injectable for tests
 * @returns {object} the node net.Server
 */
function startTerminalGateway(config, db, logger, deps = {}) {
  const netModule = deps.net || net;
  const { host, terminalPort } = config.tcp;

  const server = netModule.createServer((socket) => {
    logger.debug(`[tcp] terminal connected from ${socket.remoteAddress}`);
    socket.setEncoding('utf8');

    socket.write(`${config.tcp.banner}\r\n`);
    socket.write(`${config.system.name} / ${config.system.sysplex} - READY\r\n`);
    socket.write('TYPE HELP FOR COMMANDS\r\n> ');

    // One context per connection: the buffer is per-socket state, and sharing
    // it would interleave two operators' half-typed lines.
    const ctx = {
      socket,
      config,
      db,
      logger,
      write: (text) => {
        if (!socket.destroyed) socket.write(text);
      },
      end: () => {
        if (!socket.destroyed) socket.end();
      },
    };

    let buffer = '';

    socket.on('data', (chunk) => {
      buffer += chunk;

      let index;
      while ((index = buffer.indexOf('\n')) >= 0) {
        const line = buffer.slice(0, index).replace(/\r$/, '');
        buffer = buffer.slice(index + 1);

        if (!line.trim()) {
          ctx.write('> ');
          continue;
        }

        try {
          handleLine(ctx, line);
        } catch (err) {
          logger.error(`[tcp] command "${line}" failed: ${err.message}`);
          ctx.write(`INTERNAL ERROR: ${err.message}\r\n`);
        }

        // LOGOFF has already ended the socket; do not prompt into a closed one.
        if (!socket.destroyed && !socket.writableEnded) ctx.write('> ');
      }
    });

    socket.on('error', (err) => logger.debug(`[tcp] socket error: ${err.message}`));
    socket.on('close', () => logger.debug('[tcp] terminal disconnected'));
  });

  server.listen(terminalPort, host, () => {
    logger.info(`[tcp] terminal gateway listening on ${host}:${terminalPort}`);
  });
  server.on('error', (err) => logger.error(`[tcp] ${err.message}`));

  server.commands = COMMANDS;
  return server;
}

module.exports = {
  startTerminalGateway,
  handleLine,
  tokenize,
  findCommand,
  renderRows,
  safeCount,
  COMMANDS,
  TABLE_COLUMNS,
};
