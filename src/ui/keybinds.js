'use strict';

/**
 * Keybind registry.
 *
 * A keybind is a named action bound to one or more key combinations, in the
 * same spirit as a terminal emulator's key map: `Ctrl+K` opens the command
 * palette, `F1` opens help, `Alt+1` jumps to the first panel.
 *
 * Design notes
 * ------------
 *  - Bindings are DATA, not code. A binding is `{ action, combo, scope }`, so
 *    the set can be listed in a help screen, serialised to the client, and
 *    remapped by the user without touching the handlers.
 *  - A combo is normalised to a canonical string ("ctrl+shift+k") so matching
 *    is a plain string comparison and two spellings of the same chord cannot
 *    both bind.
 *  - `scope` decides where a binding applies. A global binding fires anywhere;
 *    a panel binding only fires while that panel has focus. That is what lets
 *    `j`/`k` move a list selection without stealing the key from a text input.
 *  - Conflicting bindings are reported rather than silently overwritten:
 *    a UI that quietly drops a shortcut is a UI where "the key does nothing"
 *    is impossible to debug.
 */

/** Keys that are modifiers on their own and never form a complete combo. */
const MODIFIER_KEYS = new Set(['control', 'ctrl', 'shift', 'alt', 'meta', 'os']);

/** Canonical modifier order, so every spelling sorts the same way. */
const MODIFIER_ORDER = ['ctrl', 'alt', 'shift', 'meta'];

/**
 * Aliases so a binding can be written the way a keyboard prints it.
 * Everything is folded to lower case before lookup.
 */
const KEY_ALIASES = {
  esc: 'escape',
  escape: 'escape',
  return: 'enter',
  enter: 'enter',
  space: ' ',
  spacebar: ' ',
  del: 'delete',
  ins: 'insert',
  pgup: 'pageup',
  pgdn: 'pagedown',
  pageup: 'pageup',
  pagedown: 'pagedown',
  arrowup: 'up',
  arrowdown: 'down',
  arrowleft: 'left',
  arrowright: 'right',
  plus: '+',
  minus: '-',
};

const MODIFIER_ALIASES = {
  ctrl: 'ctrl',
  control: 'ctrl',
  alt: 'alt',
  option: 'alt',
  shift: 'shift',
  meta: 'meta',
  cmd: 'meta',
  command: 'meta',
  win: 'meta',
  super: 'meta',
};

function normaliseKeyName(name) {
  const source = String(name ?? '');
  // The space bar is a legitimate key name; trim() would turn it into "".
  if (source === ' ') return ' ';

  const lowered = source.trim().toLowerCase();
  if (lowered === '') return '';
  return KEY_ALIASES[lowered] || lowered;
}

function normaliseModifier(name) {
  const lowered = String(name ?? '').trim().toLowerCase();
  return MODIFIER_ALIASES[lowered] || null;
}

/**
 * Parse a combo string into its parts.
 *
 * Accepts "Ctrl+Shift+K", "ctrl-shift-k", "CTRL+SHIFT+K" and returns
 * { ctrl: true, alt: false, shift: true, meta: false, key: 'k' }.
 *
 * @param {string} combo
 * @returns {{ctrl:boolean,alt:boolean,shift:boolean,meta:boolean,key:string}}
 */
function parseCombo(combo) {
  const source = String(combo ?? '');
  const raw = source.trim().toLowerCase();

  // The space bar is a single keystroke, not an empty combo. It must be
  // detected before trim() erases it, and splitting on whitespace would
  // discard it anyway, so it gets an explicit early return.
  if (source === ' ' || raw === 'space' || raw === 'spacebar') {
    return { ctrl: false, alt: false, shift: false, meta: false, key: ' ' };
  }

  const parts = source
    .split(/[+\-\s]+/)
    .map((p) => p.trim())
    .filter(Boolean);

  const result = { ctrl: false, alt: false, shift: false, meta: false, key: '' };

  for (const part of parts) {
    const modifier = normaliseModifier(part);
    if (modifier) {
      result[modifier] = true;
      continue;
    }
    // A literal '+' or '-' key arrives as an empty split fragment when written
    // as "ctrl++"; recover it from the raw string.
    const key = normaliseKeyName(part);
    if (key) result.key = key;
  }

  // Handle the trailing-symbol case ("ctrl++", "ctrl+-").
  if (!result.key && (raw.endsWith('++') || raw.endsWith('+-'))) {
    result.key = raw.slice(-1);
  }

  return result;
}

/**
 * Canonical string for a combo, so matching is a string compare.
 * @param {string|object} combo
 * @returns {string}
 */
function formatCombo(combo) {
  const parsed = typeof combo === 'string' ? parseCombo(combo) : combo;
  if (!parsed || !parsed.key) return '';

  const modifiers = MODIFIER_ORDER.filter((m) => parsed[m]);
  return [...modifiers, parsed.key].join('+');
}

/**
 * True when the combo has a real key and is not just modifiers. The space bar
 * is a valid key: parsed.key is " ", which is truthy but must not be compared
 * as if an empty string meant "no key".
 */
function isCompleteCombo(combo) {
  const parsed = typeof combo === 'string' ? parseCombo(combo) : combo;
  if (!parsed || typeof parsed.key !== 'string' || parsed.key === '') return false;
  return !MODIFIER_KEYS.has(parsed.key);
}

/**
 * Human-readable rendering for a help screen: "Ctrl+Shift+K".
 * @param {string} combo
 */
function describeCombo(combo) {
  const parsed = parseCombo(combo);
  if (!parsed.key) return '(unbound)';

  const labels = {
    ctrl: 'Ctrl',
    alt: 'Alt',
    shift: 'Shift',
    meta: 'Meta',
  };
  const prettyKey = parsed.key === ' '
    ? 'Space'
    : parsed.key.length === 1
      ? parsed.key.toUpperCase()
      : parsed.key.charAt(0).toUpperCase() + parsed.key.slice(1);

  return [
    ...MODIFIER_ORDER.filter((m) => parsed[m]).map((m) => labels[m]),
    prettyKey,
  ].join('+');
}

/**
 * Build the canonical combo string from a DOM-like keydown event.
 * Kept DOM-shape-agnostic (plain object) so it is testable in Node.
 *
 * @param {{key:string, ctrlKey?:boolean, altKey?:boolean, shiftKey?:boolean, metaKey?:boolean}} event
 */
function comboFromEvent(event) {
  if (!event || !event.key) return '';
  const key = normaliseKeyName(event.key);
  if (!key || MODIFIER_KEYS.has(key)) return '';

  return formatCombo({
    ctrl: Boolean(event.ctrlKey),
    alt: Boolean(event.altKey),
    shift: Boolean(event.shiftKey),
    meta: Boolean(event.metaKey),
    key,
  });
}

/* --------------------------------------------------------------------------
 * The registry
 * -------------------------------------------------------------------------- */

/**
 * A binding:
 *   { action, combo, scope, description, when }
 *
 *   action      name the command dispatcher understands
 *   combo       canonical combo string
 *   scope       'global' or a panel id
 *   description shown in the help screen
 *   when        optional predicate (context) that must also hold
 */
class KeybindRegistry {
  constructor() {
    /** @type {Map<string, object[]>} combo -> bindings */
    this.byCombo = new Map();
    /** @type {Map<string, object>} action -> binding (one action, one chord) */
    this.byAction = new Map();
  }

  /**
   * Bind an action to a combo.
   *
   * @param {string} action
   * @param {string} combo
   * @param {object} [opts]
   * @param {string} [opts.scope='global']
   * @param {string} [opts.description]
   * @param {Function} [opts.when]
   * @returns {{ok:boolean, reason?:string, binding?:object}}
   */
  bind(action, combo, opts = {}) {
    if (!action || typeof action !== 'string') {
      return { ok: false, reason: 'action must be a non-empty string' };
    }
    if (!isCompleteCombo(combo)) {
      return { ok: false, reason: `"${combo}" is not a complete key combination` };
    }

    const canonical = formatCombo(combo);
    const scope = opts.scope || 'global';

    if (this.byAction.has(action)) {
      return { ok: false, reason: `action "${action}" is already bound` };
    }

    // Same chord, same scope, already taken -> a real conflict.
    const existing = (this.byCombo.get(canonical) || [])
      .find((b) => b.scope === scope);
    if (existing) {
      return {
        ok: false,
        reason: `${describeCombo(canonical)} is already bound to "${existing.action}" in scope "${scope}"`,
      };
    }

    const binding = {
      action,
      combo: canonical,
      scope,
      description: opts.description || '',
      when: typeof opts.when === 'function' ? opts.when : null,
    };

    if (!this.byCombo.has(canonical)) this.byCombo.set(canonical, []);
    this.byCombo.get(canonical).push(binding);
    this.byAction.set(action, binding);

    return { ok: true, binding };
  }

  /** Remove a binding by action name. */
  unbind(action) {
    const binding = this.byAction.get(action);
    if (!binding) return false;

    const list = this.byCombo.get(binding.combo) || [];
    const index = list.indexOf(binding);
    if (index >= 0) list.splice(index, 1);
    if (!list.length) this.byCombo.delete(binding.combo);

    this.byAction.delete(action);
    return true;
  }

  /**
   * Resolve a keydown event to the binding that should run.
   *
   * Resolution order matters: a panel-scoped binding beats a global one, so a
   * focused list can own `j` while the global map does not have to give it up.
   *
   * @param {object} event        keydown-shaped object
   * @param {string} [activeScope='global']
   * @param {object} [context]    passed to each binding's `when`
   * @returns {object|null} the binding, or null when nothing matched
   */
  resolve(event, activeScope = 'global', context = {}) {
    const combo = comboFromEvent(event);
    if (!combo) return null;

    const candidates = this.byCombo.get(combo);
    if (!candidates || !candidates.length) return null;

    const applicable = candidates.filter((b) => !b.when || b.when(context));

    // Most specific scope first: the active panel, then global.
    return applicable.find((b) => b.scope === activeScope)
        || applicable.find((b) => b.scope === 'global')
        || null;
  }

  /** Every binding, for the help screen. */
  list() {
    return [...this.byAction.values()];
  }

  /** Bindings in one scope. */
  listScope(scope) {
    return this.list().filter((b) => b.scope === scope);
  }

  /** Bindings grouped for a keyboard-shortcuts table. */
  describeAll() {
    return this.list()
      .map((b) => ({
        action: b.action,
        combo: b.combo,
        pretty: describeCombo(b.combo),
        scope: b.scope,
        description: b.description,
      }))
      .sort((a, b) => (a.scope === b.scope
        ? a.pretty.localeCompare(b.pretty)
        : a.scope.localeCompare(b.scope)));
  }

  clear() {
    this.byCombo.clear();
    this.byAction.clear();
  }

  get size() {
    return this.byAction.size;
  }
}

/* --------------------------------------------------------------------------
 * Default bindings
 * -------------------------------------------------------------------------- */

/**
 * The shipped key map. Chosen to match the conventions of the operator
 * consoles this project models, and of the web tools operators already use:
 * F-keys for navigation, Ctrl chords for actions, single letters inside lists.
 *
 * @param {KeybindRegistry} registry
 */
function installDefaults(registry) {
  const results = [];
  const add = (action, combo, description, scope = 'global') => {
    const outcome = registry.bind(action, combo, { description, scope });
    results.push({ action, ...outcome });
    // Surface a bad default immediately instead of shipping a key that
    // silently does nothing. `bind` returns a reason rather than throwing.
    if (!outcome.ok) {
      throw new Error(`installDefaults: cannot bind "${action}" to "${combo}": ${outcome.reason}`);
    }
    return outcome;
  };

  /* --- Global: application ------------------------------------------------ */
  add('app.help', 'f1', 'Keyboard shortcuts and help');
  add('app.commandPalette', 'ctrl+k', 'Open the command palette');
  add('app.refresh', 'f5', 'Refresh every panel');
  add('app.toggleSidebar', 'ctrl+b', 'Show or hide the sidebar');
  add('app.toggleTheme', 'ctrl+shift+l', 'Switch between light and dark');
  add('app.closeOverlay', 'escape', 'Close the active dialog or menu');
  add('app.gameMenu', 'f10', 'Open the console menu');

  /* --- Global: navigation ------------------------------------------------- */
  add('nav.panel1', 'alt+1', 'Jump to panel 1 (Status)');
  add('nav.panel2', 'alt+2', 'Jump to panel 2 (Datasets)');
  add('nav.panel3', 'alt+3', 'Jump to panel 3 (Jobs)');
  add('nav.panel4', 'alt+4', 'Jump to panel 4 (Transactions)');
  add('nav.panel5', 'alt+5', 'Jump to panel 5 (Audit log)');
  add('nav.nextPanel', 'ctrl+pagedown', 'Next panel');
  add('nav.prevPanel', 'ctrl+pageup', 'Previous panel');
  add('nav.nextTab', 'ctrl+alt+pagedown', 'Next sub-panel tab');
  add('nav.prevTab', 'ctrl+alt+pageup', 'Previous sub-panel tab');
  add('nav.focusMenu', 'alt+m', 'Focus the menu bar');

  /* --- Global: windows ---------------------------------------------------- */
  add('window.cycle', 'ctrl+tab', 'Cycle to the next open window');
  add('window.cycleBack', 'ctrl+shift+tab', 'Cycle to the previous open window');
  add('window.tile', 'ctrl+shift+t', 'Tile every open window');
  add('window.cascade', 'ctrl+shift+c', 'Cascade every open window');

  /* --- Global: data ------------------------------------------------------- */
  add('data.search', 'ctrl+f', 'Search the active panel');
  add('data.export', 'ctrl+e', 'Export the active panel');
  add('data.refreshPanel', 'ctrl+r', 'Reload the active panel');

  /* --- List scope: single letters, only while a list has focus ------------ */
  add('list.next', 'j', 'Move down one row', 'list');
  add('list.prev', 'k', 'Move up one row', 'list');
  add('list.first', 'home', 'Jump to the first row', 'list');
  add('list.last', 'end', 'Jump to the last row', 'list');
  add('list.open', 'enter', 'Open the selected row', 'list');
  add('list.select', ' ', 'Select or clear the row', 'list');

  /* --- Dialog scope ------------------------------------------------------- */
  add('dialog.confirm', 'enter', 'Confirm the dialog', 'dialog');
  add('dialog.cancel', 'escape', 'Cancel the dialog', 'dialog');

  return results;
}

/**
 * Create a registry with the default map installed.
 * @returns {KeybindRegistry}
 */
function createDefaultRegistry() {
  const registry = new KeybindRegistry();
  installDefaults(registry);
  return registry;
}

const MFKeybinds = {
  MODIFIER_ORDER,
  MODIFIER_KEYS,
  KeybindRegistry,
  parseCombo,
  formatCombo,
  isCompleteCombo,
  describeCombo,
  comboFromEvent,
  installDefaults,
  createDefaultRegistry,
};

// The registry is shared between Node (tests, tooling) and the browser
// (public/dashboard.js). Publishing the same object keeps one source of truth
// for the key map instead of a second copy drifting inside the bundle.
//
// Both guards are needed: `module` does not exist in a browser, so an
// unguarded assignment here would throw and stop the rest of the bundle from
// loading.
if (typeof module !== 'undefined' && module.exports) module.exports = MFKeybinds;
if (typeof window !== 'undefined') window.MFKeybinds = MFKeybinds;
