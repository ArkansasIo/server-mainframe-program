'use strict';

/**
 * Interactive UI component library for the operator dashboard.
 *
 * This file is shared between two worlds:
 *
 *   - the browser, where it is served as a plain <script> and exports onto
 *     `window.MFUI`, and
 *   - Node, where the tests require() it directly.
 *
 * That is why every component is a factory function over a small `document`
 * -like object rather than a class that reaches for globals. Given no DOM at
 * all, the pure helpers (buildMenuModel, filterRows, sortRows) still work,
 * which is what the test suite exercises.
 *
 * Component inventory
 * -------------------
 *   el()             tiny hyperscript helper
 *   MenuBar          File / Edit / View / Help menu with nested submenus
 *   TableView        sortable, selectable, filterable data grid
 *   Tabs             sub-panel tab strip
 *   Dialog           modal confirm / prompt
 *   Toast            transient notifications
 *   CommandPalette   Ctrl+K fuzzy action picker
 *   StatusBar        bottom status line
 */

/* --------------------------------------------------------------------------
 * Pure helpers (no DOM required)
 * -------------------------------------------------------------------------- */

/**
 * Compile a menu tree into a flat, ordered list of visible entries, resolving
 * which submenus are open. Kept pure so menu navigation can be tested without
 * a browser.
 *
 * An entry is `{ id, label, accelerator, action, type, children, enabled }`.
 * `type` is one of 'item' | 'separator' | 'submenu' (inferred when omitted:
 * an entry with `children` is a submenu, `'-'` is a separator).
 *
 * @param {object[]} menu          top-level menu definitions
 * @param {number}   openIndex     index of the open top-level menu, or -1
 * @param {string[]} path          ids of the submenus drilled into, outermost first
 * @returns {Array<{menuIndex:number, depth:number, entry:object, path:string[]}>}
 */
function buildMenuModel(menu, openIndex = -1, path = []) {
  if (!Array.isArray(menu) || openIndex < 0 || openIndex >= menu.length) return [];

  const normalise = (entry) => {
    const type = entry.type
      || (entry === '-' || entry.label === '-' ? 'separator'
        : entry.children ? 'submenu' : 'item');
    return {
      id: entry.id || entry.action || entry.label,
      label: entry.label || '',
      accelerator: entry.accelerator || '',
      action: entry.action || '',
      enabled: entry.enabled !== false,
      children: Array.isArray(entry.children) ? entry.children : [],
      type,
    };
  };

  const out = [];
  let level = menu[openIndex].items || [];

  for (let depth = 0; ; depth += 1) {
    for (const raw of level) {
      const entry = normalise(raw);
      out.push({ menuIndex: openIndex, depth, entry, path: path.slice(0, depth) });
    }

    const nextId = path[depth];
    if (!nextId) break;

    const parent = level.map(normalise).find((e) => e.id === nextId);
    if (!parent || parent.type !== 'submenu' || !parent.children.length) break;
    level = parent.children;
  }

  return out;
}

/**
 * Filter rows by a free-text query across the named columns. Case-insensitive
 * substring match; a blank query returns the rows untouched.
 */
function filterRows(rows, columns, query) {
  const needle = String(query || '').trim().toLowerCase();
  if (!needle) return rows.slice();
  const keys = columns && columns.length ? columns : Object.keys(rows[0] || {});

  return rows.filter((row) => keys.some((key) => {
    // '__any' searches every column of the row, used by the global search box.
    if (key === '__any') {
      return Object.values(row).some((v) => String(v ?? '').toLowerCase().includes(needle));
    }
    return String(row[key] ?? '').toLowerCase().includes(needle);
  }));
}

/**
 * Sort rows by a column. Numeric columns compare numerically, everything else
 * as a locale string. Null and undefined always sort last, in both directions.
 */
function sortRows(rows, column, direction = 'asc') {
  const factor = direction === 'desc' ? -1 : 1;
  const numeric = rows.every((r) => {
    const v = r[column];
    return v === null || v === undefined || v === '' || !Number.isNaN(Number(v));
  });

  return rows.slice().sort((a, b) => {
    const av = a[column];
    const bv = b[column];
    const aEmpty = av === null || av === undefined || av === '';
    const bEmpty = bv === null || bv === undefined || bv === '';
    if (aEmpty && bEmpty) return 0;
    if (aEmpty) return 1;
    if (bEmpty) return -1;
    if (numeric) return (Number(av) - Number(bv)) * factor;
    return String(av).localeCompare(String(bv)) * factor;
  });
}

/** Count rows per distinct value of a column - drives the status summary. */
function groupCount(rows, column) {
  const counts = new Map();
  for (const row of rows) {
    const key = String(row[column] ?? '(none)');
    counts.set(key, (counts.get(key) || 0) + 1);
  }
  return counts;
}

/** Rank palette entries by a fuzzy query; returns the best matches in order. */
function rankPalette(entries, query) {
  const needle = String(query || '').trim().toLowerCase();
  if (!needle) return entries.slice(0, 12);

  const score = (text) => {
    const haystack = text.toLowerCase();
    const direct = haystack.indexOf(needle);
    if (direct >= 0) return 1000 - direct; // earlier match is better
    // Subsequence match ("opn ds" -> "open dataset"), still ordered.
    let cursor = 0;
    let hits = 0;
    for (const ch of haystack) {
      if (ch === needle[cursor]) { cursor += 1; hits += 1; }
      if (cursor === needle.length) break;
    }
    return cursor === needle.length ? hits : -1;
  };

  return entries
    .map((entry) => ({ entry, s: score(`${entry.label} ${entry.description || ''}`) }))
    .filter((r) => r.s >= 0)
    .sort((a, b) => b.s - a.s)
    .map((r) => r.entry)
    .slice(0, 12);
}

/* --------------------------------------------------------------------------
 * DOM helpers
 * -------------------------------------------------------------------------- */

/**
 * Create an element. `attrs` recognises `class`, `text`, `html`, `on*`
 * handlers, `style` objects and dataset keys; everything else becomes an
 * attribute.
 *
 * @param {Document} doc
 */
function el(doc, tag, attrs = {}, children = []) {
  const node = doc.createElement(tag);

  for (const [key, value] of Object.entries(attrs)) {
    if (value === null || value === undefined || value === false) continue;
    if (key === 'class') node.className = value;
    else if (key === 'text') node.textContent = value;
    else if (key === 'html') node.innerHTML = value;
    else if (key === 'style' && typeof value === 'object') Object.assign(node.style, value);
    else if (key.startsWith('on') && typeof value === 'function') {
      node.addEventListener(key.slice(2).toLowerCase(), value);
    } else if (key === 'dataset' && typeof value === 'object') {
      Object.assign(node.dataset, value);
    } else if (value === true) node.setAttribute(key, '');
    else node.setAttribute(key, value);
  }

  for (const child of [].concat(children)) {
    if (child === null || child === undefined || child === false) continue;

    if (typeof child === 'string') {
      node.appendChild(doc.createTextNode(child));
      continue;
    }

    // Accept a widget handle ({ root, ... }) as well as a plain node, so a
    // caller can pass the result of createDefinitionList() straight into a
    // section instead of having to remember to append `.root`.
    const target = child.root || child;
    if (target && typeof target.appendChild === 'function') node.appendChild(target);
  }

  return node;
}

/**
 * Is an overlay hidden? Both the property and the [hidden] attribute are
 * checked: browsers keep them in sync, but a re-render or a test double may
 * only set one of them.
 */
function isHidden(node) {
  if (!node) return true;
  if (node.hidden === true) return true;
  return node.hasAttribute && node.hasAttribute('hidden');
}

/* --------------------------------------------------------------------------
 * MenuBar - File / Edit / View / Help with nested submenus
 * -------------------------------------------------------------------------- */

/**
 * A keyboard- and mouse-driven menu bar.
 *
 * Interactions supported (these are the parts operators actually use):
 *   - click a title to open, click again or press Escape to close
 *   - hover a title while open to slide between menus
 *   - hover or Right-arrow on a submenu to drill in, Left-arrow to come back
 *   - Up/Down move the highlight, Enter activates, Escape closes
 *
 * @param {object} opts
 * @param {Document} opts.document
 * @param {object[]} opts.menu
 * @param {Function} opts.onCommand   called with the entry's `action`
 */
function createMenuBar(opts) {
  const doc = opts.document;
  const menu = opts.menu || [];
  const onCommand = opts.onCommand || (() => { });

  let openIndex = -1;
  let path = [];
  let highlight = 0;

  const root = el(doc, 'div', { class: 'mf-menubar', role: 'menubar' });
  const titles = [];
  let panel = null;

  const visible = () => buildMenuModel(menu, openIndex, path);

  function close() {
    openIndex = -1;
    path = [];
    highlight = 0;
    render();
  }

  function open(index) {
    openIndex = index;
    path = [];
    highlight = 0;
    render();
  }

  function activate(entry) {
    if (!entry || !entry.enabled || entry.type === 'separator') return;
    if (entry.type === 'submenu') return; // handled by hover/arrow
    close();
    onCommand(entry.action, entry);
  }

  function renderPanel() {
    const entries = visible();
    if (!entries.length) return null;

    const list = el(doc, 'div', { class: 'mf-menu-panel', role: 'menu' });

    entries.forEach((item, index) => {
      const { entry, depth } = item;
      const isHighlight = index === highlight;

      if (entry.type === 'separator') {
        list.appendChild(el(doc, 'div', { class: 'mf-menu-sep' }));
        return;
      }

      const row = el(doc, 'div', {
        class: `mf-menu-item${isHighlight ? ' is-active' : ''}`
          + `${entry.enabled ? '' : ' is-disabled'}`
          + `${entry.type === 'submenu' ? ' has-submenu' : ''}`,
        style: { paddingLeft: `${8 + depth * 14}px` },
        role: 'menuitem',
        tabindex: '-1',
      });

      row.appendChild(el(doc, 'span', { class: 'mf-menu-label', text: entry.label }));
      if (entry.accelerator) {
        row.appendChild(el(doc, 'span', { class: 'mf-menu-accel', text: entry.accelerator }));
      }
      if (entry.type === 'submenu') {
        row.appendChild(el(doc, 'span', { class: 'mf-menu-arrow', text: '\u25B6' }));
      }

      row.addEventListener('mouseenter', () => { highlight = index; render(); });
      row.addEventListener('click', (ev) => {
        ev.stopPropagation();
        if (entry.type === 'submenu') {
          path = item.path.concat(entry.id);
          highlight = 0;
          render();
          return;
        }
        activate(entry);
      });

      list.appendChild(row);
    });

    return list;
  }

  function render() {
    root.textContent = '';
    titles.length = 0;

    menu.forEach((top, index) => {
      const title = el(doc, 'button', {
        class: `mf-menu-title${index === openIndex ? ' is-open' : ''}`,
        type: 'button',
        text: top.title,
        'aria-haspopup': 'true',
        'aria-expanded': index === openIndex ? 'true' : 'false',
      });

      title.addEventListener('click', (ev) => {
        ev.stopPropagation();
        if (openIndex === index) close(); else open(index);
      });
      title.addEventListener('mouseenter', () => {
        if (openIndex >= 0 && openIndex !== index) open(index);
      });

      titles.push(title);
      root.appendChild(title);
    });

    panel = renderPanel();
    if (panel) root.appendChild(panel);
  }

  /** Route a raw keydown to the menu while it is open. */
  function handleKey(event) {
    if (openIndex < 0) return false;

    const entries = visible();
    const selectable = entries
      .map((item, index) => ({ item, index }))
      .filter(({ item }) => item.entry.type !== 'separator');

    const move = (delta) => {
      if (!selectable.length) return;
      const current = selectable.findIndex(({ index }) => index === highlight);
      const next = (current + delta + selectable.length) % selectable.length;
      highlight = selectable[next].index;
      render();
    };

    switch (event.key) {
      case 'ArrowDown': move(1); return true;
      case 'ArrowUp': move(-1); return true;
      case 'ArrowRight': {
        const current = entries[highlight];
        if (current && current.entry.type === 'submenu') {
          path = current.path.concat(current.entry.id);
          highlight = 0;
          render();
          return true;
        }
        open((openIndex + 1) % menu.length);
        return true;
      }
      case 'ArrowLeft': {
        if (path.length) {
          path = path.slice(0, -1);
          highlight = 0;
          render();
          return true;
        }
        open((openIndex - 1 + menu.length) % menu.length);
        return true;
      }
      case 'Enter': activate(entries[highlight] && entries[highlight].entry); return true;
      case 'Escape': close(); return true;
      default: return false;
    }
  }

  root.addEventListener('click', (ev) => ev.stopPropagation());
  doc.addEventListener('click', () => { if (openIndex >= 0) close(); });

  render();

  return {
    root,
    open,
    close,
    handleKey,
    isOpen: () => openIndex >= 0,
    getState: () => ({ openIndex, path, highlight }),
  };
}

/* --------------------------------------------------------------------------
 * TableView - data grid
 * -------------------------------------------------------------------------- */

/**
 * @param {object} opts
 * @param {Document} opts.document
 * @param {string[]} opts.columns
 * @param {object[]} opts.rows
 * @param {Function} [opts.onSelect]  called with the selected row
 * @param {string}   [opts.scopeId]   scope id used by the keybind registry ('list')
 */
function createTableView(opts) {
  const doc = opts.document;
  const columns = opts.columns || Object.keys((opts.rows || [])[0] || {});
  const onSelect = opts.onSelect || (() => { });

  const state = {
    rows: opts.rows || [],
    query: '',
    sortColumn: null,
    sortDirection: 'asc',
    selectedIndex: -1,
  };

  const root = el(doc, 'div', { class: 'mf-table-wrap' });

  function view() {
    const filtered = filterRows(state.rows, columns.concat('__any'), state.query);
    return state.sortColumn
      ? sortRows(filtered, state.sortColumn, state.sortDirection)
      : filtered;
  }

  function render() {
    const rows = view();
    root.textContent = '';

    const table = el(doc, 'table', { class: 'mf-table' });

    const head = el(doc, 'thead');
    const headRow = el(doc, 'tr');
    columns.forEach((column) => {
      const active = state.sortColumn === column;
      const th = el(doc, 'th', {
        class: active ? 'is-sorted' : '',
        text: column + (active ? (state.sortDirection === 'asc' ? ' \u25B2' : ' \u25BC') : ''),
        tabindex: '0',
      });
      th.addEventListener('click', () => toggleSort(column));
      headRow.appendChild(th);
    });
    head.appendChild(headRow);
    table.appendChild(head);

    const body = el(doc, 'tbody');
    rows.forEach((row, index) => {
      const tr = el(doc, 'tr', {
        class: index === state.selectedIndex ? 'is-selected' : '',
      });
      columns.forEach((column) => {
        tr.appendChild(el(doc, 'td', { text: row[column] === null ? '' : String(row[column]) }));
      });
      tr.addEventListener('click', () => select(index, row));
      body.appendChild(tr);
    });
    table.appendChild(body);
    root.appendChild(table);

    if (!rows.length) {
      root.appendChild(el(doc, 'p', { class: 'mf-empty', text: '(no rows)' }));
    }
  }

  function toggleSort(column) {
    if (state.sortColumn === column) {
      state.sortDirection = state.sortDirection === 'asc' ? 'desc' : 'asc';
    } else {
      state.sortColumn = column;
      state.sortDirection = 'asc';
    }
    render();
  }

  function select(index, row) {
    state.selectedIndex = index;
    render();
    onSelect(row, index);
  }

  render();

  return {
    root,
    scopeId: opts.scopeId || 'list',
    setRows(rows) { state.rows = rows || []; state.selectedIndex = -1; render(); },
    setQuery(query) { state.query = query; state.selectedIndex = -1; render(); },
    getQuery: () => state.query,
    getSelection: () => view()[state.selectedIndex] || null,
    rowCount: () => view().length,
    toggleSort,
    move(delta) {
      const rows = view();
      if (!rows.length) return;
      const next = Math.max(0, Math.min(rows.length - 1, state.selectedIndex + delta));
      select(next, rows[next]);
    },
    moveTo(position) {
      const rows = view();
      if (!rows.length) return;
      select(position === 'first' ? 0 : rows.length - 1, rows[position === 'first' ? 0 : rows.length - 1]);
    },
    openSelected() {
      const rows = view();
      if (!rows.length) return; // nothing to open
      // Enter/Space on an unfocused grid opens the first row rather than
      // doing nothing, which matches how the terminal console behaves.
      const index = state.selectedIndex >= 0 ? state.selectedIndex : 0;
      select(index, rows[index]);
    },
    getState: () => ({ ...state }),
  };
}

/* --------------------------------------------------------------------------
 * Tabs - sub-panel strip
 * -------------------------------------------------------------------------- */

/**
 * @param {object} opts
 * @param {Document} opts.document
 * @param {Array<{id:string,label:string}>} opts.tabs
 * @param {Function} opts.onChange
 */
function createTabs(opts) {
  const doc = opts.document;
  const tabs = opts.tabs || [];
  const onChange = opts.onChange || (() => { });

  let active = tabs.length ? tabs[0].id : null;
  const root = el(doc, 'div', { class: 'mf-tabs', role: 'tablist' });

  function render() {
    root.textContent = '';
    tabs.forEach((tab) => {
      const button = el(doc, 'button', {
        class: `mf-tab${tab.id === active ? ' is-active' : ''}`,
        type: 'button',
        role: 'tab',
        'aria-selected': tab.id === active ? 'true' : 'false',
        text: tab.label,
      });
      button.addEventListener('click', () => select(tab.id));
      root.appendChild(button);
    });
  }

  function select(id) {
    active = id;
    render();
    onChange(id);
  }

  function step(delta) {
    if (!tabs.length) return;
    const index = tabs.findIndex((t) => t.id === active);
    const next = (index + delta + tabs.length) % tabs.length;
    select(tabs[next].id);
  }

  render();

  return { root, select, next: () => step(1), prev: () => step(-1), active: () => active, tabs };
}

/* --------------------------------------------------------------------------
 * Dialog and Toast
 * -------------------------------------------------------------------------- */

/** Modal dialog. Resolves true on confirm, false on cancel. */
function createDialog(opts) {
  const doc = opts.document;
  let resolveFn = null;

  const input = opts.prompt !== undefined
    ? el(doc, 'input', { class: 'mf-input', value: opts.prompt })
    : null;

  const root = el(doc, 'div', { class: 'mf-overlay' }, [
    el(doc, 'div', { class: 'mf-dialog', role: 'dialog', 'aria-modal': 'true' }, [
      el(doc, 'h3', { class: 'mf-dialog-title', text: opts.title || 'Confirm' }),
      el(doc, 'p', { class: 'mf-dialog-text', text: opts.message || '' }),
      input,
      el(doc, 'div', { class: 'mf-dialog-actions' }, [
        el(doc, 'button', {
          class: 'mf-btn', type: 'button', text: 'Cancel',
          onclick: () => settle(false),
        }),
        el(doc, 'button', {
          class: 'mf-btn is-primary', type: 'button', text: opts.confirmLabel || 'OK',
          onclick: () => settle(true),
        }),
      ]),
    ]),
  ]);
  setHidden(root, true);

  function settle(value) {
    setHidden(root, true);
    const fn = resolveFn;
    resolveFn = null;
    if (fn) fn(value ? (input ? input.value : true) : false);
  }
  /** The 'dialog' keybind scope: Enter confirms, Escape cancels. */
  function handleKey(event) {
    if (isHidden(root)) return false;
    if (event.key === 'Enter') { settle(true); return true; }
    if (event.key === 'Escape') { settle(false); return true; }
    return false;
  }

  root.addEventListener('click', (ev) => {
    if (ev.target === root) settle(false);
  });

  return {
    root,
    handleKey,
    isOpen: () => !isHidden(root),
    open() {
      setHidden(root, false);
      if (input) { input.value = opts.prompt || ''; input.focus(); }
      return new Promise((resolve) => { resolveFn = resolve; });
    },
  };
}

/**
 * Show or hide an overlay, keeping the `hidden` property and the [hidden]
 * attribute in step. Browsers mirror the two automatically, but code that
 * reads one and writes the other is a classic source of "the dialog will not
 * close" bugs - so every overlay goes through here.
 */
function setHidden(node, hidden) {
  if (!node) return;
  node.hidden = hidden;
  if (hidden) {
    if (node.setAttribute) node.setAttribute('hidden', '');
  } else if (node.removeAttribute) {
    node.removeAttribute('hidden');
  }
}

/** Stack of transient notifications. */
function createToastHost(opts) {
  const doc = opts.document;
  const root = el(doc, 'div', { class: 'mf-toasts', role: 'status', 'aria-live': 'polite' });

  function show(message, kind = 'info', ttlMs = 4000) {
    const node = el(doc, 'div', { class: `mf-toast is-${kind}`, text: message });
    root.appendChild(node);

    const remove = () => {
      if (node.parentNode) node.parentNode.removeChild(node);
    };
    node.addEventListener('click', remove);
    if (ttlMs > 0) setTimeout(remove, ttlMs);

    return remove;
  }

  return {
    root,
    info: (m) => show(m, 'info'),
    warn: (m) => show(m, 'warn'),
    error: (m) => show(m, 'error', 8000),
    success: (m) => show(m, 'success'),
    show,
  };
}

/* --------------------------------------------------------------------------
 * CommandPalette
 * -------------------------------------------------------------------------- */

/**
 * Fuzzy action picker. Entries are `{ action, label, description, scope }`,
 * typically derived from the keybind registry so the palette and the keyboard
 * can never drift apart.
 */
function createCommandPalette(opts) {
  const doc = opts.document;
  const onCommand = opts.onCommand || (() => { });
  let entries = opts.entries || [];
  let matches = [];
  let highlight = 0;

  const input = el(doc, 'input', {
    class: 'mf-input', type: 'text', placeholder: 'Type a command\u2026',
    'aria-label': 'Command palette',
  });
  const list = el(doc, 'div', { class: 'mf-palette-list', role: 'listbox' });

  const root = el(doc, 'div', { class: 'mf-overlay' }, [
    el(doc, 'div', { class: 'mf-palette', role: 'dialog', 'aria-modal': 'true' }, [input, list]),
  ]);
  setHidden(root, true);

  function render() {
    matches = rankPalette(entries, input.value);
    if (highlight >= matches.length) highlight = Math.max(0, matches.length - 1);

    list.textContent = '';
    matches.forEach((entry, index) => {
      const row = el(doc, 'div', {
        class: `mf-palette-item${index === highlight ? ' is-active' : ''}`,
        role: 'option',
      }, [
        el(doc, 'span', { class: 'mf-palette-label', text: entry.label }),
        el(doc, 'span', { class: 'mf-palette-desc', text: entry.description || '' }),
        entry.accelerator
          ? el(doc, 'span', { class: 'mf-palette-accel', text: entry.accelerator })
          : null,
      ]);
      row.addEventListener('mouseenter', () => { highlight = index; render(); });
      row.addEventListener('click', () => choose(entry));
      list.appendChild(row);
    });

    if (!matches.length) {
      list.appendChild(el(doc, 'div', { class: 'mf-palette-empty', text: 'No matching command' }));
    }
  }

  function choose(entry) {
    close();
    onCommand(entry.action, entry);
  }

  function close() {
    setHidden(root, true);
    input.value = '';
  }

  function open() {
    setHidden(root, false);
    input.value = '';
    highlight = 0;
    render();
    input.focus();
  }

  input.addEventListener('input', () => { highlight = 0; render(); });

  /** Enter runs the highlighted entry; Escape closes. */
  function handleKey(event) {
    if (isHidden(root)) return false;
    switch (event.key) {
      case 'ArrowDown':
        highlight = Math.min(highlight + 1, Math.max(0, matches.length - 1));
        render();
        return true;
      case 'ArrowUp':
        highlight = Math.max(0, highlight - 1);
        render();
        return true;
      case 'Enter':
        if (matches[highlight]) choose(matches[highlight]);
        return true;
      case 'Escape':
        close();
        return true;
      default:
        return false;
    }
  }

  root.addEventListener('click', (ev) => { if (ev.target === root) close(); });

  return {
    root,
    open,
    close,
    handleKey,
    isOpen: () => !isHidden(root),
    setEntries(next) { entries = next || []; },
    getMatches: () => matches.map((m) => m.action),
  };
}

/* --------------------------------------------------------------------------
 * StatusBar
 * -------------------------------------------------------------------------- */

function createStatusBar(opts) {
  const doc = opts.document;
  const left = el(doc, 'span', { class: 'mf-status-left' });
  const right = el(doc, 'span', { class: 'mf-status-right' });

  const root = el(doc, 'div', { class: 'mf-statusbar', role: 'status' }, [left, right]);

  return {
    root,
    setLeft: (text) => { left.textContent = text; },
    setRight: (text) => { right.textContent = text; },
  };
}

/* --------------------------------------------------------------------------
 * BootLoader - the kernel-style console shown while the system comes up
 * -------------------------------------------------------------------------- */

/**
 * The full-screen boot sequence, written the way a Linux console writes one.
 *
 * A mainframe operator already reads kernel logs for a living, so the boot
 * screen borrows that grammar rather than inventing a new one:
 *
 *   [    0.000000] MAINFRAME-1 kernel 1.0.0 booting
 *   [    0.014220] cpu: MAINFRAME cpu0 detected
 *   [    0.331004] db: storage subsystem online
 *   [  OK  ] Started HTTP control plane.
 *   [FAILED] Failed to start terminal gateway.
 *
 * Two kinds of line are emitted, and they mean different things:
 *
 *   `emit()`  - a plain kernel message: timestamped, informational, always
 *               succeeds. Use it for probes that cannot fail.
 *   `step()`  - a unit start: emits a timestamped result line and, on success,
 *               a `[  OK  ]` line. This is the one that advances the bar.
 *
 * Both derive their timestamp from the moment of the call, so the log is a
 * real record of when work happened - not a scripted animation.
 *
 * @param {object} opts
 * @param {Document} opts.document
 * @param {string} opts.system
 * @param {string} [opts.version]
 * @param {string[]} [opts.steps]     unit names, started in order
 * @param {boolean} [opts.interactive] wait for Enter before revealing the console
 */
function createBootLoader(opts) {
  const doc = opts.document;
  const system = opts.system || 'MAINFRAME';
  const version = opts.version || '1.0.0';
  const steps = opts.steps || [
    'Storage subsystem',
    'Database service',
    'Dataset catalog',
    'HTTP control plane',
    'Terminal gateway',
    'Operator console',
  ];

  /* --- The console frame ------------------------------------------------- */

  const log = el(doc, 'div', { class: 'mf-boot-log', role: 'log', 'aria-live': 'polite' });
  const cursor = el(doc, 'span', { class: 'mf-boot-cursor' });
  const promptRow = el(doc, 'div', { class: 'mf-boot-prompt' }, [cursor]);

  const bar = el(doc, 'div', { class: 'mf-boot-bar' });
  const fill = el(doc, 'div', { class: 'mf-boot-fill' });
  bar.appendChild(fill);

  const percent = el(doc, 'span', { class: 'mf-boot-percent', text: '0%' });
  const indicator = el(doc, 'span', { class: 'mf-boot-indicator', text: 'booting' });

  const console = el(doc, 'div', { class: 'mf-boot-console' }, [log, promptRow]);

  const root = el(doc, 'div', { class: 'mf-boot' }, [
    el(doc, 'div', { class: 'mf-boot-frame' }, [
      el(doc, 'div', { class: 'mf-boot-head' }, [
        el(doc, 'span', { class: 'mf-boot-brand', text: system }),
        el(doc, 'span', { class: 'mf-boot-sub', text: `kernel ${version}` }),
        indicator,
      ]),
      console,
      el(doc, 'div', { class: 'mf-boot-foot' }, [
        el(doc, 'span', { class: 'mf-boot-stage', text: 'SYSTEM INITIALISATION' }),
        bar,
        percent,
      ]),
    ]),
  ]);

  /* --- State ------------------------------------------------------------- */

  const startedAt = Date.now();
  let completed = 0;
  let failed = false;
  /** @type {Array<{name:string,state:'pending'|'active'|'done'|'error'}>} */
  const tracked = steps.map((name) => ({ name, state: 'pending' }));

  /** Kernel uptime stamp: seconds since the loader appeared, 6 decimals. */
  function stamp() {
    return ((Date.now() - startedAt) / 1000).toFixed(6).padStart(12, ' ');
  }

  function append(node) {
    log.appendChild(node);
    // Keep the console pinned to the newest line, the way a terminal does.
    console.scrollTop = console.scrollHeight || 0;
    return node;
  }

  function line(kind, text) {
    return append(el(doc, 'div', {
      class: `mf-boot-line is-${kind}`,
      text: `[${stamp()}] ${text}`,
    }));
  }

  function progress() {
    const pct = Math.round((completed / tracked.length) * 100);
    fill.style.width = `${pct}%`;
    percent.textContent = `${pct}%`;
  }

  /* --- Emission ---------------------------------------------------------- */

  /** A plain kernel message. Always informational. */
  function emit(text) {
    line('info', text);
  }

  /** A `[  OK  ]` / `[FAILED]` unit line, as systemd prints them. */
  function unit(label, ok) {
    const marker = ok ? '[  OK  ]' : '[FAILED]';
    append(el(doc, 'div', {
      class: `mf-boot-unit is-${ok ? 'ok' : 'failed'}`,
      text: `${marker} ${label}`,
    }));
  }

  /**
   * Complete the in-flight unit and start the next.
   * @param {'done'|'error'} [result='done']
   * @param {string} [detail]  extra kernel message emitted with the result
   */
  function step(result = 'done', detail) {
    const active = tracked.find((s) => s.state === 'active');
    if (active) {
      active.state = result;
      const ok = result === 'done';
      if (ok) completed += 1;
      else failed = true;

      if (detail) emit(detail);
      unit(`Started ${active.name}.`, ok);
    }

    const next = tracked.find((s) => s.state === 'pending');
    if (next) next.state = 'active';

    progress();
  }

  /** Mark the current unit failed and hold the console for the operator. */
  function fail(reason) {
    const active = tracked.find((s) => s.state === 'active');
    if (active) {
      active.state = 'error';
      failed = true;
      unit(`Failed to start ${active.name}.`, false);
    } else {
      unit('Boot failure.', false);
    }

    if (reason) line('error', `error: ${reason}`);
    line('error', 'Booting halted. The console cannot start without this service.');

    root.classList.add('is-failed');
    indicator.textContent = 'failed';
    indicator.className = 'mf-boot-indicator is-failed';
    promptRow.style.display = 'none';
  }

  /** Print the boot banner. Called once, before any real work. */
  function banner(extra = {}) {
    emit(`${system} kernel ${version} booting`);
    emit(`Command line: sysplex=${extra.sysplex || 'DEFAULT'} region=${extra.region || 'DEFAULT'}`);
    emit(`cpu: ${system} cpu0 detected`);
    emit(`memory: 65536K/65536K available`);
    emit(`console: colour tty0, 80x25`);
  }

  // The first unit is in flight as soon as the loader appears.
  if (tracked.length) tracked[0].state = 'active';
  progress();

  return {
    root,
    console,
    banner,
    emit,
    step,
    fail,
    /**
     * Mark the boot finished: fade out and hand control to the console.
     * Returns a promise that resolves once the splash has been removed.
     */
    finish: (summary) => new Promise((resolve) => {
      if (summary) emit(summary);
      emit('Reached target Operator Console.');
      unit('Started MAINFRAME interactive console.', true);

      indicator.textContent = 'ready';
      indicator.className = 'mf-boot-indicator is-ready';
      root.classList.add('is-done');
      resolve();
    }),
    /** Remove the splash from the document. */
    remove: () => {
      if (root.parentNode) root.parentNode.removeChild(root);
    },
    isComplete: () => !failed && completed >= tracked.length,
    hasFailed: () => failed,
    getSteps: () => tracked.map((s) => ({ ...s })),
    getLog: () => log.children.map((n) => n.textContent),
  };
}

/* --------------------------------------------------------------------------
 * LoadingScreen - the in-place spinner for a slow panel
 * -------------------------------------------------------------------------- */

/**
 * A loading placeholder, used inside a panel while its data is in flight.
 * Distinct from the boot loader: this one is transient and inline, and it
 * tells the operator exactly which resource is being waited on.
 */
function createLoadingScreen(opts) {
  const doc = opts.document;
  const label = el(doc, 'div', { class: 'mf-loading-label', text: opts.label || 'Loading\u2026' });

  const root = el(doc, 'div', { class: 'mf-loading', role: 'status', 'aria-live': 'polite' }, [
    el(doc, 'div', { class: 'mf-loading-spinner' }),
    label,
  ]);

  return {
    root,
    setLabel: (text) => { label.textContent = text; },
  };
}

/* --------------------------------------------------------------------------
 * WindowPane - dockable panels and system views
 * -------------------------------------------------------------------------- */

/**
 * A movable, collapsible window pane.
 *
 * The operator console is really several tools at once - the record list, the
 * system summary, the job spool - and forcing them into one fixed layout wastes
 * the screen. A pane can be dragged by its title bar and collapsed to just the
 * bar, so the operator arranges the console once and keeps it.
 *
 * Position is clamped to the viewport so a pane can never be dragged out of
 * reach.
 *
 * @param {object} opts
 * @param {Document} opts.document
 * @param {string} opts.title
 * @param {Node}   [opts.body]     pane content
 * @param {number} [opts.x]
 * @param {number} [opts.y]
 * @param {number} [opts.width]
 * @param {Function} [opts.onClose]
 */
function createWindowPane(opts) {
  const doc = opts.document;
  const x = opts.x ?? 40;
  const y = opts.y ?? 40;
  const width = opts.width ?? 420;

  const body = el(doc, 'div', { class: 'mf-pane-body' }, [opts.body || null]);
  const title = el(doc, 'span', { class: 'mf-pane-title', text: opts.title });

  const collapseBtn = el(doc, 'button', {
    class: 'mf-pane-btn', type: 'button', text: '\u2013', title: 'Collapse', 'aria-label': 'Collapse pane',
  });
  const closeBtn = el(doc, 'button', {
    class: 'mf-pane-btn', type: 'button', text: '\u00D7', title: 'Close', 'aria-label': 'Close pane',
  });

  const titleBar = el(doc, 'div', { class: 'mf-pane-titlebar' }, [title, collapseBtn, closeBtn]);
  const root = el(doc, 'div', {
    class: 'mf-pane',
    style: { left: `${x}px`, top: `${y}px`, width: `${width}px` },
  }, [titleBar, body]);

  let collapsed = false;

  collapseBtn.addEventListener('click', (ev) => {
    ev.stopPropagation();
    collapsed = !collapsed;
    root.classList.toggle('is-collapsed', collapsed);
    collapseBtn.textContent = collapsed ? '+' : '\u2013';
  });

  closeBtn.addEventListener('click', (ev) => {
    ev.stopPropagation();
    if (root.parentNode) root.parentNode.removeChild(root);
    if (opts.onClose) opts.onClose();
  });

  // Dragging. Pointer events cover mouse, pen and touch with one code path.
  let drag = null;

  function clamp(value, min, max) {
    return Math.min(Math.max(value, min), max);
  }

  titleBar.addEventListener('pointerdown', (ev) => {
    if (ev.target === collapseBtn || ev.target === closeBtn) return;
    const rect = root.getBoundingClientRect
      ? root.getBoundingClientRect()
      : { left: x, top: y };

    drag = { dx: ev.clientX - rect.left, dy: ev.clientY - rect.top };
    root.classList.add('is-dragging');
  });

  doc.addEventListener('pointermove', (ev) => {
    if (!drag) return;
    // Viewport size comes from the document's own view rather than the global
    // `window`, so this code is exercisable outside a browser.
    const view = doc.defaultView || (typeof window !== 'undefined' ? window : null);
    const vw = (view && view.innerWidth) || 1200;
    const vh = (view && view.innerHeight) || 800;

    const maxX = Math.max(0, vw - 120);
    const maxY = Math.max(0, vh - 40);
    const left = clamp(ev.clientX - drag.dx, 0, maxX);
    const top = clamp(ev.clientY - drag.dy, 0, maxY);
    root.style.left = `${left}px`;
    root.style.top = `${top}px`;
  });

  doc.addEventListener('pointerup', () => {
    if (!drag) return;
    drag = null;
    root.classList.remove('is-dragging');
  });

  return {
    root,
    body,
    titleBar,
    setTitle: (text) => { title.textContent = text; },
    setContent: (nodes) => {
      // Remove children explicitly rather than relying on textContent = '',
      // since that only clears child nodes in a real DOM.
      while (body.firstChild) body.removeChild(body.firstChild);
      body.textContent = '';
      for (const node of [].concat(nodes)) {
        if (node) body.appendChild(node);
      }
    },
    isCollapsed: () => collapsed,
    getPosition: () => ({ left: root.style.left, top: root.style.top }),
    close: () => { if (root.parentNode) root.parentNode.removeChild(root); },
  };
}

/* --------------------------------------------------------------------------
 * Pane content widgets
 * -------------------------------------------------------------------------- */

/**
 * A key/value block - the shape most system views want.
 *
 * Values are stringified here rather than by each caller, so a view can pass a
 * raw number or null and get consistent output (`\u2014` for an absent value).
 */
function createDefinitionList(opts) {
  const doc = opts.document;
  const root = el(doc, 'dl', { class: 'mf-defs' });

  const render = (entries = []) => {
    while (root.firstChild) root.removeChild(root.firstChild);
    for (const [key, value] of entries) {
      root.appendChild(el(doc, 'dt', { text: key }));
      root.appendChild(el(doc, 'dd', {
        text: value === null || value === undefined || value === '' ? '\u2014' : String(value),
      }));
    }
  };

  render(opts.entries || []);
  return { root, setEntries: render, entries: () => (opts.entries || []) };
}

/**
 * A horizontal bar per distinct value of a column - the distribution view.
 *
 * Percentages are of the row total, so a caller does not have to normalise.
 */
function createDistributionList(opts) {
  const doc = opts.document;
  const root = el(doc, 'div', { class: 'mf-dist' });

  const render = (rows = [], column = '') => {
    while (root.firstChild) root.removeChild(root.firstChild);

    if (!rows.length) {
      root.appendChild(el(doc, 'p', { class: 'mf-empty', text: '(no rows)' }));
      return;
    }

    const counts = groupCount(rows, column);
    const total = rows.length;

    // Largest first: the interesting value should be at the top.
    for (const [label, count] of [...counts].sort((a, b) => b[1] - a[1])) {
      const percent = Math.round((count / total) * 100);
      root.appendChild(el(doc, 'div', { class: 'mf-dist-row' }, [
        el(doc, 'span', { class: 'mf-dist-label', text: label, title: label }),
        el(doc, 'span', { class: 'mf-dist-bar' }, [
          el(doc, 'span', { class: 'mf-dist-fill', style: { width: `${percent}%` } }),
        ]),
        el(doc, 'span', { class: 'mf-dist-count', text: `${count} (${percent}%)` }),
      ]));
    }
  };

  render(opts.rows || [], opts.column || '');
  return { root, setData: render };
}

/**
 * A labelled section - a heading plus whatever the view wants under it.
 * Views are built from a stack of these, which keeps the headings uniform.
 */
function createSection(opts) {
  const doc = opts.document;
  const heading = el(doc, 'h4', { class: 'mf-pane-heading', text: opts.heading || '' });
  // `el` unwraps widget handles, so the content may be nodes, widgets or text.
  const body = el(doc, 'div', { class: 'mf-section-body' }, [].concat(opts.content || []));

  return {
    root: el(doc, 'section', { class: 'mf-section' }, [heading, body]),
    body,
    setHeading: (text) => { heading.textContent = text; },
    setContent: (nodes) => {
      while (body.firstChild) body.removeChild(body.firstChild);
      for (const node of [].concat(nodes)) {
        const target = node && (node.root || node);
        if (target && typeof target.appendChild === 'function') body.appendChild(target);
      }
    },
  };
}

/* --------------------------------------------------------------------------
 * WindowManager - the desktop that owns every pane
 * -------------------------------------------------------------------------- */

/**
 * A tiny window manager: open panes, focus them, stack them, tile them.
 *
 * The console is genuinely several tools at once, and before this each view
 * just floated wherever it was placed with no z-order, no way back to a
 * window you lost behind another, and no way to see what was open. A window
 * that cannot be found again is worse than no window at all.
 *
 * Responsibilities:
 *   - one place that knows every open window, so a taskbar can list them
 *   - focus and z-order: clicking a window raises it, and there is always a
 *     known active window
 *   - minimize / restore / maximize, with the geometry saved across a
 *     minimize so restoring puts the window back where it was
 *   - tiling and cascading, so a lost window is one click away
 *
 * It owns the desktop element and the taskbar; individual panes stay the
 * business of createWindowPane.
 */
function createWindowManager(opts) {
  const doc = opts.document;
  const onFocusChange = opts.onFocusChange || (() => {});

  const desktop = el(doc, 'div', { class: 'mf-desktop' });
  const taskbar = el(doc, 'div', { class: 'mf-taskbar', role: 'toolbar' });
  const root = el(doc, 'div', { class: 'mf-wm' }, [desktop, taskbar]);

  /** @type {Map<string, object>} id -> window record */
  const windows = new Map();
  let zCounter = 10;
  let focusedId = null;

  /* --- taskbar ---------------------------------------------------------- */

  function renderTaskbar() {
    while (taskbar.firstChild) taskbar.removeChild(taskbar.firstChild);

    taskbar.appendChild(el(doc, 'span', { class: 'mf-taskbar-label', text: 'WINDOWS' }));

    if (!windows.size) {
      taskbar.appendChild(el(doc, 'span', { class: 'mf-taskbar-empty', text: '(none open)' }));
      return;
    }

    for (const record of windows.values()) {
      const button = el(doc, 'button', {
        class: 'mf-task'
          + `${record.id === focusedId ? ' is-active' : ''}`
          + `${record.minimized ? ' is-minimized' : ''}`,
        type: 'button',
        text: record.title,
        title: record.minimized ? `Restore ${record.title}` : `Focus ${record.title}`,
      });

      button.addEventListener('click', () => {
        // Clicking the active window's button minimizes it, the way a taskbar
        // button behaves everywhere else.
        if (record.id === focusedId && !record.minimized) minimize(record.id);
        else focus(record.id);
      });

      taskbar.appendChild(button);
    }
  }

  /* --- focus and z-order ------------------------------------------------ */

  function focus(id) {
    const record = windows.get(id);
    if (!record) return false;

    record.minimized = false;
    record.pane.root.classList.remove('is-minimized');
    record.pane.root.hidden = false;

    zCounter += 1;
    record.z = zCounter;
    record.pane.root.style.zIndex = String(zCounter);

    for (const other of windows.values()) {
      other.pane.root.classList.toggle('is-focused', other.id === id);
    }

    focusedId = id;
    renderTaskbar();
    onFocusChange(record);
    return true;
  }

  /* --- geometry --------------------------------------------------------- */

  /** Remember where a window was, so restore can put it back. */
  function snapshotGeometry(record) {
    const style = record.pane.root.style;
    // Store bare numbers, not the "120px" the style holds, so restoring can go
    // back through setGeometry() without the unit being appended twice.
    record.restore = {
      left: parseFloat(style.left) || 0,
      top: parseFloat(style.top) || 0,
      width: parseFloat(style.width) || null,
      height: style.height ? parseFloat(style.height) : null,
    };
  }

  function setGeometry(record, box) {
    const style = record.pane.root.style;
    if (box.left !== undefined) style.left = `${box.left}px`;
    if (box.top !== undefined) style.top = `${box.top}px`;
    if (box.width !== undefined) style.width = box.width === null ? '' : `${box.width}px`;
    // An explicit null clears the height, returning the window to its
    // content-driven size.
    if (box.height !== undefined) style.height = box.height === null ? '' : `${box.height}px`;
  }

  function viewport() {
    const view = doc.defaultView || (typeof window !== 'undefined' ? window : null);
    return {
      width: (view && view.innerWidth) || 1200,
      height: (view && view.innerHeight) || 800,
    };
  }

  /* --- window operations ------------------------------------------------ */

  function open(id, title, content, options = {}) {
    const existing = windows.get(id);
    if (existing) {
      // Reopening refreshes the content and raises the window rather than
      // stacking a duplicate on top of itself.
      if (content) existing.pane.setContent(content);
      focus(id);
      return existing.pane;
    }

    const index = windows.size;
    const pane = createWindowPane({
      document: doc,
      title,
      body: content,
      x: options.x ?? (60 + index * 26),
      y: options.y ?? (80 + index * 26),
      width: options.width ?? 460,
      onClose: () => { windows.delete(id); if (focusedId === id) focusedId = null; renderTaskbar(); },
    });

    const record = {
      id,
      title,
      pane,
      z: 0,
      minimized: false,
      maximized: false,
      restore: null,
    };

    windows.set(id, record);
    desktop.appendChild(pane.root);

    pane.root.addEventListener('pointerdown', () => focus(id));

    focus(id);
    renderTaskbar();
    return pane;
  }

  function close(id) {
    const record = windows.get(id);
    if (!record) return false;
    record.pane.close();
    windows.delete(id);
    if (focusedId === id) focusedId = null;
    renderTaskbar();
    return true;
  }

  function closeAll() {
    const count = windows.size;
    for (const record of [...windows.values()]) record.pane.close();
    windows.clear();
    focusedId = null;
    renderTaskbar();
    return count;
  }

  function minimize(id) {
    const record = windows.get(id);
    if (!record) return false;

    record.minimized = true;
    record.pane.root.classList.add('is-minimized');
    record.pane.root.hidden = true;

    if (focusedId === id) {
      focusedId = null;
      // Focus the topmost window that is still visible.
      const visible = [...windows.values()]
        .filter((w) => !w.minimized)
        .sort((a, b) => b.z - a.z)[0];
      if (visible) focus(visible.id);
    }

    renderTaskbar();
    return true;
  }

  function restore(id) {
    const record = windows.get(id);
    if (!record) return false;
    return focus(id);
  }

  function toggleMaximize(id) {
    const record = windows.get(id);
    if (!record) return false;

    if (record.maximized) {
      // Put it back where it was before it filled the desktop.
      if (record.restore) setGeometry(record, record.restore);
      record.maximized = false;
      record.pane.root.classList.remove('is-maximized');
      return true;
    }

    snapshotGeometry(record);
    const view = viewport();
    setGeometry(record, { left: 0, top: 0, width: view.width, height: view.height - 34 });
    record.maximized = true;
    record.pane.root.classList.add('is-maximized');
    focus(id);
    return true;
  }

  /**
   * Arrange every window in a grid, so nothing can hide behind anything else.
   * Columns are chosen to keep each cell close to 16:9.
   */
  function tile() {
    const ids = [...windows.keys()];
    if (!ids.length) return 0;

    const view = viewport();
    const columns = Math.max(1, Math.ceil(Math.sqrt(ids.length)));
    const rows = Math.ceil(ids.length / columns);
    const cellWidth = Math.floor(view.width / columns);
    const cellHeight = Math.floor((view.height - 34) / rows);

    ids.forEach((id, index) => {
      const record = windows.get(id);
      record.minimized = false;
      record.maximized = false;
      record.pane.root.hidden = false;
      record.pane.root.classList.remove('is-minimized', 'is-maximized');

      setGeometry(record, {
        left: (index % columns) * cellWidth,
        top: Math.floor(index / columns) * cellHeight,
        width: cellWidth - 4,
        height: cellHeight - 4,
      });
    });

    renderTaskbar();
    return ids.length;
  }

  /** Stagger the windows so every title bar stays visible and clickable. */
  function cascade() {
    const ids = [...windows.keys()];
    ids.forEach((id, index) => {
      const record = windows.get(id);
      record.minimized = false;
      record.maximized = false;
      record.pane.root.hidden = false;
      record.pane.root.classList.remove('is-minimized', 'is-maximized');
      setGeometry(record, { left: 50 + index * 28, top: 70 + index * 28 });
    });
    renderTaskbar();
    return ids.length;
  }

  /** The next / previous window in taskbar order - Alt+Tab for panes. */
  function cycle(delta = 1) {
    const ids = [...windows.keys()];
    if (ids.length < 2) return false;

    const index = ids.indexOf(focusedId);
    const next = (index + delta + ids.length) % ids.length;
    return focus(ids[next]);
  }

  renderTaskbar();

  return {
    root,
    desktop,
    taskbar,
    open,
    close,
    closeAll,
    focus,
    minimize,
    restore,
    toggleMaximize,
    tile,
    cascade,
    cycle,
    has: (id) => windows.has(id),
    list: () => [...windows.values()].map((w) => ({
      id: w.id,
      title: w.title,
      z: w.z,
      minimized: w.minimized,
      maximized: w.maximized,
    })),
    focused: () => focusedId,
    get: (id) => (windows.has(id) ? windows.get(id).pane : null),
    count: () => windows.size,
    renderTaskbar,
  };
}

/* --------------------------------------------------------------------------
 * GameMenu - the full-screen title and pause menu
 * -------------------------------------------------------------------------- */

/**
 * Normalise a menu tree into entries the caller owns.
 *
 * Called once when a game menu is created, so every later read - rendering,
 * highlighting, toggling a switch - sees the same objects. Without this the
 * menu would rebuild its tree on each render and any value the player changed
 * would be discarded.
 *
 * @param {object} menu
 * @returns {{items: object[]}}
 */
function normaliseGameMenu(menu) {
  const normalise = (entry) => {
    // A bare '-' is the separator shorthand.
    if (entry === '-') return { id: '-', label: '', type: 'separator', enabled: false, children: [] };

    const children = Array.isArray(entry.children) ? entry.children.map(normalise) : [];

    return {
      id: entry.id || entry.action || entry.label,
      label: entry.label || '',
      action: entry.action || '',
      hint: entry.hint || '',
      value: entry.value === undefined ? null : entry.value,
      // `min`/`max`/`step` only matter for sliders; they are carried through
      // so the renderer does not need a second lookup table.
      min: entry.min === undefined ? 0 : entry.min,
      max: entry.max === undefined ? 100 : entry.max,
      step: entry.step === undefined ? 10 : entry.step,
      enabled: entry.enabled !== false,
      type: entry.type
        || (entry.label === '-' ? 'separator'
          : children.length ? 'submenu'
            : entry.toggle ? 'toggle'
              : entry.slider ? 'slider' : 'item'),
      children,
    };
  };

  return { items: (menu && Array.isArray(menu.items) ? menu.items : []).map(normalise) };
}

/**
 * Compile an already-normalised game menu into the levels currently on screen.
 *
 * Each column is a complete menu rather than a dropdown, so the result is one
 * group per level and the renderer can lay the levels out side by side. The
 * entry objects are shared, not copied, so a value the player changes on a
 * slider survives the next render.
 *
 * @param {object} menu    normalised menu (see normaliseGameMenu)
 * @param {string[]} path  ids of the submenus drilled into, outermost first
 * @returns {Array<{level:number, parentId:string|null, entries:object[]}>}
 */
function buildGameMenuLevels(menu, path = []) {
  if (!menu || !Array.isArray(menu.items)) return [];

  const levels = [{
    level: 0,
    parentId: null,
    entries: menu.items,
  }];

  let current = menu.items;
  for (const id of path) {
    // Match case-insensitively: a path may be written by hand (or derived from
    // a label), and a casing difference should not silently stop the descent.
    const wanted = String(id).toLowerCase();
    const parent = current.find((e) => e.type === 'submenu'
      && e.children.length
      && (String(e.id).toLowerCase() === wanted || e.label.toLowerCase() === wanted));
    if (!parent) break;
    levels.push({
      level: levels.length,
      parentId: parent.id,
      entries: parent.children,
    });
    current = parent.children;
  }

  return levels;
}

/**
 * Where a slider's value sits within its own range, as 0-100.
 * A degenerate range (min === max) reads as full rather than dividing by zero.
 */
function scaledPercent(entry) {
  const min = Number(entry.min) || 0;
  const max = entry.max === undefined || entry.max === null ? 100 : Number(entry.max);
  const span = max - min;
  if (span <= 0) return 100;
  const value = Number(entry.value) || 0;
  return Math.max(0, Math.min(100, ((value - min) / span) * 100));
}

/**
 * The first selectable entry at or after `from`, wrapping around.
 * Separators and non-interactive labels are skipped so arrow keys never park
 * the cursor on something the player cannot activate.
 */
function nextSelectable(entries, from, delta) {
  if (!entries.length) return -1;
  const selectable = (e) => e.type !== 'separator' && e.type !== 'label' && e.enabled;

  let index = from;
  for (let guard = 0; guard <= entries.length; guard += 1) {
    index = (index + delta + entries.length) % entries.length;
    if (selectable(entries[index])) return index;
  }
  return selectable(entries[from]) ? from : -1;
}

/**
 * A full-screen game menu: a title screen and an in-game pause menu.
 *
 * Behaves the way a console game menu does, because that is what the operator
 * of a terminal console expects from something called a menu:
 *
 *   - Up/Down move through the entries, skipping separators and disabled rows
 *   - Right or Enter drills into a submenu, opening the next column
 *   - Left or Escape backs out one level, and Escape at the top closes
 *   - Enter activates an item, toggles a switch, or grabs a slider
 *   - Left/Right on a slider or toggle changes the value in place
 *
 * The menu is described by data, so the same structure renders a title screen,
 * a pause menu or a settings pane.
 *
 * @param {object} opts
 * @param {Document} opts.document
 * @param {string} opts.title
 * @param {string} [opts.subtitle]
 * @param {object} opts.menu         root menu object
 * @param {Function} opts.onCommand  called with (action, entry)
 * @param {Function} [opts.onChange] called with (entry, value) for sliders/toggles
 * @param {Function} [opts.onClose]
 */
function createGameMenu(opts) {
  const doc = opts.document;
  const onCommand = opts.onCommand || (() => {});
  const onChange = opts.onChange || (() => {});
  const onClose = opts.onClose || (() => {});

  /**
   * Normalise the menu tree once, into objects the menu owns.
   *
   * This has to happen up front: `buildGameMenuLevels` produces a fresh list on
   * every call, so toggling a switch or dragging a slider would write to a
   * throwaway copy and the change would vanish on the next render.
   */
  const tree = normaliseGameMenu(opts.menu);

  const titleEl = el(doc, 'div', { class: 'mf-game-title', text: opts.title || '' });
  const subtitleEl = el(doc, 'div', { class: 'mf-game-subtitle', text: opts.subtitle || '' });
  const columns = el(doc, 'div', { class: 'mf-game-columns' });
  const hintEl = el(doc, 'div', { class: 'mf-game-hint', text: '' });

  const root = el(doc, 'div', { class: 'mf-game', role: 'dialog', 'aria-modal': 'true' }, [
    el(doc, 'div', { class: 'mf-game-panel' }, [titleEl, subtitleEl, columns, hintEl]),
  ]);

  // path[0] drives the first column, path[1] the second, and so on.
  let path = [];
  let highlight = 0;
  let open = false;

  const levels = () => buildGameMenuLevels(tree, path);

  /** The entries of the deepest open column - the ones the cursor walks. */
  function activeEntries() {
    const all = levels();
    return all.length ? all[all.length - 1].entries : [];
  }

  function render() {
    // Remove the previous columns explicitly. Setting textContent alone is not
    // enough: it clears text nodes but leaves the element children in place,
    // so each render would append a second copy of the menu.
    while (columns.firstChild) columns.removeChild(columns.firstChild);
    columns.textContent = '';

    const all = levels();

    all.forEach((group, levelIndex) => {
      const column = el(doc, 'div', { class: 'mf-game-column' });

      group.entries.forEach((entry, index) => {
        if (entry.type === 'separator') {
          column.appendChild(el(doc, 'div', { class: 'mf-game-sep' }));
          return;
        }

        const isActive = levelIndex === all.length - 1 && index === highlight;
        const row = el(doc, 'div', {
          class: 'mf-game-item'
            + `${isActive ? ' is-active' : ''}`
            + `${entry.enabled ? '' : ' is-disabled'}`
            + `${entry.type === 'submenu' ? ' has-children' : ''}`,
          role: 'menuitem',
        }, [
          el(doc, 'span', { class: 'mf-game-marker', text: isActive ? '\u25B8' : '' }),
          el(doc, 'span', { class: 'mf-game-label', text: entry.label }),
        ]);

        // Sliders and switches show their state on the row itself, the way
        // a game settings screen does instead of opening a dialog.
        if (entry.type === 'slider') {
          row.appendChild(el(doc, 'span', {
            class: 'mf-game-value',
            text: `${entry.value ?? ''}`.padEnd(3, ' '),
          }));
          row.appendChild(el(doc, 'span', { class: 'mf-game-gauge' }, [
            el(doc, 'span', {
              class: 'mf-game-gauge-fill',
              // The filled width is the value's position inside its own range,
              // so a min/max pair other than 0-100 still renders correctly.
              style: { width: `${scaledPercent(entry)}%` },
            }),
          ]));
        } else if (entry.type === 'toggle') {
          row.appendChild(el(doc, 'span', {
            class: `mf-game-switch${entry.value ? ' is-on' : ''}`,
            text: entry.value ? 'ON' : 'OFF',
          }));
        } else if (entry.hint) {
          row.appendChild(el(doc, 'span', { class: 'mf-game-item-hint', text: entry.hint }));
        }

        if (entry.type === 'submenu') {
          row.appendChild(el(doc, 'span', { class: 'mf-game-arrow', text: '\u25B8' }));
        }

        row.addEventListener('mouseenter', () => {
          if (levelIndex !== all.length - 1) return;
          highlight = index;
          render();
        });
        row.addEventListener('click', (ev) => {
          ev.stopPropagation();
          if (levelIndex !== all.length - 1) return;
          highlight = index;
          activate(entry);
        });

        column.appendChild(row);
      });

      columns.appendChild(column);
    });

    const active = activeEntries()[highlight];
    hintEl.textContent = active
      ? (active.enabled ? (active.hint || active.action || '') : 'Unavailable')
      : '';
  }

  /** Descend into a submenu, resetting the cursor to a selectable row. */
  function enter(entry) {
    if (!entry.enabled || !entry.children.length) return;
    path = path.concat(entry.id);
    highlight = Math.max(0, nextSelectable(activeEntries(), -1, 1));
    render();
  }

  /** Back out one level. Returns false when already at the top. */
  function back() {
    if (!path.length) return false;
    const leaving = path[path.length - 1];
    path = path.slice(0, -1);

    // Put the cursor back on the submenu we just left, not on row zero.
    const entries = activeEntries();
    const index = entries.findIndex((e) => e.id === leaving);
    highlight = index >= 0 ? index : 0;
    render();
    return true;
  }

  /** Run an entry: drill in, toggle, or fire the action. */
  function activate(entry) {
    if (!entry || !entry.enabled || entry.type === 'separator') return;

    if (entry.type === 'submenu') { enter(entry); return; }

    if (entry.type === 'toggle') {
      const value = !entry.value;
      entry.value = value;
      onChange(entry, value);
      render();
      return;
    }

    if (entry.type === 'slider') {
      stepValue(entry, 1);
      return;
    }

    onCommand(entry.action, entry);
  }

  /**
   * Nudge a slider or a switch along its own range.
   *
   * The bounds come from the entry, so a slider declared min=0 max=255 step=5
   * behaves as written rather than being clamped to an arbitrary 0-100.
   */
  function stepValue(entry, delta) {
    const step = Number(entry.step) || 1;
    const min = Number(entry.min) || 0;
    const max = entry.max === undefined || entry.max === null ? 100 : Number(entry.max);
    const current = Number(entry.value) || 0;
    const next = Math.max(min, Math.min(max, current + delta * step));

    if (next === current) return;   // already at the end of the range

    entry.value = next;
    onChange(entry, next);
    render();
  }

  function move(delta) {
    const entries = activeEntries();
    highlight = nextSelectable(entries, highlight, delta);
    render();
  }

  /**
   * Key routing. Returns true when the key was consumed.
   * This is the menu's own scope, so the caller only forwards while it is up.
   */
  function handleKey(event) {
    if (!open) return false;
    const entry = activeEntries()[highlight];

    switch (event.key) {
      case 'ArrowDown': move(1); return true;
      case 'ArrowUp': move(-1); return true;

      case 'ArrowRight':
        if (entry && (entry.type === 'slider' || entry.type === 'toggle')) {
          stepValue(entry, 1);
          return true;
        }
        if (entry && entry.type === 'submenu') { enter(entry); return true; }
        return true;

      case 'ArrowLeft':
        if (entry && (entry.type === 'slider' || entry.type === 'toggle')) {
          stepValue(entry, -1);
          return true;
        }
        back();
        return true;

      case 'Enter':
      case ' ':
        activate(entry);
        return true;

      case 'Escape':
        // Escape unwinds the menu one level at a time, which is what players
        // expect; only the top level actually dismisses the menu.
        if (!back()) { close(); onClose(); }
        return true;

      default:
        return false;
    }
  }

  function openMenu(nextPath = []) {
    open = true;
    path = nextPath;
    highlight = Math.max(0, nextSelectable(activeEntries(), -1, 1));
    render();
    return api;
  }

  function close() {
    open = false;
    path = [];
    highlight = 0;
    render();
  }

  const api = {
    root,
    open: openMenu,
    close,
    handleKey,
    activate,
    move,
    enter,
    back,
    isOpen: () => open,
    getPath: () => path.slice(),
    getHighlight: () => highlight,
    getActiveLabel: () => {
      const entry = activeEntries()[highlight];
      return entry ? entry.label : null;
    },
    /** Entries of the deepest column, as a read-only snapshot. */
    getActiveEntries: () => activeEntries().map((e) => ({ ...e, children: undefined })),
    /** The live entry the cursor is on - the one toggles and sliders write to. */
    getActiveEntry: () => activeEntries()[highlight] || null,
    /** Every level currently open, as a read-only snapshot. */
    levels: () => levels().map((g) => ({ ...g, entries: g.entries.map((e) => ({ ...e, children: undefined })) })),
    render,
  };

  render();
  return api;
}

/* --------------------------------------------------------------------------
 * Exports
 * -------------------------------------------------------------------------- */

const MFUI = {
  el,
  isHidden,
  setHidden,
  buildMenuModel,
  filterRows,
  sortRows,
  groupCount,
  rankPalette,
  createMenuBar,
  createTableView,
  createTabs,
  createDialog,
  createToastHost,
  createCommandPalette,
  createStatusBar,
  createBootLoader,
  createLoadingScreen,
  createWindowPane,
  createGameMenu,
  buildGameMenuLevels,
  normaliseGameMenu,
  nextSelectable,
  createDefinitionList,
  createDistributionList,
  createSection,
  createWindowManager,
};

if (typeof module !== 'undefined' && module.exports) module.exports = MFUI;
if (typeof window !== 'undefined') window.MFUI = MFUI;
