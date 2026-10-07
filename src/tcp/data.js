'use strict';

/**
 * The dataset (record) transfer port.
 *
 * The terminal gateway on :3270 is for interactive commands. This is the other
 * port the README promises: a bulk record channel, for moving a dataset's
 * records in or out without tying up a terminal session.
 *
 * The protocol is deliberately line-oriented and self-describing, so it can be
 * driven with `telnet` while debugging:
 *
 *   -> HELLO
 *   <- OK MAINFRAME-1 datasets=6
 *
 *   -> LIST
 *   <- DATASET MF1.PROD.CUSTOMER.MASTER records=1200 lrecl=80
 *   <- END 6
 *
 *   -> GET MF1.PROD.CUSTOMER.MASTER 10
 *   <- RECORD 00000001 <payload>
 *   ...
 *   <- END 10
 *
 *   -> PUT MF1.TEST.NEW
 *   <- READY
 *   -> DATA hello
 *   <- STORED 1
 *   -> COMMIT
 *   <- COMMITTED 1
 *
 * A transfer is bounded: records are capped per request, and a PUT session is
 * abandoned after a timeout so an open connection cannot accumulate state
 * forever.
 */

const net = require('net');

/** Hard cap on records returned by one GET, so a request cannot exhaust memory. */
const MAX_RECORDS_PER_GET = 1000;

/** A PUT session is discarded if it goes quiet for this long. */
const SESSION_IDLE_MS = 30000;

/* --------------------------------------------------------------------------
 * Dataset access
 * -------------------------------------------------------------------------- */

/** Find a dataset row by name. Returns null when absent. */
function findDataset(db, name) {
  try {
    return db.prepare('SELECT * FROM datasets WHERE name = ?').get(name) || null;
  } catch {
    return null;
  }
}

/** Read a dataset's records in sequence order. */
function readRecords(db, datasetId, limit) {
  try {
    return db
      .prepare('SELECT * FROM dataset_records WHERE dataset_id = ? ORDER BY sequence LIMIT ?')
      .all(datasetId, limit);
  } catch {
    return [];
  }
}

/** All datasets, for the LIST command. */
function listDatasets(db) {
  try {
    return db.prepare('SELECT * FROM datasets ORDER BY name').all();
  } catch {
    return [];
  }
}

/* --------------------------------------------------------------------------
 * Records
 * -------------------------------------------------------------------------- */

/** Format one dataset record as a terminal line. */
function formatRecord(row, index) {
  // The column is `payload` - see sql/schema.sql.
  const payload = row.payload === undefined || row.payload === null ? '' : row.payload;
  return `RECORD ${String(index + 1).padStart(8, '0')} ${payload}`;
}

/* --------------------------------------------------------------------------
 * Sessions
 * -------------------------------------------------------------------------- */

/**
 * Per-connection transfer state.
 *
 * A PUT is accumulated in memory and only written on COMMIT, so an aborted
 * upload cannot leave a half-written dataset behind. That is the whole reason
 * the protocol has an explicit COMMIT rather than writing as it receives.
 */
function createSession(db, logger) {
  return {
    /** Target dataset name for an in-progress PUT, or null. */
    pending: null,
    /** Records buffered for the pending PUT. */
    buffer: [],
    /** ms timestamp of the last activity, for the idle timeout. */
    touchedAt: Date.now(),

    touch() {
      this.touchedAt = Date.now();
    },

    expired() {
      return this.pending !== null && Date.now() - this.touchedAt > SESSION_IDLE_MS;
    },

    reset() {
      this.pending = null;
      this.buffer = [];
      this.touch();
    },

    /** Write the buffered records as one dataset. */
    commit() {
      const name = this.pending;
      const records = this.buffer;

      let dataset = findDataset(db, name);
      if (!dataset) {
        db.prepare(
          'INSERT INTO datasets (name, dsorg, recfm, lrecl, volume, status) VALUES (?, ?, ?, ?, ?, ?)',
        ).run(name, 'PS', 'FB', 80, 'MFVOL1', 'AVAILABLE');
        dataset = findDataset(db, name);
      }

      let written = 0;
      for (let i = 0; i < records.length; i += 1) {
        db.prepare(
          'INSERT INTO dataset_records (dataset_id, sequence, payload) VALUES (?, ?, ?)',
        ).run(dataset.dataset_id, i + 1, records[i]);
        written += 1;
      }

      if (written) {
        db.prepare(
          'UPDATE datasets SET record_count = record_count + ?, updated_at = datetime(\'now\') WHERE dataset_id = ?',
        ).run(written, dataset.dataset_id);
      }

      logger.info(`[tcp:data] committed ${written} record(s) to ${name}`);
      this.reset();
      return { name, written };
    },
  };
}

/* --------------------------------------------------------------------------
 * Protocol
 * -------------------------------------------------------------------------- */

/**
 * Execute one command against the session. Returns the lines to send back.
 *
 * Kept free of socket handling so the protocol can be tested directly.
 *
 * @returns {{lines: string[], end?: boolean}}
 */
function execute(session, command, args, ctx) {
  const { db, config } = ctx;
  const verb = String(command || '').toUpperCase();

  switch (verb) {
    case 'HELLO': {
      const datasets = listDatasets(db).length;
      return { lines: [`OK ${config.system.name} datasets=${datasets}`] };
    }

    case 'LIST': {
      const datasets = listDatasets(db);
      const lines = datasets.map((d) => `DATASET ${d.name} records=${d.record_count} lrecl=${d.lrecl}`);
      lines.push(`END ${datasets.length}`);
      return { lines };
    }

    case 'GET': {
      const name = args[0];
      if (!name) return { lines: ['ERR usage: GET <dataset> [limit]'] };

      const dataset = findDataset(db, name);
      if (!dataset) return { lines: [`ERR no such dataset: ${name}`] };

      const limit = Math.min(Math.max(parseInt(args[1], 10) || 100, 1), MAX_RECORDS_PER_GET);
      const records = readRecords(db, dataset.dataset_id, limit);

      return {
        lines: [
          ...records.map((row, index) => formatRecord(row, index)),
          `END ${records.length}`,
        ],
      };
    }

    case 'PUT': {
      const name = args[0];
      if (!name) return { lines: ['ERR usage: PUT <dataset>'] };
      if (session.pending) {
        return { lines: [`ERR a transfer to ${session.pending} is already open - COMMIT or ABORT`] };
      }

      session.pending = name;
      session.buffer = [];
      session.touch();
      return { lines: ['READY'] };
    }

    case 'DATA': {
      if (!session.pending) return { lines: ['ERR no transfer open - send PUT first'] };
      // The payload is everything after the verb, including its spacing.
      session.buffer.push(args.join(' '));
      session.touch();
      return { lines: [`STORED ${session.buffer.length}`] };
    }

    case 'COMMIT': {
      if (!session.pending) return { lines: ['ERR no transfer open'] };
      if (!session.buffer.length) {
        session.reset();
        return { lines: ['ERR nothing buffered - transfer aborted'] };
      }
      const result = session.commit();
      return { lines: [`COMMITTED ${result.written} into ${result.name}`] };
    }

    case 'ABORT': {
      const abandoned = session.buffer.length;
      session.reset();
      return { lines: [`ABORTED ${abandoned} record(s) discarded`] };
    }

    case 'QUIT':
    case 'BYE': {
      return { lines: ['BYE'], end: true };
    }

    default:
      return { lines: [`ERR unknown command: ${verb}`, 'COMMANDS: HELLO LIST GET PUT DATA COMMIT ABORT QUIT'] };
  }
}

/**
 * Start the dataset transfer port.
 *
 * @param {object} config
 * @param {object} db
 * @param {object} logger
 * @param {object} [deps]
 * @param {object} [deps.net] injectable for tests
 * @returns {object} the node net.Server
 */
function startDataPort(config, db, logger, deps = {}) {
  const netModule = deps.net || net;
  const { host, dataPort } = config.tcp;

  const server = netModule.createServer((socket) => {
    logger.debug(`[tcp:data] transfer channel opened from ${socket.remoteAddress}`);
    socket.setEncoding('utf8');

    const session = createSession(db, logger);
    socket.write(`MFDTP/1 ${config.system.name} READY\r\n`);

    let buffer = '';

    socket.on('data', (chunk) => {
      buffer += chunk;

      // An abandoned PUT must not hold its buffer forever.
      if (session.expired()) {
        session.reset();
        socket.write('ERR transfer timed out and was discarded\r\n');
      }

      let index;
      while ((index = buffer.indexOf('\n')) >= 0) {
        const line = buffer.slice(0, index).replace(/\r$/, '');
        buffer = buffer.slice(index + 1);
        if (!line.trim()) continue;

        const verb = line.trim().split(/\s+/)[0];
        const rest = line.trim().slice(verb.length).trim();
        const args = rest ? rest.split(/\s+/) : [];

        try {
          const { lines, end } = execute(session, verb, args, { db, config });
          for (const out of lines) socket.write(`${out}\r\n`);
          if (end) { socket.end(); return; }
        } catch (err) {
          logger.error(`[tcp:data] ${verb} failed: ${err.message}`);
          socket.write(`ERR ${err.message}\r\n`);
        }
      }
    });

    socket.on('error', (err) => logger.debug(`[tcp:data] socket error: ${err.message}`));
    socket.on('close', () => logger.debug('[tcp:data] transfer channel closed'));
  });

  server.listen(dataPort, host, () => {
    logger.info(`[tcp:data] dataset transfer listening on ${host}:${dataPort}`);
  });
  server.on('error', (err) => logger.error(`[tcp:data] ${err.message}`));

  return server;
}

module.exports = {
  startDataPort,
  execute,
  createSession,
  findDataset,
  readRecords,
  listDatasets,
  formatRecord,
  MAX_RECORDS_PER_GET,
  SESSION_IDLE_MS,
};
