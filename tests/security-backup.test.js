'use strict';

/**
 * Tests for API key authentication and database backups.
 *
 * Both features existed only as configuration before this: the loader
 * validated `http.auth` and resolved `database.backup`, and nothing read
 * either. The tests below cover the behaviour that was missing, with emphasis
 * on the cases where getting it wrong is silent - fail-open auth, and a backup
 * that cannot actually be restored.
 */

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');

const auth = require('../src/http/auth');
const backup = require('../src/db/backup');

/* --------------------------------------------------------------------------
 * Auth
 * -------------------------------------------------------------------------- */

function policyFor(authConfig) {
  return auth.createAuthPolicy({ http: { auth: authConfig } });
}

/** A request with the given headers, and a URL with optional query. */
function req(headers = {}) {
  return { headers, socket: { remoteAddress: '127.0.0.1' } };
}

function urlWith(query = '') {
  return new URL(`http://localhost/api/health${query}`);
}

test('auth is off by default and does not protect anything', () => {
  const policy = policyFor({ required: false, apiKeys: ['k'] });
  assert.equal(policy.required, false);
  assert.equal(policy.shouldProtect('/api/health'), false);
  assert.equal(policy.check(req(), urlWith()).ok, true, 'no key needed');
});

test('auth protects /api/* when required', () => {
  const policy = policyFor({ required: true, apiKeys: ['secret'] });
  assert.equal(policy.shouldProtect('/api/health'), true);
  assert.equal(policy.shouldProtect('/api/rows'), true);
});

test('auth does not protect the console shell or its assets', () => {
  // A browser cannot set an Authorization header on a top-level navigation,
  // so protecting the shell would make the console unreachable rather than
  // secure. The shell serves no data; its API calls are what are protected.
  const policy = policyFor({ required: true, apiKeys: ['secret'] });
  for (const pathname of ['/', '/index.html', '/dashboard.js', '/dashboard.css',
    '/components.js', '/favicon.svg']) {
    assert.equal(policy.shouldProtect(pathname), false, `${pathname} must stay public`);
  }
});

test('a correct key in an Authorization header is accepted', () => {
  const policy = policyFor({ required: true, apiKeys: ['secret'] });
  assert.equal(policy.check(req({ authorization: 'Bearer secret' }), urlWith()).ok, true);
});

test('a bare Authorization value is accepted', () => {
  const policy = policyFor({ required: true, apiKeys: ['secret'] });
  assert.equal(policy.check(req({ authorization: 'secret' }), urlWith()).ok, true);
});

test('X-API-Key is accepted', () => {
  const policy = policyFor({ required: true, apiKeys: ['secret'] });
  assert.equal(policy.check(req({ 'x-api-key': 'secret' }), urlWith()).ok, true);
});

test('the key may come from the query string', () => {
  // The only way a plain browser navigation can carry a credential.
  const policy = policyFor({ required: true, apiKeys: ['secret'] });
  assert.equal(policy.check(req(), urlWith('?api_key=secret')).ok, true);
});

test('Bearer matching is case-insensitive', () => {
  const policy = policyFor({ required: true, apiKeys: ['secret'] });
  assert.equal(policy.check(req({ authorization: 'bearer secret' }), urlWith()).ok, true);
});

test('a wrong key is rejected', () => {
  const policy = policyFor({ required: true, apiKeys: ['secret'] });
  const result = policy.check(req({ authorization: 'Bearer nope' }), urlWith());
  assert.equal(result.ok, false);
  assert.match(result.reason, /invalid/i);
});

test('a missing key is rejected', () => {
  const policy = policyFor({ required: true, apiKeys: ['secret'] });
  const result = policy.check(req(), urlWith());
  assert.equal(result.ok, false);
  assert.match(result.reason, /missing/i);
});

test('an empty key is treated as missing, not as a match', () => {
  const policy = policyFor({ required: true, apiKeys: [''] });
  const result = policy.check(req({ authorization: '' }), urlWith());
  assert.equal(result.ok, false, 'an empty candidate must never authenticate');
});

test('any of the configured keys is accepted', () => {
  const policy = policyFor({ required: true, apiKeys: ['first', 'second', 'third'] });
  for (const key of ['first', 'second', 'third']) {
    assert.equal(policy.check(req({ authorization: `Bearer ${key}` }), urlWith()).ok, true, key);
  }
});

test('a key that is a prefix of a real key is rejected', () => {
  // The length check must run before the constant-time compare, or this
  // would throw rather than return false.
  const policy = policyFor({ required: true, apiKeys: ['secret-long-key'] });
  assert.equal(policy.check(req({ authorization: 'Bearer secret' }), urlWith()).ok, false);
});

test('required with no keys fails closed', () => {
  // The safe reading of "authentication required, no keys configured" is
  // "nobody may pass", not "everybody may pass".
  const policy = policyFor({ required: true, apiKeys: [] });
  const result = policy.check(req({ authorization: 'anything' }), urlWith());
  assert.equal(result.ok, false);
  assert.match(result.reason, /no api keys/i);
});

test('safeEqual compares exactly', () => {
  assert.equal(auth.safeEqual('abc', 'abc'), true);
  assert.equal(auth.safeEqual('abc', 'abd'), false);
  assert.equal(auth.safeEqual('abc', 'abcd'), false, 'differing lengths are false, not a throw');
  assert.equal(auth.safeEqual('', 'abc'), false);
  assert.equal(auth.safeEqual(null, 'abc'), false);
  assert.equal(auth.safeEqual(undefined, 'abc'), false);
});

test('extractKey returns null when nothing is presented', () => {
  assert.equal(auth.extractKey(req(), urlWith()), null);
  assert.equal(auth.extractKey(req({ authorization: '   ' }), urlWith()), null);
});

test('extractKey prefers the header over the query string', () => {
  const found = auth.extractKey(req({ authorization: 'Bearer from-header' }), urlWith('?api_key=from-query'));
  assert.equal(found, 'from-header');
});

test('the guard denies a protected path and passes a public one', () => {
  const policy = policyFor({ required: true, apiKeys: ['secret'] });
  const denied = [];
  const guard = auth.createAuthGuard({ policy, onDenied: (r, reason) => denied.push(reason) });

  assert.equal(guard(req(), {}, urlWith()), true, 'protected and unkeyed -> denied');
  assert.equal(denied.length, 1);

  assert.equal(guard(req(), {}, new URL('http://localhost/dashboard.js')), false,
    'assets are never denied');
});

test('the guard permits a correctly keyed request', () => {
  const policy = policyFor({ required: true, apiKeys: ['secret'] });
  const guard = auth.createAuthGuard({ policy });
  const request = req({ authorization: 'Bearer secret' });
  assert.equal(guard(request, {}, urlWith()), false, 'allowed through');
});

test('the guard is inert when auth is off', () => {
  const policy = policyFor({ required: false, apiKeys: [] });
  const guard = auth.createAuthGuard({ policy });
  assert.equal(guard(req(), {}, urlWith()), false);
});

/* --------------------------------------------------------------------------
 * Backup
 * -------------------------------------------------------------------------- */

/** A throwaway directory for one test. */
function tempDir() {
  return fs.mkdtempSync(path.join(os.tmpdir(), 'mf-backup-'));
}

/** A real SQLite database, so VACUUM INTO has something to copy. */
function makeDatabase(file, rows = 3) {
  const Database = require('better-sqlite3');
  const db = new Database(file);
  db.exec('CREATE TABLE users (user_id INTEGER PRIMARY KEY, username TEXT)');
  const insert = db.prepare('INSERT INTO users (username) VALUES (?)');
  for (let i = 0; i < rows; i += 1) insert.run(`user${i}`);
  return db;
}

test('timestamp is filename-safe and sortable', () => {
  const stamp = backup.timestamp(new Date('2026-01-14T09:30:00Z'));
  assert.equal(stamp, '2026-01-14T09-30-00');
  assert.ok(!stamp.includes(':'), 'colons are illegal in Windows filenames');
});

test('backupPath names the file after the database and the moment', () => {
  const result = backup.backupPath('D:\\b', 'D:\\data\\mainframe.db', new Date('2026-01-14T09:30:00Z'));
  assert.equal(result, path.join('D:\\b', 'mainframe-2026-01-14T09-30-00.db'));
});

test('a backup is produced and verifies as a readable database', () => {
  const dir = tempDir();
  const file = path.join(dir, 'mainframe.db');
  const db = makeDatabase(file, 5);

  const config = {
    database: {
      driver: 'sqlite',
      file,
      backup: { enabled: true, directory: path.join(dir, 'backups'), retain: 10 },
    },
  };

  const result = backup.backupDatabase({ db, config, now: new Date('2026-01-14T09:30:00Z') });
  assert.equal(result.ok, true);
  assert.ok(fs.existsSync(result.path), 'the snapshot exists');
  assert.ok(result.bytes > 0);

  const check = backup.verifyBackup(result.path);
  assert.equal(check.ok, true, check.reason);
  assert.ok(check.tables.includes('users'), 'the schema came across');

  db.close();
  fs.rmSync(dir, { recursive: true, force: true });
});

test('the backup contains the rows that were written before it', () => {
  const dir = tempDir();
  const file = path.join(dir, 'mainframe.db');
  const db = makeDatabase(file, 7);

  const config = { database: { driver: 'sqlite', file, backup: { enabled: true, directory: dir, retain: 5 } } };
  const result = backup.backupDatabase({ db, config, now: new Date('2026-01-14T10:00:00Z') });

  const Database = require('better-sqlite3');
  const restored = new Database(result.path, { readonly: true });
  const count = restored.prepare('SELECT COUNT(*) AS n FROM users').get().n;
  restored.close();

  assert.equal(count, 7, 'every row made it into the snapshot');

  db.close();
  fs.rmSync(dir, { recursive: true, force: true });
});

test('backups are skipped when disabled, unless forced', () => {
  const dir = tempDir();
  const file = path.join(dir, 'mainframe.db');
  const db = makeDatabase(file, 1);

  const config = { database: { driver: 'sqlite', file, backup: { enabled: false, directory: dir, retain: 5 } } };

  const skipped = backup.backupDatabase({ db, config });
  assert.equal(skipped.ok, false);
  assert.match(skipped.reason, /disabled/);

  const forced = backup.backupDatabase({ db, config, force: true });
  assert.equal(forced.ok, true, 'force overrides the setting');

  db.close();
  fs.rmSync(dir, { recursive: true, force: true });
});

test('the in-memory driver has nothing to back up', () => {
  const config = { database: { driver: 'memory', file: ':memory:', backup: { enabled: true, directory: '.', retain: 1 } } };
  const result = backup.backupDatabase({ db: {}, config });
  assert.equal(result.ok, false);
  assert.match(result.reason, /in-memory/);
});

test('retention keeps the newest N and deletes the rest', () => {
  const dir = tempDir();
  const file = path.join(dir, 'mainframe.db');
  const db = makeDatabase(file, 1);

  const config = { database: { driver: 'sqlite', file, backup: { enabled: true, directory: dir, retain: 3 } } };

  // Five backups, one second apart so the names sort unambiguously.
  for (let i = 0; i < 5; i += 1) {
    backup.backupDatabase({ db, config, now: new Date(Date.UTC(2026, 0, 14, 9, 30, i)) });
  }

  const remaining = backup.listBackups(dir, file);
  assert.equal(remaining.length, 3, 'pruned down to the retained count');

  // The survivors must be the newest three, not an arbitrary three.
  assert.ok(remaining.some((b) => b.name.includes('09-30-04')), 'newest kept');
  assert.ok(remaining.some((b) => b.name.includes('09-30-02')), 'third-newest kept');
  assert.ok(!remaining.some((b) => b.name.includes('09-30-00')), 'oldest pruned');

  db.close();
  fs.rmSync(dir, { recursive: true, force: true });
});

test('prune does nothing when under the limit', () => {
  const dir = tempDir();
  const file = path.join(dir, 'mainframe.db');
  fs.writeFileSync(file, '');
  for (const stamp of ['2026-01-14T09-30-00', '2026-01-14T09-30-01']) {
    fs.writeFileSync(path.join(dir, `mainframe-${stamp}.db`), 'x');
  }

  assert.deepEqual(backup.pruneBackups(dir, file, 5), []);
  assert.equal(backup.listBackups(dir, file).length, 2);
  fs.rmSync(dir, { recursive: true, force: true });
});

test('listBackups returns oldest first and ignores unrelated files', () => {
  const dir = tempDir();
  const file = path.join(dir, 'mainframe.db');

  fs.writeFileSync(path.join(dir, 'mainframe-2026-01-14T09-30-01.db'), 'b');
  fs.writeFileSync(path.join(dir, 'mainframe-2026-01-14T09-30-00.db'), 'a');
  fs.writeFileSync(path.join(dir, 'notes.txt'), 'ignore me');
  fs.writeFileSync(path.join(dir, 'other-2026-01-14T09-30-00.db'), 'ignore me too');

  const found = backup.listBackups(dir, file);
  assert.equal(found.length, 2);
  assert.ok(found[0].name.endsWith('09-30-00.db'), 'oldest first');
  assert.ok(found[1].name.endsWith('09-30-01.db'));

  fs.rmSync(dir, { recursive: true, force: true });
});

test('listBackups tolerates a directory that does not exist', () => {
  assert.deepEqual(backup.listBackups(path.join(os.tmpdir(), 'no-such-dir-mf'), 'x.db'), []);
});

test('verifyBackup rejects a file that is not a database', () => {
  const dir = tempDir();
  const file = path.join(dir, 'notadb.db');
  fs.writeFileSync(file, 'this is not sqlite');

  const result = backup.verifyBackup(file);
  assert.equal(result.ok, false);
  assert.ok(result.reason);

  fs.rmSync(dir, { recursive: true, force: true });
});

test('verifyBackup rejects a file that does not exist', () => {
  const result = backup.verifyBackup(path.join(os.tmpdir(), 'definitely-not-here.db'));
  assert.equal(result.ok, false);
  assert.match(result.reason, /no such file/);
});

test('verifyBackup rejects a database with no tables', () => {
  // An empty file is a valid SQLite database, so "it opened" is not a
  // sufficient check - it has to contain something.
  const dir = tempDir();
  const file = path.join(dir, 'empty.db');
  const Database = require('better-sqlite3');
  const db = new Database(file);
  db.close();

  const result = backup.verifyBackup(file);
  assert.equal(result.ok, false);
  assert.match(result.reason, /no tables/);

  fs.rmSync(dir, { recursive: true, force: true });
});
