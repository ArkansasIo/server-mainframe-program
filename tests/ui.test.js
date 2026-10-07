'use strict';

/**
 * Interactive UI tests.
 *
 * The component library is deliberately split into pure helpers and DOM
 * factories. The pure half is tested directly here; the DOM half is exercised
 * through a tiny fake document so menu opening, keyboard routing, table
 * selection and palette ranking are covered without a browser.
 */

const test = require('node:test');
const assert = require('node:assert/strict');

const UI = require('../src/ui/components');
const KB = require('../src/ui/keybinds');

const { fakeDocument, withClass, FakeNode } = require('./helpers/fake-dom');

/* --------------------------------------------------------------------------
 * Pure helpers
 * -------------------------------------------------------------------------- */

test('buildMenuModel returns nothing while the menu is closed', () => {
  const menu = [{ title: 'File', items: [{ label: 'New' }] }];
  assert.deepEqual(UI.buildMenuModel(menu, -1), []);
});

test('buildMenuModel flattens the open menu and infers entry types', () => {
  const menu = [{
    title: 'File',
    items: [
      { label: 'New' },
      '-',
      { label: 'Recent', children: [{ label: 'a.txt' }] },
    ],
  }];

  const top = UI.buildMenuModel(menu, 0);
  assert.deepEqual(top.map((e) => `${e.depth}:${e.entry.type}`), [
    '0:item', '0:separator', '0:submenu',
  ]);

  // Drilling into the submenu exposes its children at depth 1.
  const drilled = UI.buildMenuModel(menu, 0, ['Recent']);
  const child = drilled.find((e) => e.entry.label === 'a.txt');
  assert.equal(child.depth, 1);
});

test('buildMenuModel stops at an unknown submenu id', () => {
  const menu = [{ title: 'File', items: [{ label: 'New' }] }];
  assert.equal(UI.buildMenuModel(menu, 0, ['missing']).length, 1);
});

test('filterRows matches a substring, case insensitively', () => {
  const rows = [{ user: 'ALICE' }, { user: 'bob' }];
  assert.equal(UI.filterRows(rows, ['user'], 'ali').length, 1, 'lower-case query finds upper-case data');
  assert.equal(UI.filterRows(rows, ['user'], 'ALI').length, 1, 'upper-case query finds lower-case data');
  assert.equal(UI.filterRows(rows, ['user'], 'o').length, 1);
  assert.equal(UI.filterRows(rows, ['user'], 'zzz').length, 0);
});

test('filterRows with a blank query copies every row', () => {
  const rows = [{ a: 1 }];
  const out = UI.filterRows(rows, ['a'], '   ');
  assert.deepEqual(out, rows);
  assert.notEqual(out, rows, 'should be a copy, not the same array');
});

test('filterRows __any searches across all columns', () => {
  const rows = [{ a: 'x', b: 'needle' }, { a: 'y', b: 'z' }];
  assert.equal(UI.filterRows(rows, ['__any'], 'needle').length, 1);
});

test('sortRows compares numbers numerically, not as strings', () => {
  const rows = [{ n: 10 }, { n: 2 }, { n: 30 }];
  assert.deepEqual(UI.sortRows(rows, 'n', 'asc').map((r) => r.n), [2, 10, 30]);
  assert.deepEqual(UI.sortRows(rows, 'n', 'desc').map((r) => r.n), [30, 10, 2]);
});

test('sortRows keeps blank values last in both directions', () => {
  const rows = [{ n: 2 }, { n: null }, { n: 1 }];
  assert.deepEqual(UI.sortRows(rows, 'n', 'asc').map((r) => r.n), [1, 2, null]);
  assert.deepEqual(UI.sortRows(rows, 'n', 'desc').map((r) => r.n), [2, 1, null]);
});

test('sortRows falls back to strings for mixed columns', () => {
  const rows = [{ v: 'beta' }, { v: 'Alpha' }];
  assert.deepEqual(UI.sortRows(rows, 'v', 'asc').map((r) => r.v), ['Alpha', 'beta']);
});

test('sortRows does not mutate the input array', () => {
  const rows = [{ n: 2 }, { n: 1 }];
  UI.sortRows(rows, 'n', 'asc');
  assert.deepEqual(rows.map((r) => r.n), [2, 1]);
});

test('groupCount counts distinct values', () => {
  const counts = UI.groupCount([{ s: 'A' }, { s: 'A' }, { s: 'B' }], 's');
  assert.equal(counts.get('A'), 2);
  assert.equal(counts.get('B'), 1);
});

test('rankPalette prefers direct substring matches over subsequences', () => {
  const entries = [{ label: 'Refresh panel' }, { label: 'Open dataset' }];
  assert.deepEqual(UI.rankPalette(entries, 'refresh'), [entries[0]]);
});

test('rankPalette returns the head of the list for an empty query', () => {
  const entries = Array.from({ length: 20 }, (_, i) => ({ label: `cmd ${i}` }));
  assert.equal(UI.rankPalette(entries, '').length, 12);
});

test('rankPalette drops non-matches', () => {
  assert.equal(UI.rankPalette([{ label: 'alpha' }], 'zzz').length, 0);
});

/* --------------------------------------------------------------------------
 * el()
 * -------------------------------------------------------------------------- */

test('el sets class, text, handlers and attributes', () => {
  const doc = fakeDocument();
  let clicked = 0;
  const node = UI.el(doc, 'button', {
    class: 'mf-btn', text: 'Go', type: 'button', onclick: () => { clicked += 1; },
  });

  assert.equal(node.tagName, 'BUTTON');
  assert.equal(node.className, 'mf-btn');
  assert.equal(node.textContent, 'Go');
  assert.equal(node.attributes.type, 'button');

  node.fire('click');
  assert.equal(clicked, 1);
});

test('el skips null, undefined and false attributes', () => {
  const doc = fakeDocument();
  const node = UI.el(doc, 'div', { title: null, hidden: false, 'data-x': undefined });
  assert.equal(node.attributes.title, undefined);
  assert.equal(node.attributes.hidden, undefined);
});

test('el appends child nodes and text', () => {
  const doc = fakeDocument();
  const child = UI.el(doc, 'span', { text: 'inner' });
  const node = UI.el(doc, 'div', {}, [child, 'tail']);
  assert.equal(node.children.length, 2);
  assert.equal(node.allText, 'innertail');
});

/* --------------------------------------------------------------------------
 * MenuBar
 * -------------------------------------------------------------------------- */

function sampleMenu() {
  return [
    { title: 'File', items: [{ label: 'New', action: 'file.new' }] },
    {
      title: 'View',
      items: [
        { label: 'Panels', children: [{ label: 'Users', action: 'nav.users' }] },
      ],
    },
  ];
}

test('menu bar renders one title per top-level menu', () => {
  const doc = fakeDocument();
  const bar = UI.createMenuBar({ document: doc, menu: sampleMenu(), onCommand() { } });
  assert.equal(bar.root.findAll(withClass('mf-menu-title')).length, 2);
  assert.equal(bar.isOpen(), false);
});

test('clicking a title opens the menu and renders its items', () => {
  const doc = fakeDocument();
  const bar = UI.createMenuBar({ document: doc, menu: sampleMenu(), onCommand() { } });

  bar.root.findAll(withClass('mf-menu-title'))[0].fire('click');
  assert.equal(bar.isOpen(), true);
  assert.ok(bar.root.findAll(withClass('mf-menu-item')).length >= 1);
});

test('a menu command reports its action and closes the menu', () => {
  const doc = fakeDocument();
  const seen = [];
  const bar = UI.createMenuBar({
    document: doc, menu: sampleMenu(), onCommand: (a) => seen.push(a),
  });

  bar.open(0);
  bar.root.findAll(withClass('mf-menu-item'))[0].fire('click');

  assert.deepEqual(seen, ['file.new']);
  assert.equal(bar.isOpen(), false);
});

test('clicking a submenu drills in instead of firing', () => {
  const doc = fakeDocument();
  const seen = [];
  const bar = UI.createMenuBar({
    document: doc, menu: sampleMenu(), onCommand: (a) => seen.push(a),
  });

  bar.open(1);
  bar.root.findAll(withClass('mf-menu-item'))[0].fire('click');

  // The parent did not fire; its child is now visible instead.
  assert.deepEqual(seen, []);
  assert.deepEqual(bar.getState().path, ['Panels']);
  assert.ok(bar.root.findAll(withClass('mf-menu-item'))
    .some((n) => n.allText.includes('Users')));
});

test('ArrowRight enters a submenu and ArrowLeft backs out', () => {
  const doc = fakeDocument();
  const bar = UI.createMenuBar({ document: doc, menu: sampleMenu(), onCommand() { } });

  bar.open(1);
  assert.equal(bar.handleKey({ key: 'ArrowRight' }), true);
  assert.deepEqual(bar.getState().path, ['Panels']);

  assert.equal(bar.handleKey({ key: 'ArrowLeft' }), true);
  assert.deepEqual(bar.getState().path, []);
});

test('Escape closes the menu and further keys are ignored', () => {
  const doc = fakeDocument();
  const bar = UI.createMenuBar({ document: doc, menu: sampleMenu(), onCommand() { } });

  bar.open(0);
  assert.equal(bar.handleKey({ key: 'Escape' }), true);
  assert.equal(bar.isOpen(), false);
  assert.equal(bar.handleKey({ key: 'ArrowDown' }), false);
});

test('Enter activates the highlighted entry', () => {
  const doc = fakeDocument();
  const seen = [];
  const bar = UI.createMenuBar({
    document: doc, menu: sampleMenu(), onCommand: (a) => seen.push(a),
  });

  bar.open(0);
  bar.handleKey({ key: 'Enter' });
  assert.deepEqual(seen, ['file.new']);
});

/* --------------------------------------------------------------------------
 * TableView
 * -------------------------------------------------------------------------- */

function sampleTable(opts = {}) {
  const doc = fakeDocument();
  return {
    doc,
    table: UI.createTableView({
      document: doc,
      columns: ['n', 'name'],
      rows: [{ n: 2, name: 'b' }, { n: 1, name: 'a' }, { n: 3, name: 'c' }],
      ...opts,
    }),
  };
}

test('table renders a header cell per column', () => {
  const { table } = sampleTable();
  const headers = table.root.findAll((n) => n.tagName === 'TH');
  assert.deepEqual(headers.map((h) => h.textContent.trim()), ['n', 'name']);
});

test('table renders one row per data row', () => {
  const { table } = sampleTable();
  assert.equal(table.root.findAll((n) => n.tagName === 'TR').length, 4); // + header
});

test('clicking a header sorts and marks the column', () => {
  const { table } = sampleTable();
  const header = table.root.findAll((n) => n.tagName === 'TH')[0];

  header.fire('click');
  assert.equal(table.getState().sortColumn, 'n');
  assert.equal(table.getState().sortDirection, 'asc');

  header.fire('click');
  assert.equal(table.getState().sortDirection, 'desc');
});

test('setQuery filters the visible rows and rowCount follows', () => {
  const { table } = sampleTable();
  assert.equal(table.rowCount(), 3);
  table.setQuery('a');
  assert.equal(table.rowCount(), 1);
});

test('move clamps at both ends and selects a row', () => {
  const { table } = sampleTable();
  table.move(-1);
  assert.equal(table.getSelection().name, 'b'); // clamped to the first row
  table.moveTo('last');
  assert.equal(table.getSelection().name, 'c');
  table.move(1);
  assert.equal(table.getSelection().name, 'c'); // still clamped
});

test('openSelected falls back to the first row when nothing is highlighted', () => {
  const seen = [];
  const { table } = sampleTable({ onSelect: (row) => seen.push(row) });
  table.openSelected();
  assert.deepEqual(seen, [{ n: 2, name: 'b' }]);
});

test('openSelected is a no-op on an empty table', () => {
  const seen = [];
  const { table } = sampleTable({ rows: [], onSelect: (row) => seen.push(row) });
  table.openSelected();
  assert.deepEqual(seen, []);
});

test('openSelected reports the highlighted row', () => {
  const seen = [];
  const { table } = sampleTable({ onSelect: (row) => seen.push(row) });
  table.moveTo('first');
  seen.length = 0; // moveTo already announced that selection
  table.openSelected();
  assert.deepEqual(seen, [{ n: 2, name: 'b' }]);
});

/* --------------------------------------------------------------------------
 * Tabs
 * -------------------------------------------------------------------------- */

test('tabs start on the first tab and report changes', () => {
  const doc = fakeDocument();
  const seen = [];
  const tabs = UI.createTabs({
    document: doc,
    tabs: [{ id: 'a', label: 'A' }, { id: 'b', label: 'B' }],
    onChange: (id) => seen.push(id),
  });

  assert.equal(tabs.active(), 'a');
  tabs.next();
  assert.equal(tabs.active(), 'b');
  tabs.next();
  assert.equal(tabs.active(), 'a', 'wraps around');
  assert.deepEqual(seen, ['b', 'a']);
});

test('clicking a tab selects it', () => {
  const doc = fakeDocument();
  const tabs = UI.createTabs({
    document: doc,
    tabs: [{ id: 'a', label: 'A' }, { id: 'b', label: 'B' }],
    onChange() { },
  });
  tabs.root.findAll(withClass('mf-tab'))[1].fire('click');
  assert.equal(tabs.active(), 'b');
});

/* --------------------------------------------------------------------------
 * Dialog and toasts
 * -------------------------------------------------------------------------- */

test('dialog is hidden until opened', () => {
  const doc = fakeDocument();
  const dialog = UI.createDialog({ document: doc, title: 't', message: 'm' });
  assert.equal(dialog.isOpen(), false);
  dialog.open();
  assert.equal(dialog.isOpen(), true);
});

test('dialog Enter confirms and Escape cancels', async () => {
  const doc = fakeDocument();

  const confirmed = UI.createDialog({ document: doc, title: 't' });
  const pending = confirmed.open();
  confirmed.handleKey({ key: 'Enter' });
  assert.equal(await pending, true);

  const cancelled = UI.createDialog({ document: doc, title: 't' });
  const pending2 = cancelled.open();
  cancelled.handleKey({ key: 'Escape' });
  assert.equal(await pending2, false);
});

test('dialog prompt returns the typed value', async () => {
  const doc = fakeDocument();
  const dialog = UI.createDialog({ document: doc, title: 'Find', prompt: 'seed' });
  const pending = dialog.open();
  dialog.handleKey({ key: 'Enter' });
  assert.equal(await pending, 'seed');
});

test('dialog ignores keys while hidden', () => {
  const doc = fakeDocument();
  const dialog = UI.createDialog({ document: doc, title: 't' });
  assert.equal(dialog.handleKey({ key: 'Enter' }), false);
});

test('toast host appends and removes notifications', () => {
  const doc = fakeDocument();
  const toasts = UI.createToastHost({ document: doc });
  const remove = toasts.success('saved');
  assert.equal(toasts.root.children.length, 1);
  remove();
  assert.equal(toasts.root.children.length, 0);
});

/* --------------------------------------------------------------------------
 * CommandPalette
 * -------------------------------------------------------------------------- */

test('palette runs the highlighted entry and closes', () => {
  const doc = fakeDocument();
  const seen = [];
  const palette = UI.createCommandPalette({
    document: doc,
    entries: [
      { action: 'app.refresh', label: 'app.refresh', description: 'Refresh every panel' },
      { action: 'app.help', label: 'app.help', description: 'Keyboard shortcuts' },
    ],
    onCommand: (a) => seen.push(a),
  });

  palette.open();
  palette.handleKey({ key: 'Enter' }); // first entry is highlighted on open

  assert.deepEqual(seen, ['app.refresh']);
  assert.equal(palette.isOpen(), false, 'running a command closes the palette');
});

test('palette ArrowDown moves the highlight', () => {
  const doc = fakeDocument();
  const seen = [];
  const palette = UI.createCommandPalette({
    document: doc,
    entries: [
      { action: 'app.refresh', label: 'app.refresh', description: 'Refresh every panel' },
      { action: 'app.help', label: 'app.help', description: 'Keyboard shortcuts' },
    ],
    onCommand: (a) => seen.push(a),
  });

  palette.open();
  palette.handleKey({ key: 'ArrowDown' });
  palette.handleKey({ key: 'Enter' });

  assert.deepEqual(seen, ['app.help']);
});

test('palette filters entries as the query changes', () => {
  const doc = fakeDocument();
  const seen = [];
  const palette = UI.createCommandPalette({
    document: doc,
    entries: [
      { action: 'app.refresh', label: 'app.refresh', description: 'Refresh every panel' },
      { action: 'app.help', label: 'app.help', description: 'Keyboard shortcuts' },
    ],
    onCommand: (a) => seen.push(a),
  });

  palette.open();
  // Typing narrows the list, so only the shortcuts entry remains highlighted.
  const input = palette.root.find((n) => n.tagName === 'INPUT');
  input.value = 'shortcut';
  input.fire('input');
  assert.deepEqual(palette.getMatches(), ['app.help']);

  palette.handleKey({ key: 'Enter' });
  assert.deepEqual(seen, ['app.help']);
});

test('palette ignores keys while closed', () => {
  const doc = fakeDocument();
  const palette = UI.createCommandPalette({ document: doc, entries: [], onCommand() { } });
  assert.equal(palette.handleKey({ key: 'Enter' }), false);
});

/* --------------------------------------------------------------------------
 * The keybind contract the UI relies on
 * -------------------------------------------------------------------------- */

test('every action the dashboard binds exists in the registry', () => {
  const registry = KB.createDefaultRegistry();
  const bound = registry.describeAll().map((b) => b.action);

  // These are the actions public/dashboard.js implements. If a binding is
  // renamed or dropped, this test is the tripwire.
  const required = [
    'app.help', 'app.commandPalette', 'app.refresh', 'app.toggleSidebar',
    'app.toggleTheme', 'app.closeOverlay', 'app.gameMenu',
    'nav.panel1', 'nav.panel2', 'nav.panel3', 'nav.panel4', 'nav.panel5',
    'nav.nextPanel', 'nav.prevPanel', 'nav.nextTab', 'nav.prevTab', 'nav.focusMenu',
    'data.search', 'data.export', 'data.refreshPanel',
    'list.next', 'list.prev', 'list.first', 'list.last', 'list.open', 'list.select',
    'dialog.confirm', 'dialog.cancel',
  ];

  for (const action of required) {
    assert.ok(bound.includes(action), `registry is missing "${action}"`);
  }
});

test('every list-scope binding is one the grid implements', () => {
  const registry = KB.createDefaultRegistry();
  const listActions = registry.listScope('list').map((b) => b.action);
  const implemented = ['list.next', 'list.prev', 'list.first', 'list.last', 'list.open', 'list.select'];

  for (const action of listActions) {
    assert.ok(implemented.includes(action), `grid does not implement "${action}"`);
  }
});

test('the list scope owns j/k so a text input keeps them', () => {
  const registry = KB.createDefaultRegistry();
  const down = registry.resolve({ key: 'j' }, 'list');
  const typing = registry.resolve({ key: 'j' }, 'global', { typing: true });

  assert.equal(down && down.action, 'list.next');
  assert.equal(typing, null, 'a global scope must not steal j from an input');
});

test('escape resolves to closeOverlay in the global scope', () => {
  const registry = KB.createDefaultRegistry();
  const binding = registry.resolve({ key: 'Escape' }, 'global');
  assert.equal(binding && binding.action, 'app.closeOverlay');
});

test('ctrl+k resolves to the command palette', () => {
  const registry = KB.createDefaultRegistry();
  const binding = registry.resolve({ key: 'k', ctrlKey: true }, 'global');
  assert.equal(binding && binding.action, 'app.commandPalette');
});

test('registry.describeAll produces the palette shape', () => {
  const registry = KB.createDefaultRegistry();
  const first = registry.describeAll()[0];
  assert.ok(first.action);
  assert.ok(first.description);
  assert.ok(typeof first.pretty === 'string');
});

/* --------------------------------------------------------------------------
 * BootLoader
 * -------------------------------------------------------------------------- */

test('boot loader starts on the first step and is not complete', () => {
  const doc = fakeDocument();
  const boot = UI.createBootLoader({
    document: doc, system: 'MAINFRAME-1', steps: ['one', 'two', 'three'],
  });

  const steps = boot.getSteps();
  assert.equal(steps[0].state, 'active', 'first step is in flight immediately');
  assert.equal(steps[1].state, 'pending');
  assert.equal(steps[2].state, 'pending');
  assert.equal(boot.isComplete(), false);
});

test('boot loader advances one step at a time', () => {
  const doc = fakeDocument();
  const boot = UI.createBootLoader({ document: doc, steps: ['one', 'two', 'three'] });

  boot.step();
  let steps = boot.getSteps();
  assert.equal(steps[0].state, 'done');
  assert.equal(steps[1].state, 'active');
  assert.equal(steps[2].state, 'pending');

  boot.step();
  steps = boot.getSteps();
  assert.equal(steps[1].state, 'done');
  assert.equal(steps[2].state, 'active');
});

test('boot loader reports completion only after the last step', () => {
  const doc = fakeDocument();
  const boot = UI.createBootLoader({ document: doc, steps: ['one', 'two'] });

  boot.step();
  assert.equal(boot.isComplete(), false);
  boot.step();
  assert.equal(boot.isComplete(), true);
});

test('boot loader errors do not count as completed steps', () => {
  const doc = fakeDocument();
  const boot = UI.createBootLoader({ document: doc, steps: ['one', 'two'] });

  boot.fail('database unreachable');
  const steps = boot.getSteps();
  assert.equal(steps[0].state, 'error');
  assert.equal(boot.isComplete(), false, 'a failed boot must never look ready');
});

test('boot loader renders each step as a log line', () => {
  const doc = fakeDocument();
  const boot = UI.createBootLoader({ document: doc, steps: ['alpha', 'beta'] });

  boot.banner();
  boot.step('done', 'alpha: online');

  const lines = boot.getLog();
  assert.ok(lines.some((l) => l.includes('alpha: online')), 'detail line is logged');
  assert.ok(lines.some((l) => l.startsWith('[  OK  ]') && l.includes('Started alpha.')),
    'a completed unit prints an [ OK ] line');
});

/* --- Linux-style console formatting -------------------------------------- */

test('every kernel line is prefixed with a timestamp', () => {
  const doc = fakeDocument();
  const boot = UI.createBootLoader({ document: doc, system: 'MF', steps: ['one'] });

  boot.banner();
  boot.emit('cpu: MF cpu0 detected');

  for (const entry of boot.getLog()) {
    assert.match(entry, /^\[\s*\d+\.\d{6}\]/, `missing timestamp: ${entry}`);
  }
});

test('the banner reports the kernel identity and command line', () => {
  const doc = fakeDocument();
  const boot = UI.createBootLoader({ document: doc, system: 'MAINFRAME-1', version: '2.5.0' });

  boot.banner({ sysplex: 'SYSPLEX-B', region: 'EU-WEST' });

  const log = boot.getLog().join('\n');
  assert.ok(log.includes('MAINFRAME-1 kernel 2.5.0 booting'));
  assert.ok(log.includes('sysplex=SYSPLEX-B'));
  assert.ok(log.includes('region=EU-WEST'));
  assert.ok(log.includes('cpu0 detected'));
});

test('a successful unit prints [  OK  ] and a failure prints [FAILED]', () => {
  const doc = fakeDocument();
  const ok = UI.createBootLoader({ document: doc, steps: ['svc'] });
  ok.step();
  assert.ok(ok.getLog().join('\n').includes('[  OK  ]'));

  const bad = UI.createBootLoader({ document: fakeDocument(), steps: ['svc'] });
  bad.step('error');
  assert.ok(bad.getLog().join('\n').includes('[FAILED]'));
});

test('a failed boot is never reported as complete', () => {
  const doc = fakeDocument();
  const boot = UI.createBootLoader({ document: doc, steps: ['svc'] });

  boot.fail('connection refused');

  assert.equal(boot.hasFailed(), true);
  assert.equal(boot.isComplete(), false);
  const log = boot.getLog().join('\n');
  assert.ok(log.includes('error: connection refused'));
  assert.ok(log.includes('Booting halted'));
});

test('a failure before any completion still halts the boot', () => {
  const doc = fakeDocument();
  const boot = UI.createBootLoader({ document: doc, steps: ['one', 'two'] });

  boot.step();          // one done
  boot.fail('disk gone'); // two fails

  assert.equal(boot.isComplete(), false);
  assert.equal(boot.getSteps()[1].state, 'error');
});

test('finish emits the target and marks the boot complete', async () => {
  const doc = fakeDocument();
  const boot = UI.createBootLoader({ document: doc, steps: ['one'] });

  boot.step();
  await boot.finish('Boot complete in 12ms.');

  const log = boot.getLog().join('\n');
  assert.ok(log.includes('Reached target Operator Console.'));
  assert.ok(log.includes('Boot complete in 12ms.'));
  assert.ok(boot.root.classList.contains('is-done'));
});

test('remove detaches the splash from the document', () => {
  const doc = fakeDocument();
  doc.body = fakeDocument().createElement('body');
  const boot = UI.createBootLoader({ document: doc, steps: ['one'] });

  doc.body.appendChild(boot.root);
  assert.equal(doc.body.children.length, 1);

  boot.remove();
  assert.equal(doc.body.children.length, 0);
});

/* --------------------------------------------------------------------------
 * LoadingScreen
 * -------------------------------------------------------------------------- */

test('loading screen shows its label and can be retargeted', () => {
  const doc = fakeDocument();
  const loading = UI.createLoadingScreen({ document: doc, label: 'Loading users\u2026' });

  assert.ok(loading.root.allText.includes('Loading users'));
  loading.setLabel('Loading jobs\u2026');
  assert.ok(loading.root.allText.includes('Loading jobs'));
});

test('loading screen has a spinner element', () => {
  const doc = fakeDocument();
  const loading = UI.createLoadingScreen({ document: doc });
  assert.ok(loading.root.find(withClass('mf-loading-spinner')));
});

/* --------------------------------------------------------------------------
 * WindowPane
 * -------------------------------------------------------------------------- */

test('window pane applies its title, position and width', () => {
  const doc = fakeDocument();
  const pane = UI.createWindowPane({
    document: doc, title: 'Job Queue', x: 30, y: 50, width: 320,
  });

  assert.ok(pane.root.allText.includes('Job Queue'));
  assert.equal(pane.root.style.left, '30px');
  assert.equal(pane.root.style.top, '50px');
  assert.equal(pane.root.style.width, '320px');
});

test('window pane collapses and expands from its title bar button', () => {
  const doc = fakeDocument();
  const pane = UI.createWindowPane({ document: doc, title: 'Storage' });

  const [collapse] = pane.root.findAll(withClass('mf-pane-btn'));
  assert.equal(pane.isCollapsed(), false);

  collapse.fire('click');
  assert.equal(pane.isCollapsed(), true);

  collapse.fire('click');
  assert.equal(pane.isCollapsed(), false);
});

test('window pane closes and reports it to the caller', () => {
  const doc = fakeDocument();
  let closed = 0;
  const pane = UI.createWindowPane({
    document: doc, title: 'Security', onClose: () => { closed += 1; },
  });

  const host = fakeDocument().createElement('div');
  host.appendChild(pane.root);

  const buttons = pane.root.findAll(withClass('mf-pane-btn'));
  buttons[buttons.length - 1].fire('click'); // the close button

  assert.equal(closed, 1);
  assert.equal(pane.root.parentNode, null, 'pane is removed from its host');
});
test('window pane swaps its content', () => {
  const doc = fakeDocument();
  const pane = UI.createWindowPane({ document: doc, title: 'Overview' });

  pane.setContent([UI.el(doc, 'p', { text: 'first' })]);
  assert.ok(pane.body.allText.includes('first'));

  pane.setContent([UI.el(doc, 'p', { text: 'second' })]);
  assert.ok(pane.body.allText.includes('second'));
  assert.ok(!pane.body.allText.includes('first'), 'old content is replaced, not appended');
});

test('window pane dragging is clamped to the viewport', () => {
  const doc = fakeDocument();
  doc.body = new FakeNode('body');
  const pane = UI.createWindowPane({ document: doc, title: 'Jobs', x: 0, y: 0 });

  // Start a drag from the title bar, then move far off-screen.
  pane.titleBar.fire('pointerdown', { clientX: 5, clientY: 5 });
  doc.fire('pointermove', { clientX: 99999, clientY: 99999 });

  const left = parseInt(pane.root.style.left, 10);
  assert.ok(left < 99999, 'a pane cannot be dragged out of reach');
  assert.ok(left >= 0);
});
