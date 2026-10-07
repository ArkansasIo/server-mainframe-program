'use strict';

/**
 * Window manager and view registry tests.
 *
 * The window manager exists so a window can always be found again: focus moves
 * predictably, minimizing hands focus to something visible, and tiling puts
 * every window somewhere reachable. Those are the properties asserted here,
 * because a window that cannot be recovered is worse than one that never
 * opened.
 *
 * The view registry tests check the contract every view must honour - a view
 * that throws must not take the console down with it, and one that returns a
 * widget handle must still render.
 */

const test = require('node:test');
const assert = require('node:assert/strict');

const UI = require('../src/ui/components');
const Views = require('../src/ui/views');
const { fakeDocument } = require('./helpers/fake-dom');

/* --------------------------------------------------------------------------
 * Window manager
 * -------------------------------------------------------------------------- */

function wmHarness() {
  const doc = fakeDocument();
  const focused = [];
  const wm = UI.createWindowManager({
    document: doc,
    onFocusChange: (record) => focused.push(record ? record.id : null),
  });
  return { doc, wm, focused };
}

test('a new window manager has nothing open and no focus', () => {
  const { wm } = wmHarness();
  assert.equal(wm.count(), 0);
  assert.equal(wm.focused(), null);
});

test('opening a window adds it and focuses it', () => {
  const { doc, wm } = wmHarness();
  wm.open('users', 'Users', [UI.el(doc, 'p', { text: 'rows' })]);

  assert.equal(wm.count(), 1);
  assert.equal(wm.focused(), 'users');
  assert.equal(wm.has('users'), true);
});

test('opening two windows focuses the newest', () => {
  const { doc, wm } = wmHarness();
  wm.open('users', 'Users', [UI.el(doc, 'p', { text: 'a' })]);
  wm.open('jobs', 'Jobs', [UI.el(doc, 'p', { text: 'b' })]);

  assert.equal(wm.count(), 2);
  assert.equal(wm.focused(), 'jobs');
});

test('opening the same id twice raises it instead of duplicating', () => {
  const { doc, wm } = wmHarness();
  wm.open('users', 'Users', [UI.el(doc, 'p', { text: 'first' })]);
  wm.open('jobs', 'Jobs', [UI.el(doc, 'p', { text: 'b' })]);
  wm.open('users', 'Users', [UI.el(doc, 'p', { text: 'second' })]);

  assert.equal(wm.count(), 2, 'no duplicate window');
  assert.equal(wm.focused(), 'users', 'the existing window is raised');
});

test('focusing raises a window above the others', () => {
  const { doc, wm } = wmHarness();
  wm.open('a', 'A', [UI.el(doc, 'p', {})]);
  wm.open('b', 'B', [UI.el(doc, 'p', {})]);
  wm.focus('a');

  const [a, b] = ['a', 'b'].map((id) => wm.list().find((w) => w.id === id));
  assert.ok(a.z > b.z, 'the focused window has the higher z-order');
});

test('exactly one window is marked focused at a time', () => {
  const { doc, wm } = wmHarness();
  wm.open('a', 'A', [UI.el(doc, 'p', {})]);
  wm.open('b', 'B', [UI.el(doc, 'p', {})]);

  const focused = wm.list().filter((w) => w.id === wm.focused());
  assert.equal(focused.length, 1);

  const paneA = wm.get('a');
  const paneB = wm.get('b');
  assert.equal(paneB.root.classList.contains('is-focused'), true);
  assert.equal(paneA.root.classList.contains('is-focused'), false);
});

test('minimizing hands focus to a visible window', () => {
  const { doc, wm } = wmHarness();
  wm.open('a', 'A', [UI.el(doc, 'p', {})]);
  wm.open('b', 'B', [UI.el(doc, 'p', {})]);

  wm.minimize('b');
  assert.equal(wm.focused(), 'a', 'focus fell back to the visible window');
  assert.equal(wm.get('b').root.hidden, true);
  assert.equal(wm.list().find((w) => w.id === 'b').minimized, true);
});

test('minimizing the only window leaves nothing focused', () => {
  const { doc, wm } = wmHarness();
  wm.open('a', 'A', [UI.el(doc, 'p', {})]);
  wm.minimize('a');
  assert.equal(wm.focused(), null);
  assert.equal(wm.count(), 1, 'the window is still open, just hidden');
});

test('restoring a window brings it back and focuses it', () => {
  const { doc, wm } = wmHarness();
  wm.open('a', 'A', [UI.el(doc, 'p', {})]);
  wm.minimize('a');
  wm.restore('a');

  assert.equal(wm.focused(), 'a');
  assert.equal(wm.get('a').root.hidden, false);
  assert.equal(wm.list().find((w) => w.id === 'a').minimized, false);
});

test('maximizing then restoring returns the original geometry', () => {
  const { doc, wm } = wmHarness();
  wm.open('a', 'A', [UI.el(doc, 'p', {})], { x: 120, y: 140, width: 300 });

  const pane = wm.get('a');
  const before = { left: pane.root.style.left, top: pane.root.style.top };

  wm.toggleMaximize('a');
  assert.equal(wm.list().find((w) => w.id === 'a').maximized, true);
  assert.notEqual(pane.root.style.left, before.left);

  wm.toggleMaximize('a');
  assert.equal(wm.list().find((w) => w.id === 'a').maximized, false);
  assert.equal(pane.root.style.left, before.left, 'the window returns where it was');
  assert.equal(pane.root.style.top, before.top);
});

test('tiling gives every window a distinct position', () => {
  const { doc, wm } = wmHarness();
  for (const id of ['a', 'b', 'c', 'd']) wm.open(id, id.toUpperCase(), [UI.el(doc, 'p', {})]);

  assert.equal(wm.tile(), 4);

  const positions = wm.list().map((w) => {
    const pane = wm.get(w.id);
    return `${pane.root.style.left},${pane.root.style.top}`;
  });
  assert.equal(new Set(positions).size, 4, 'no two windows share a cell');
});

test('tiling un-minimizes everything', () => {
  const { doc, wm } = wmHarness();
  wm.open('a', 'A', [UI.el(doc, 'p', {})]);
  wm.open('b', 'B', [UI.el(doc, 'p', {})]);
  wm.minimize('a');

  wm.tile();
  assert.equal(wm.get('a').root.hidden, false);
  assert.equal(wm.list().every((w) => !w.minimized), true);
});

test('tiling an empty desktop is a no-op', () => {
  const { wm } = wmHarness();
  assert.equal(wm.tile(), 0);
  assert.equal(wm.cascade(), 0);
});

test('cascading staggers the windows', () => {
  const { doc, wm } = wmHarness();
  wm.open('a', 'A', [UI.el(doc, 'p', {})]);
  wm.open('b', 'B', [UI.el(doc, 'p', {})]);
  assert.equal(wm.cascade(), 2);

  const a = wm.get('a').root.style;
  const b = wm.get('b').root.style;
  assert.notEqual(`${a.left},${a.top}`, `${b.left},${b.top}`);
});

test('cycling moves focus and wraps', () => {
  const { doc, wm } = wmHarness();
  wm.open('a', 'A', [UI.el(doc, 'p', {})]);
  wm.open('b', 'B', [UI.el(doc, 'p', {})]);

  assert.equal(wm.focused(), 'b');
  wm.cycle(1);
  assert.equal(wm.focused(), 'a');
  wm.cycle(1);
  assert.equal(wm.focused(), 'b', 'wraps around');
});

test('cycling backwards moves the other way', () => {
  const { doc, wm } = wmHarness();
  wm.open('a', 'A', [UI.el(doc, 'p', {})]);
  wm.open('b', 'B', [UI.el(doc, 'p', {})]);
  assert.equal(wm.focused(), 'b');

  wm.cycle(-1);
  assert.equal(wm.focused(), 'a');
});

test('cycling needs at least two windows', () => {
  const { doc, wm } = wmHarness();
  wm.open('a', 'A', [UI.el(doc, 'p', {})]);
  assert.equal(wm.cycle(1), false);
});

test('closing a window removes it and clears focus', () => {
  const { doc, wm } = wmHarness();
  wm.open('a', 'A', [UI.el(doc, 'p', {})]);

  assert.equal(wm.close('a'), true);
  assert.equal(wm.count(), 0);
  assert.equal(wm.has('a'), false);
  assert.equal(wm.focused(), null);
});

test('closing an unknown window reports failure', () => {
  const { wm } = wmHarness();
  assert.equal(wm.close('nope'), false);
});

test('the taskbar lists every open window', () => {
  const { doc, wm } = wmHarness();
  wm.open('users', 'Users', [UI.el(doc, 'p', {})]);
  wm.open('jobs', 'Jobs', [UI.el(doc, 'p', {})]);

  const buttons = wm.taskbar.findAll((n) => n.tagName === 'BUTTON');
  assert.equal(buttons.length, 2);
  assert.deepEqual(buttons.map((b) => b.textContent), ['Users', 'Jobs']);
});

test('the taskbar marks the focused and minimized windows', () => {
  const { doc, wm } = wmHarness();
  wm.open('a', 'A', [UI.el(doc, 'p', {})]);
  wm.open('b', 'B', [UI.el(doc, 'p', {})]);
  wm.minimize('a');

  const buttons = wm.taskbar.findAll((n) => n.tagName === 'BUTTON');
  const [a, b] = buttons;
  assert.equal(a.classList.contains('is-minimized'), true);
  assert.equal(b.classList.contains('is-active'), true);
});

test('clicking the taskbar focuses a minimized window', () => {
  const { doc, wm } = wmHarness();
  wm.open('a', 'A', [UI.el(doc, 'p', {})]);
  wm.open('b', 'B', [UI.el(doc, 'p', {})]);
  wm.minimize('a');

  // The first button belongs to 'a'.
  wm.taskbar.findAll((n) => n.tagName === 'BUTTON')[0].fire('click');
  assert.equal(wm.focused(), 'a');
  assert.equal(wm.get('a').root.hidden, false);
});

test('clicking the active taskbar button minimizes it', () => {
  const { doc, wm } = wmHarness();
  wm.open('a', 'A', [UI.el(doc, 'p', {})]);

  wm.taskbar.findAll((n) => n.tagName === 'BUTTON')[0].fire('click');
  assert.equal(wm.list().find((w) => w.id === 'a').minimized, true);
});

test('closing a window through its own close button updates the manager', () => {
  const { doc, wm } = wmHarness();
  wm.open('a', 'A', [UI.el(doc, 'p', {})]);

  // The last title-bar button is Close.
  const buttons = wm.get('a').root.findAll((n) => String(n.className).includes('mf-pane-btn'));
  buttons[buttons.length - 1].fire('click');

  assert.equal(wm.has('a'), false);
  assert.equal(wm.count(), 0);
});

test('closeAll empties the desktop and reports the count', () => {
  const { doc, wm } = wmHarness();
  for (const id of ['a', 'b', 'c']) wm.open(id, id.toUpperCase(), [UI.el(doc, 'p', {})]);

  assert.equal(wm.closeAll(), 3);
  assert.equal(wm.count(), 0);
  assert.equal(wm.focused(), null);
});

test('focus changes are reported to the caller', () => {
  const { doc, wm, focused } = wmHarness();
  wm.open('a', 'A', [UI.el(doc, 'p', {})]);
  wm.open('b', 'B', [UI.el(doc, 'p', {})]);

  assert.deepEqual(focused, ['a', 'b']);
});

/* --------------------------------------------------------------------------
 * View registry
 * -------------------------------------------------------------------------- */

test('the registry has no duplicate ids', () => {
  const views = Views.createViewRegistry();
  const index = Views.indexViews(views);
  assert.equal(index.size, views.length);
});

test('every view is fully declared', () => {
  for (const view of Views.createViewRegistry()) {
    assert.ok(view.id, 'id is required');
    assert.ok(view.title, `${view.id} needs a title`);
    assert.ok(view.description, `${view.id} needs a description`);
    assert.equal(typeof view.build, 'function', `${view.id} needs a build function`);
    assert.ok(Array.isArray(view.needs), `${view.id} needs a needs array`);
  }
});

test('indexViews rejects a duplicate id', () => {
  assert.throws(
    () => Views.indexViews([{ id: 'same' }, { id: 'same' }]),
    /duplicate view id/,
  );
});

test('indexViews fills in the defaults', () => {
  const index = Views.indexViews([{ id: 'x', build: () => [] }]);
  const view = index.get('x');
  assert.equal(view.group, 'system');
  assert.deepEqual(view.needs, []);
  assert.equal(typeof view.width, 'number');
});

test('viewsInGroup filters by group', () => {
  const views = Views.createViewRegistry();
  const groups = new Set(views.map((v) => v.group));
  for (const group of groups) {
    assert.ok(Views.viewsInGroup(views, group).every((v) => v.group === group));
  }
  assert.equal(Views.viewsInGroup(views, 'nonexistent').length, 0);
});

test('requiredTables lists every table any view needs, once', () => {
  const names = Views.requiredTables(Views.createViewRegistry());
  assert.equal(new Set(names).size, names.length, 'no duplicates');
  assert.ok(names.includes('users'));
  assert.ok(names.includes('jobs'));
});

/* --------------------------------------------------------------------------
 * View building
 * -------------------------------------------------------------------------- */

/** A context with enough data for every view to render something. */
function sampleContext(overrides = {}) {
  const doc = fakeDocument();
  return Views.createViewContext({
    document: doc,
    tables: {
      users: [{ username: 'alice', full_name: 'Alice', role: 'ADMIN', status: 'ACTIVE', department: 'OPS', failed_logons: 0 }],
      jobs: [{ job_name: 'PAYROLL', job_class: 'A', status: 'ABEND', return_code: 12 }],
      datasets: [{ volume: 'MFVOL1', dsorg: 'PS', recfm: 'FB', bytes_used: 4096, record_count: 10 }],
      transactions: [{ terminal: 'L3270', txn_code: 'INQ1', status: 'OK', rows_read: 3, rows_written: 1 }],
      audit_log: [{ severity: 'ERROR', event_type: 'LOGON', actor: 'SYSTEM', message: 'bad logon', created_at: '2026-01-01' }],
    },
    health: { system: 'MAINFRAME-1', sysplex: 'SYSPLEX-A', version: '1.0.0', status: 'ok', uptimeSeconds: 120, database: { driver: 'sqlite', users: 1 } },
    settings: { scanlines: true, timestamps: true, compact: false, volume: 70 },
    bootstrap: { system: { name: 'MAINFRAME-1', region: 'DEFAULT' }, httpPort: 8080, terminalPort: 3270, generatedAt: 'now' },
    ...overrides,
  });
}

test('every registered view builds without error', () => {
  const context = sampleContext();
  for (const view of Views.createViewRegistry()) {
    const result = Views.buildView(view, context);
    assert.equal(result.error, null, `${view.id} threw: ${result.error && result.error.message}`);
    assert.ok(result.nodes.length > 0, `${view.id} produced no nodes`);
  }
});

test('every view renders a heading', () => {
  const context = sampleContext();
  for (const view of Views.createViewRegistry()) {
    const { nodes } = Views.buildView(view, context);
    const text = nodes.map((n) => n.allText).join(' ');
    assert.ok(text.trim().length > 0, `${view.id} rendered no text`);
  }
});

test('views survive being handed empty tables', () => {
  const context = sampleContext({ tables: {} });
  for (const view of Views.createViewRegistry()) {
    const result = Views.buildView(view, context);
    assert.equal(result.error, null, `${view.id} threw on empty data`);
  }
});

test('a view that throws is contained and rendered as an error', () => {
  const context = sampleContext();
  const result = Views.buildView({
    id: 'bad',
    build: () => { throw new Error('boom'); },
  }, context);

  assert.ok(result.error, 'the error is reported');
  assert.match(result.error.message, /boom/);
  assert.equal(result.nodes.length, 1, 'an error node is rendered instead');
  assert.ok(result.nodes[0].allText.includes('failed to render'));
});

test('buildView rejects a view with no build function', () => {
  const result = Views.buildView({ id: 'x' }, sampleContext());
  assert.ok(result.error);
  assert.equal(result.nodes.length, 0);
});

test('toNodes unwraps widget handles', () => {
  const doc = fakeDocument();
  const widget = UI.createDefinitionList({ document: doc, entries: [['a', 1]] });
  const nodes = Views.toNodes([widget]);

  assert.equal(nodes.length, 1);
  assert.equal(nodes[0], widget.root, 'the widget root is used, not the handle');
});

test('toNodes flattens nested arrays and drops empties', () => {
  const doc = fakeDocument();
  const node = UI.el(doc, 'p', { text: 'x' });
  const nodes = Views.toNodes([node, [null, undefined, false, [node]]]);

  assert.equal(nodes.length, 2);
});

/* --------------------------------------------------------------------------
 * Formatting helpers
 * -------------------------------------------------------------------------- */

test('num groups thousands', () => {
  assert.equal(Views.num(1234567), '1,234,567');
  assert.equal(Views.num('42'), '42');
  assert.equal(Views.num(undefined), '0');
});

test('bytes scales to the right unit', () => {
  assert.equal(Views.bytes(512), '512 B');
  assert.equal(Views.bytes(2048), '2.0 KiB');
  assert.equal(Views.bytes(5 * 1024 * 1024), '5.0 MiB');
  assert.equal(Views.bytes(3 * 1024 * 1024 * 1024), '3.00 GiB');
});

test('percent guards against a zero total', () => {
  assert.equal(Views.percent(1, 0), 0);
  assert.equal(Views.percent(1, 4), 25);
});

test('countWhere matches any of the given values', () => {
  const rows = [{ s: 'OK' }, { s: 'FAIL' }, { s: 'ABEND' }, { s: 'OK' }];
  assert.equal(Views.countWhere(rows, 's', 'OK'), 2);
  assert.equal(Views.countWhere(rows, 's', 'FAIL', 'ABEND'), 2);
  assert.equal(Views.countWhere(rows, 's', 'NOPE'), 0);
});

test('sum adds a numeric column and ignores junk', () => {
  assert.equal(Views.sum([{ n: 1 }, { n: 2 }, { n: null }, {}], 'n'), 3);
});

/* --------------------------------------------------------------------------
 * API surface
 * -------------------------------------------------------------------------- */

test('allowedTables exposes real table names, not singularised ones', () => {
  // A regression guard. The dashboard asks for a panel's declared table, and an
  // earlier version stripped a trailing "s" from the configured names:
  // USERS -> "user", which is not a table, so every request 404'd.
  const { allowedTables } = require('../src/http/server');

  const config = {
    spreadsheet: {
      tables: [
        { name: 'USERS' }, { name: 'DATASETS' }, { name: 'JOBS' },
        { name: 'TRANSACTIONS' }, { name: 'AUDIT_LOG' },
      ],
    },
  };

  const allowed = allowedTables(config);

  // Every name the dashboard actually queries must be allowed.
  for (const table of ['users', 'datasets', 'jobs', 'transactions', 'audit_log']) {
    assert.ok(allowed.includes(table), `"${table}" must be an allowed table`);
  }

  // And the singularised forms must not be presented as tables.
  assert.ok(!allowed.includes('user'), '"user" is not a table');
  assert.ok(!allowed.includes('dataset'), '"dataset" is not a table');
});

test('allowedTables rejects names that are not identifiers', () => {
  const { allowedTables } = require('../src/http/server');

  const allowed = allowedTables({
    spreadsheet: { tables: [{ name: 'bad name' }, { name: 'drop;--' }, { name: '1leading' }] },
  });

  assert.ok(!allowed.includes('bad name'));
  assert.ok(!allowed.includes('drop;--'));
  assert.ok(!allowed.includes('1leading'), 'a name may not start with a digit');
});

test('allowedTables is exposed through the entrypoint for back-compat', () => {
  // The function used to live in src/index.js; callers that imported it from
  // there must keep working after the split into src/http/.
  const entry = require('../src/index');
  assert.equal(typeof entry.allowedTables, 'function');
  assert.deepEqual(entry.allowedTables({ spreadsheet: { tables: [] } }).includes('users'), true);
});

/* --------------------------------------------------------------------------
 * The dashboard page must actually boot
 * -------------------------------------------------------------------------- */

/**
 * Load public/dashboard.js the way a browser does: after the three library
 * scripts, in one shared scope, with a document that has the methods the page
 * uses.
 *
 * This is the test that would have caught the black screen. dashboard.js read
 * `registry` from module scope before its `const` had initialised - a temporal
 * dead zone error that threw on load and left #app empty. Every unit test
 * passed, because each function was correct in isolation; the page as a whole
 * never rendered.
 */
function loadDashboardPage() {
  const fs = require('node:fs');
  const path = require('node:path');
  const vm = require('node:vm');

  const root = path.resolve(__dirname, '..');
  const doc = fakeDocument();

  const app = doc.createElement('div');
  app.id = 'app';
  doc.body.appendChild(app);
  doc.getElementById = (id) => doc.body.findAll((n) => n.id === id)[0] || null;
  doc.readyState = 'complete';

  const sandbox = {
    console, setTimeout, clearTimeout, Promise, Date, Math, JSON,
    Object, Array, Set, Map, Number, String, Boolean, Error,
    isNaN, parseInt, parseFloat, RegExp, URL, Blob,
    // The page probes the API on boot; failing fast keeps the test offline.
    fetch: () => Promise.reject(new Error('offline harness')),
    performance: { now: () => 0 },
  };
  sandbox.window = {};
  sandbox.document = doc;
  sandbox.window.document = doc;
  sandbox.window.setTimeout = setTimeout;
  sandbox.window.innerWidth = 1440;
  sandbox.window.innerHeight = 900;
  sandbox.document.defaultView = sandbox.window;
  vm.createContext(sandbox);

  const files = [
    'src/ui/components.js',
    'src/ui/keybinds.js',
    'src/ui/views.js',
    'public/dashboard.js',
  ];

  for (const file of files) {
    const source = fs.readFileSync(path.join(root, file), 'utf8');
    try {
      vm.runInContext(source, sandbox, { filename: file });
    } catch (error) {
      throw new Error(`${file} threw while loading: ${error.message}`);
    }
  }
  return { sandbox, doc, app };
}

test('the dashboard page loads without throwing', () => {
  const { sandbox } = loadDashboardPage();

  assert.equal(typeof sandbox.window.MFUI, 'object');
  assert.equal(typeof sandbox.window.MFKeybinds, 'object');
  assert.equal(typeof sandbox.window.MFViews, 'object');
  assert.equal(typeof sandbox.window.MFApp, 'object', 'the app must publish itself');
});

test('the dashboard mounts its UI instead of leaving a blank page', () => {
  const { app } = loadDashboardPage();

  // A black screen is exactly "#app has no children". Assert the opposite.
  assert.ok(app.children.length > 0, '#app must contain the console chrome');

  const text = app.allText;
  assert.ok(text.length > 0, 'the page must render text');
});

test('the dashboard wires its panels, windows and views', () => {
  const { sandbox } = loadDashboardPage();
  const app = sandbox.window.MFApp;

  assert.ok(Array.isArray(app.panels), 'panels are declared');
  assert.ok(app.panels.length >= 5, 'the console ships several panels');
  assert.ok(app.views().length >= 5, 'the view registry is populated');

  const windows = app.getWindows();
  assert.ok(windows && typeof windows.open === 'function', 'a window manager is built');

  // And every declared view must open without throwing.
  for (const id of app.views()) {
    assert.doesNotThrow(() => app.openView(id), `opening view "${id}"`);
  }
  assert.equal(windows.count(), app.views().length);
});

/* --------------------------------------------------------------------------
 * Browser load order
 * -------------------------------------------------------------------------- */

test('the UI bundle loads in a browser-shaped scope', () => {
  // The page loads these three as plain <script> tags, so they share one
  // top-level scope and have no `require` or `module`. Two real bugs shipped
  // this way: keybinds.js assigned module.exports unguarded (a ReferenceError
  // in a browser), and views.js declared `const el`, colliding with the same
  // name in components.js. This test runs the trio the way a browser would.
  const fs = require('node:fs');
  const path = require('node:path');
  const vm = require('node:vm');

  const sandbox = {
    console, setTimeout, clearTimeout, Promise, Date, Math, JSON,
    Object, Array, Set, Map, Number, String, Boolean, Error,
    isNaN, parseInt, parseFloat, RegExp,
  };
  sandbox.window = {};
  sandbox.document = fakeDocument();
  sandbox.window.document = sandbox.document;
  sandbox.window.innerWidth = 1440;
  sandbox.window.innerHeight = 900;
  sandbox.document.defaultView = sandbox.window;
  vm.createContext(sandbox);

  const root = path.resolve(__dirname, '..');
  for (const file of ['components.js', 'keybinds.js', 'views.js']) {
    const source = fs.readFileSync(path.join(root, 'src', 'ui', file), 'utf8');
    assert.doesNotThrow(
      () => vm.runInContext(source, sandbox, { filename: file }),
      `${file} must load in a browser (no require, no module) scope`,
    );
  }

  assert.equal(typeof sandbox.window.MFUI, 'object', 'components publishes MFUI');
  assert.equal(typeof sandbox.window.MFKeybinds, 'object', 'keybinds publishes MFKeybinds');
  assert.equal(typeof sandbox.window.MFViews, 'object', 'views publishes MFViews');

  // And they must actually work from those globals.
  assert.ok(sandbox.window.MFUI.createWindowManager({ document: sandbox.document }));
  assert.ok(sandbox.window.MFViews.createViewRegistry().length > 0);
  assert.ok(sandbox.window.MFKeybinds.createDefaultRegistry().size > 0);
});

test('the bundle declares no colliding top-level names', () => {
  // A cheap guard against the class of bug above: two <script> files must not
  // both declare the same top-level const/let/function.
  const fs = require('node:fs');
  const path = require('node:path');
  const root = path.resolve(__dirname, '..');

  const declared = new Map();
  for (const file of ['components.js', 'keybinds.js', 'views.js']) {
    const source = fs.readFileSync(path.join(root, 'src', 'ui', file), 'utf8');

    // Only look at column-0 declarations; anything indented is inside a
    // function or the IIFE and cannot collide.
    const pattern = /^(?:const|let|class|function)\s+([A-Za-z_$][\w$]*)/gm;
    let match;
    while ((match = pattern.exec(source)) !== null) {
      const name = match[1];
      assert.ok(
        !declared.has(name),
        `${file} redeclares "${name}", already declared in ${declared.get(name)};`
        + ' a browser shares one scope across <script> tags',
      );
      declared.set(name, file);
    }
  }
});
