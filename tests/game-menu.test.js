
'use strict';

/**
 * Game menu tests: the full-screen title and pause menu.
 *
 * The navigation rules are the interesting part - what a keypress does to the
 * highlight, which level is live, and whether a value change reaches the
 * entry the menu holds. Those are asserted directly rather than by reading
 * rendered markup, so a passing test means the behaviour is right and not
 * merely that some text appeared.
 */

const test = require('node:test');
const assert = require('node:assert/strict');

const UI = require('../src/ui/components');
const { fakeDocument, withClass } = require('./helpers/fake-dom');

/* --------------------------------------------------------------------------
 * GameMenu
 * -------------------------------------------------------------------------- */

/** A menu shaped like the title/pause screens this component is for. */
function sampleGameMenu() {
  return {
    items: [
      { id: 'start', label: 'Start Console', action: 'game.start', hint: 'Open the console' },
      { id: 'sep', label: '-' },
      {
        id: 'options',
        label: 'Options',
        children: [
          { id: 'volume', label: 'Volume', slider: true, value: 50 },
          { id: 'scan', label: 'Scanlines', toggle: true, value: true },
          { id: 'sound', label: 'Sound', toggle: true, value: false, enabled: false },
        ],
      },
      { id: 'quit', label: 'Quit', action: 'game.quit' },
    ],
  };
}

function sampleGameMenuHarness(extra = {}) {
  const doc = fakeDocument();
  const commands = [];
  const changes = [];
  const menu = UI.createGameMenu({
    document: doc,
    title: 'MAINFRAME',
    subtitle: 'v1.0.0',
    menu: sampleGameMenu(),
    onCommand: (a) => commands.push(a),
    onChange: (entry, value) => changes.push([entry.label, value]),
    ...extra,
  });
  return { doc, menu, commands, changes };
}

/* --- Pure helpers -------------------------------------------------------- */

test('normaliseGameMenu infers entry types', () => {
  const tree = UI.normaliseGameMenu(sampleGameMenu());
  const types = tree.items.map((e) => e.type);
  assert.deepEqual(types, ['item', 'separator', 'submenu', 'item']);
});

test('normaliseGameMenu carries slider bounds and step', () => {
  const tree = UI.normaliseGameMenu({
    items: [{ label: 'Brightness', slider: true, value: 3, min: 1, max: 5, step: 1 }],
  });
  const entry = tree.items[0];
  assert.equal(entry.type, 'slider');
  assert.equal(entry.min, 1);
  assert.equal(entry.max, 5);
  assert.equal(entry.step, 1);
});

test('normaliseGameMenu treats a bare dash as a separator', () => {
  const tree = UI.normaliseGameMenu({ items: ['-'] });
  assert.equal(tree.items[0].type, 'separator');
});

test('buildGameMenuLevels returns one level when nothing is open', () => {
  const tree = UI.normaliseGameMenu(sampleGameMenu());
  assert.equal(UI.buildGameMenuLevels(tree, []).length, 1);
});

test('buildGameMenuLevels adds a level per open submenu', () => {
  const tree = UI.normaliseGameMenu(sampleGameMenu());
  const levels = UI.buildGameMenuLevels(tree, ['options']);
  assert.equal(levels.length, 2);
  assert.equal(levels[1].parentId, 'options');
  assert.deepEqual(levels[1].entries.map((e) => e.label), ['Volume', 'Scanlines', 'Sound']);
});

test('buildGameMenuLevels stops at an unknown submenu id', () => {
  const tree = UI.normaliseGameMenu(sampleGameMenu());
  assert.equal(UI.buildGameMenuLevels(tree, ['nope']).length, 1);
});

test('buildGameMenuLevels shares entry objects rather than copying them', () => {
  const tree = UI.normaliseGameMenu(sampleGameMenu());
  const levels = UI.buildGameMenuLevels(tree, ['options']);
  assert.equal(levels[1].entries[0], tree.items[2].children[0],
    'a slider must be the same object the tree holds, or its value is lost');
});

test('nextSelectable skips separators and disabled entries', () => {
  const entries = [
    { type: 'item', enabled: true },
    { type: 'separator', enabled: false },
    { type: 'item', enabled: true },
    { type: 'item', enabled: false },
    { type: 'item', enabled: true },
  ];
  assert.equal(UI.nextSelectable(entries, 0, 1), 2, 'skips the separator');
  assert.equal(UI.nextSelectable(entries, 2, 1), 4, 'skips the disabled entry');
});

test('nextSelectable wraps around in both directions', () => {
  const entries = [{ type: 'item', enabled: true }, { type: 'item', enabled: true }];
  assert.equal(UI.nextSelectable(entries, 1, 1), 0);
  assert.equal(UI.nextSelectable(entries, 0, -1), 1);
});

test('nextSelectable returns -1 when nothing is selectable', () => {
  assert.equal(UI.nextSelectable([{ type: 'separator', enabled: false }], 0, 1), -1);
  assert.equal(UI.nextSelectable([], 0, 1), -1);
});

/* --- Menu behaviour ------------------------------------------------------ */

test('game menu starts closed and opens on request', () => {
  const { menu } = sampleGameMenuHarness();
  assert.equal(menu.isOpen(), false);
  menu.open();
  assert.equal(menu.isOpen(), true);
});

test('game menu highlights the first selectable entry', () => {
  const { menu } = sampleGameMenuHarness();
  menu.open();
  assert.equal(menu.getActiveLabel(), 'Start Console');
});

test('game menu ignores keys while closed', () => {
  const { menu } = sampleGameMenuHarness();
  assert.equal(menu.handleKey({ key: 'ArrowDown' }), false);
});

test('ArrowDown and ArrowUp move the highlight', () => {
  const { menu } = sampleGameMenuHarness();
  menu.open();

  menu.handleKey({ key: 'ArrowDown' });
  assert.equal(menu.getActiveLabel(), 'Options', 'the separator is skipped');

  menu.handleKey({ key: 'ArrowUp' });
  assert.equal(menu.getActiveLabel(), 'Start Console');
});

test('Enter on a submenu opens the next column', () => {
  const { menu, commands } = sampleGameMenuHarness();
  menu.open();
  menu.handleKey({ key: 'ArrowDown' });
  menu.handleKey({ key: 'Enter' });

  assert.deepEqual(menu.getPath(), ['options']);
  assert.deepEqual(commands, [], 'drilling in must not fire a command');
  assert.equal(menu.getActiveLabel(), 'Volume');
});

test('ArrowRight opens a submenu', () => {
  const { menu } = sampleGameMenuHarness();
  menu.open();
  menu.handleKey({ key: 'ArrowDown' });
  menu.handleKey({ key: 'ArrowRight' });
  assert.deepEqual(menu.getPath(), ['options']);
  assert.equal(menu.getActiveLabel(), 'Volume');
});

test('ArrowLeft on a slider steps its value rather than backing out', () => {
  const { menu, changes } = sampleGameMenuHarness();
  menu.open(['options']);

  menu.handleKey({ key: 'ArrowLeft' });
  assert.deepEqual(changes, [['Volume', 40]]);
  assert.deepEqual(menu.getPath(), ['options'], 'the column stays open');
});

test('Escape backs out of a submenu and restores the parent row', () => {
  const { menu } = sampleGameMenuHarness();
  menu.open(['options']);

  menu.handleKey({ key: 'Escape' });
  assert.deepEqual(menu.getPath(), []);
  assert.equal(menu.getActiveLabel(), 'Options', 'the cursor returns to the parent row');
  assert.equal(menu.isOpen(), true, 'backing out does not dismiss the menu');
});

test('ArrowRight on a slider steps its value and reports the change', () => {
  const { menu, changes } = sampleGameMenuHarness();
  menu.open(['options']);

  menu.handleKey({ key: 'ArrowRight' });
  assert.deepEqual(changes, [['Volume', 60]]);

  menu.handleKey({ key: 'ArrowLeft' });
  assert.deepEqual(changes, [['Volume', 60], ['Volume', 50]]);
});

test('a slider clamps to its own range', () => {
  const doc = fakeDocument();
  const changes = [];
  const menu = UI.createGameMenu({
    document: doc,
    menu: { items: [{ label: 'Level', slider: true, value: 8, min: 1, max: 10, step: 1 }] },
    onChange: (entry, value) => changes.push(value),
  });
  menu.open();

  for (let i = 0; i < 10; i += 1) menu.handleKey({ key: 'ArrowRight' });
  assert.equal(changes[changes.length - 1], 10, 'stops at max');

  for (let i = 0; i < 20; i += 1) menu.handleKey({ key: 'ArrowLeft' });
  assert.equal(changes[changes.length - 1], 1, 'stops at min');
});

test('Enter toggles a switch and reports the new value', () => {
  const { menu, changes } = sampleGameMenuHarness();
  menu.open(['options']);
  menu.handleKey({ key: 'ArrowDown' });   // Scanlines

  menu.handleKey({ key: 'Enter' });
  assert.deepEqual(changes, [['Scanlines', false]]);
});

test('a disabled entry is skipped by navigation', () => {
  const { menu } = sampleGameMenuHarness();
  menu.open(['options']);

  // Volume -> Scanlines -> (Sound is disabled) -> wraps to Volume.
  menu.handleKey({ key: 'ArrowDown' });
  assert.equal(menu.getActiveLabel(), 'Scanlines');
  menu.handleKey({ key: 'ArrowDown' });
  assert.equal(menu.getActiveLabel(), 'Volume');
});

test('Enter on an item fires its action', () => {
  const { menu, commands } = sampleGameMenuHarness();
  menu.open();
  menu.handleKey({ key: 'Enter' });
  assert.deepEqual(commands, ['game.start']);
});

test('Escape unwinds one level, then closes at the top', () => {
  const { menu } = sampleGameMenuHarness();
  menu.open(['options']);

  menu.handleKey({ key: 'Escape' });
  assert.deepEqual(menu.getPath(), []);
  assert.equal(menu.isOpen(), true, 'one Escape only backs out a level');

  menu.handleKey({ key: 'Escape' });
  assert.equal(menu.isOpen(), false, 'Escape at the top dismisses the menu');
});

test('closing the menu calls onClose exactly once', () => {
  let closed = 0;
  const { menu } = sampleGameMenuHarness({ onClose: () => { closed += 1; } });
  menu.open();

  menu.handleKey({ key: 'Escape' });
  assert.equal(closed, 1);

  // A second Escape while closed is ignored.
  assert.equal(menu.handleKey({ key: 'Escape' }), false);
  assert.equal(closed, 1);
});

test('closing resets the path and highlight', () => {
  const { menu } = sampleGameMenuHarness();
  menu.open(['options']);
  menu.close();

  assert.deepEqual(menu.getPath(), []);
  assert.equal(menu.isOpen(), false);
});

test('clicking an entry activates it', () => {
  const { menu, commands } = sampleGameMenuHarness();
  menu.open();

  const rows = menu.root.findAll(withClass('mf-game-item'));
  rows[0].fire('click');
  assert.deepEqual(commands, ['game.start']);
});

test('the deepest column is the only one that is interactive', () => {
  const { menu, commands } = sampleGameMenuHarness();
  menu.open(['options']);

  // Row 0 of the rendered list belongs to the *first* column (Start Console);
  // clicking it while a submenu is open must do nothing.
  const rows = menu.root.findAll(withClass('mf-game-item'));
  rows[0].fire('click');
  assert.deepEqual(commands, []);
});

test('a submenu row is flagged for the renderer', () => {
  const { menu } = sampleGameMenuHarness();
  menu.open();
  const rows = menu.root.findAll(withClass('mf-game-item'));
  assert.ok(rows.some((r) => r.classList.contains('has-children')));
});

test('sliders and switches render their state', () => {
  const { menu } = sampleGameMenuHarness();
  menu.open(['options']);

  assert.ok(menu.root.find(withClass('mf-game-gauge-fill')), 'slider has a gauge');
  assert.ok(menu.root.find((n) => n.className === 'mf-game-switch is-on'),
    'the on switch is marked');
});
