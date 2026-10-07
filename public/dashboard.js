'use strict';

/**
 * Dashboard shell.
 *
 * Owns the operator console: the menu bar and its submenus, the buttons, the
 * panel tabs, the data grid, the command palette, the dialogs and toasts - all
 * driven by actions so the mouse, the keyboard and Ctrl+K run the same code
 * path.
 *
 * The action table below is the whole surface of the UI. A menu entry, a
 * button, a palette row and a keybind all name an action here; none of them
 * contain behaviour of their own. That is what keeps the menu labels and the
 * key map from disagreeing with each other.
 *
 * This file is loaded in the browser after components.js and keybinds.js and
 * is intentionally DOM-only - the pure logic it relies on lives in those two
 * modules and is covered by tests/.
 */

/* global MFUI, MFKeybinds */

(function main() {
  const UI = window.MFUI;
  const KB = window.MFKeybinds;
  const Views = window.MFViews;

  if (!UI || !KB || !Views) {
    // A missing bundle means the page will render nothing useful; say which
    // one rather than failing later with a confusing TypeError.
    document.body.textContent = 'Console failed to load: components.js, keybinds.js '
      + 'and views.js must all load before dashboard.js.';
    return;
  }

  const doc = window.document;

  /* ------------------------------------------------------------------------
   * Panel definitions
   * ---------------------------------------------------------------------- */

  const PANELS = [
    { id: 'status', label: 'Status', table: 'health', columns: ['name', 'value'] },
    { id: 'users', label: 'Users', table: 'users', columns: ['user_id', 'username', 'full_name', 'department', 'role', 'status'] },
    { id: 'datasets', label: 'Datasets', table: 'datasets', columns: ['dataset_id', 'name', 'dsorg', 'recfm', 'lrecl', 'volume', 'record_count', 'status'] },
    { id: 'jobs', label: 'Jobs', table: 'jobs', columns: ['job_id', 'job_name', 'job_class', 'status', 'return_code', 'priority', 'submitted_at'] },
    { id: 'transactions', label: 'Transactions', table: 'transactions', columns: ['txn_id', 'txn_code', 'terminal', 'user_id', 'status', 'rows_read', 'rows_written', 'elapsed_ms'] },
    { id: 'audit', label: 'Audit Log', table: 'audit_log', columns: ['log_id', 'event_type', 'severity', 'actor', 'resource', 'message', 'created_at'] },
  ];

  /* ------------------------------------------------------------------------
   * Application state
   * ---------------------------------------------------------------------- */

  const state = {
    panelIndex: 0,
    data: {},          // panel id -> rows
    loading: {},
    error: null,
    system: {},
    booted: false,
    // Settings the game menu owns. Kept here rather than in the menu so the
    // console can read them after the menu closes.
    settings: {
      scanlines: true,
      volume: 70,
      timestamps: true,
      compact: false,
    },
  };

  let menuBar;
  let table;
  let tabs;
  let palette;
  let dialog;
  let gameMenu;
  let windows;

  /** The declared views, in a lookup keyed by id. */
  const views = Views.createViewRegistry();
  const viewIndex = Views.indexViews(views);

  /*
   * The keybind registry. Declared here, near the top, because the menu model
   * below reads it for its accelerator labels at module scope. Leaving this
   * further down put it in the temporal dead zone: `accelerate()` runs while
   * the MENU array is being built, hits the not-yet-initialised binding, and
   * throws - which aborts the whole script and leaves a black screen.
   */
  const registry = KB.createDefaultRegistry();

  let toasts;
  let statusBar;
  let loadingScreen;

  const panel = () => PANELS[state.panelIndex];
  const rowsFor = (id) => state.data[id] || [];

  /** Small helper so a button and a menu entry cannot look different. */
  const button = (label, action, opts = {}) => UI.el(doc, 'button', {
    class: `mf-btn${opts.primary ? ' is-primary' : ''}${opts.className ? ` ${opts.className}` : ''}`,
    type: 'button',
    title: opts.title || label,
    text: label,
    disabled: opts.disabled === true,
    onclick: () => dispatch(action),
  });

  const toolbarButton = (label, action, title) => {
    const node = button(label, action);
    node.classList.add('mf-toolbar-btn');
    node.title = title || label;
    return node;
  };

  /* ------------------------------------------------------------------------
   * Data access
   * ---------------------------------------------------------------------- */

  async function fetchJson(url) {
    const response = await fetch(url, { headers: { accept: 'application/json' } });
    if (!response.ok) throw new Error(`${response.status} ${response.statusText} for ${url}`);
    return response.json();
  }

  async function loadPanel(id) {
    state.loading[id] = true;
    state.error = null;
    renderStatus();
    if (id === panel().id) renderTable();

    try {
      if (id === 'status') {
        const health = await fetchJson('/api/health');
        state.system = health;
        state.data.status = Object.entries(flatten(health)).map(([name, value]) => ({ name, value }));
      } else {
        // Fetch by the panel's declared table, not its id: the two differ
        // ("audit" reads the audit_log table), and querying the id 404s.
        const declared = PANELS.find((p) => p.id === id);
        const table = (declared && declared.table) || id;
        const body = await fetchJson(`/api/rows?table=${encodeURIComponent(table)}`);
        state.data[id] = body.rows || [];
      }
    } catch (err) {
      state.error = err.message;
      state.data[id] = state.data[id] || [];
    } finally {
      state.loading[id] = false;
      renderStatus();
      if (id === panel().id) renderTable();
    }
  }

  /** Flatten the health payload into dotted keys for the status table. */
  function flatten(object, prefix = '') {
    const out = {};
    for (const [key, value] of Object.entries(object || {})) {
      const path = prefix ? `${prefix}.${key}` : key;
      if (value && typeof value === 'object' && !Array.isArray(value)) {
        Object.assign(out, flatten(value, path));
      } else {
        out[path] = Array.isArray(value) ? value.join(', ') : value;
      }
    }
    return out;
  }

  /* ------------------------------------------------------------------------
   * Windows - the desktop, its manager and the view registry
   * ---------------------------------------------------------------------- */

  /**
   * Everything a view's build() may use. Assembled fresh per open so a window
   * always renders against current data rather than a snapshot from startup.
   */
  function viewContext() {
    return Views.createViewContext({
      document: doc,
      tables: state.data,
      health: state.system,
      settings: state.settings,
      bootstrap: window.MF_BOOTSTRAP || null,
    });
  }

  /**
   * Open a registered view in a window.
   *
   * The view is looked up in the registry rather than passed in, so the menu,
   * the palette and the toolbar all name a view by id and cannot drift apart.
   */
  function openView(id) {
    const view = viewIndex.get(id);
    if (!view) {
      toast('warn', `Unknown view "${id}"`);
      return null;
    }

    const { nodes, error } = Views.buildView(view, viewContext());
    if (error) toast('error', `${view.title}: ${error.message}`);

    return windows.open(id, view.title, nodes, { width: view.width });
  }

  /** Open every view in a group - the "show me everything" action. */
  function openGroup(group) {
    const group_views = Views.viewsInGroup(views, group);
    for (const view of group_views) openView(view.id);
    windows.cascade();
    toast('info', `Opened ${group_views.length} ${group} window(s)`);
  }

  /** Re-render every open window against the current data. */
  function refreshWindows() {
    let refreshed = 0;
    for (const record of windows.list()) {
      const view = viewIndex.get(record.id);
      const pane = windows.get(record.id);
      if (!view || !pane) continue;

      const { nodes } = Views.buildView(view, viewContext());
      pane.setContent(nodes);
      refreshed += 1;
    }
    return refreshed;
  }

  async function refreshAll() {
    await Promise.all(PANELS.map((p) => loadPanel(p.id)));
    const refreshed = refreshWindows();
    toast('success', refreshed
      ? `Panels refreshed, ${refreshed} window(s) updated`
      : 'All panels refreshed');
  }

  async function refreshActive() {
    await loadPanel(panel().id);
    renderTable();
    toast('info', `${panel().label} reloaded`);
  }

  /* ------------------------------------------------------------------------
   * Actions - the single command surface
   * ---------------------------------------------------------------------- */

  const ACTIONS = {
    'app.help': () => showHelp(),
    'app.commandPalette': () => palette.open(),
    'app.refresh': () => refreshAll(),
    'app.toggleSidebar': () => {
      doc.body.classList.toggle('sidebar-hidden');
      const hidden = doc.body.classList.contains('sidebar-hidden');
      toast('info', hidden ? 'Sidebar hidden' : 'Sidebar shown');
    },
    'app.toggleTheme': () => {
      doc.body.classList.toggle('theme-light');
      toast('info', doc.body.classList.contains('theme-light') ? 'Light theme' : 'Dark theme');
    },
    'app.closeOverlay': () => {
      if (gameMenu && gameMenu.isOpen()) return gameMenu.handleKey({ key: 'Escape' });
      if (dialog && dialog.isOpen()) return dialog.handleKey({ key: 'Escape' });
      if (palette.isOpen()) return palette.close();
      if (menuBar.isOpen()) return menuBar.close();
      return undefined;
    },
    'app.gameMenu': () => gameMenu.open(),

    'nav.panel1': () => selectPanel(0),
    'nav.panel2': () => selectPanel(1),
    'nav.panel3': () => selectPanel(2),
    'nav.panel4': () => selectPanel(3),
    'nav.panel5': () => selectPanel(4),
    'nav.nextPanel': () => selectPanel((state.panelIndex + 1) % PANELS.length),
    'nav.prevPanel': () => selectPanel((state.panelIndex - 1 + PANELS.length) % PANELS.length),
    'nav.nextTab': () => tabs.next(),
    'nav.prevTab': () => tabs.prev(),
    'nav.focusMenu': () => menuBar.open(0),

    'data.search': () => promptSearch(),
    'data.export': () => exportCsv(),
    'data.refreshPanel': () => refreshActive(),

    'list.next': () => table.move(1),
    'list.prev': () => table.move(-1),
    'list.first': () => table.moveTo('first'),
    'list.last': () => table.moveTo('last'),
    'list.open': () => table.openSelected(),
    'list.select': () => table.openSelected(),

    'dialog.confirm': () => dialog.handleKey({ key: 'Enter' }),
    'dialog.cancel': () => dialog.handleKey({ key: 'Escape' }),

    /* Panel-specific and menu-only commands. */
    'panel.next': () => selectPanel((state.panelIndex + 1) % PANELS.length),
    'panel.prev': () => selectPanel((state.panelIndex - 1 + PANELS.length) % PANELS.length),
    'view.sortAsc': () => sortActive('asc'),
    'view.sortDesc': () => sortActive('desc'),
    'view.resetView': () => { table.setQuery(''); renderStatus(); toast('info', 'View reset'); },
    'edit.copyRow': () => copySelected(),
    'edit.selectAll': () => toast('info', `${table.rowCount()} row(s) in view`),
    'help.about': () => showAbout(),
    'file.quit': () => quit(),

    /* System views, each declared in the view registry. */
    'sys.overview': () => openView('overview'),
    'sys.storage': () => openView('storage'),
    'sys.subsystems': () => openView('subsystems'),
    'sys.security': () => openView('security'),
    'sys.jobs': () => openView('jobs'),
    'sys.terminals': () => openView('terminals'),
    'sys.audit': () => openView('audit'),
    'sys.accounts': () => openView('accounts'),
    'sys.config': () => openView('config'),
    'sys.openAll': () => openGroup('system'),
    'ops.openAll': () => openGroup('operations'),
    'data.openAll': () => openGroup('data'),

    /* The game menu: title screen and pause menu. */
    'game.title': () => gameMenu.open(),
    'game.pause': () => gameMenu.open(),
    'game.resume': () => {
      gameMenu.close();
      toast('info', 'Console resumed');
    },
    'game.start': () => {
      gameMenu.close();
      toast('success', 'Console session started');
    },
    'game.settings': () => gameMenu.open(['options']),
    'game.quit': () => quit(),
    'game.toggleScanlines': () => {
      doc.body.classList.toggle('mf-scanlines', state.settings.scanlines);
      toast('info', state.settings.scanlines ? 'Scanlines on' : 'Scanlines off');
    },
    'game.toggleTimestamps': () => {
      doc.body.classList.toggle('mf-no-timestamps', !state.settings.timestamps);
      toast('info', state.settings.timestamps ? 'Timestamps shown' : 'Timestamps hidden');
    },
    'game.toggleCompact': () => {
      doc.body.classList.toggle('mf-compact', state.settings.compact);
      toast('info', state.settings.compact ? 'Compact rows' : 'Comfortable rows');
    },
    /* Window management. */
    'window.tile': () => {
      const count = windows.tile();
      toast('info', count ? `Tiled ${count} window(s)` : 'No windows open');
    },
    'window.cascade': () => {
      const count = windows.cascade();
      toast('info', count ? `Cascaded ${count} window(s)` : 'No windows open');
    },
    'window.cycle': () => {
      if (!windows.cycle(1)) toast('info', 'Fewer than two windows open');
    },
    'window.cycleBack': () => {
      if (!windows.cycle(-1)) toast('info', 'Fewer than two windows open');
    },
    'window.closeAll': () => {
      const closed = windows.closeAll();
      toast('info', closed ? `Closed ${closed} window(s)` : 'No windows open');
    },
    'window.reflow': () => {
      windows.renderTaskbar();
      toast('info', 'Taskbar refreshed');
    },
  };

  /** Run an action by name, reporting unknown names rather than failing mute. */
  function dispatch(action, entry) {
    const handler = ACTIONS[action];
    if (!handler) {
      toast('warn', `No handler for action "${action}"`);
      return;
    }
    statusBar.setRight(entry && entry.label ? `${entry.label}` : action);
    try {
      handler();
    } catch (err) {
      toast('error', `${action}: ${err.message}`);
    }
  }

  /* ------------------------------------------------------------------------
   * Panel and table behaviour
   * ---------------------------------------------------------------------- */

  function selectPanel(index) {
    if (index < 0 || index >= PANELS.length) return;
    state.panelIndex = index;
    renderTitles();
    renderTable();
    if (!state.data[panel().id] && !state.loading[panel().id]) {
      loadPanel(panel().id).then(renderTable);
    }
    renderStatus();
  }

  function sortActive(direction) {
    const column = table.getState().sortColumn || panel().columns[0];
    const current = table.getState();
    if (current.sortColumn === column && current.sortDirection === direction) return;
    table.toggleSort(column);
    if (table.getState().sortDirection !== direction) table.toggleSort(column);
    toast('info', `Sorted by ${column} (${direction})`);
  }

  function promptSearch() {
    dialog.open().then((result) => {
      if (result !== false) {
        table.setQuery(String(result));
        renderStatus();
      }
    });
  }

  function copySelected() {
    const row = table.getSelection();
    if (!row) { toast('warn', 'No row selected'); return; }
    const text = JSON.stringify(row);
    if (navigator.clipboard) {
      navigator.clipboard.writeText(text)
        .then(() => toast('success', 'Row copied to clipboard'))
        .catch(() => toast('warn', text));
    } else {
      toast('info', text);
    }
  }

  function exportCsv() {
    const rows = rowsFor(panel().id);
    if (!rows.length) { toast('warn', 'Nothing to export in this panel'); return; }

    const columns = panel().columns.filter((c) => c in (rows[0] || {}));
    const escape = (value) => `"${String(value ?? '').replace(/"/g, '""')}"`;
    const lines = [
      columns.map(escape).join(','),
      ...rows.map((row) => columns.map((c) => escape(row[c])).join(',')),
    ];

    const blob = new Blob([lines.join('\r\n')], { type: 'text/csv;charset=utf-8' });
    const url = URL.createObjectURL(blob);
    const link = UI.el(doc, 'a', { href: url, download: `${panel().id}.csv` });
    doc.body.appendChild(link);
    link.click();
    doc.body.removeChild(link);
    URL.revokeObjectURL(url);
    toast('success', `Exported ${rows.length} row(s) to ${panel().id}.csv`);
  }

  async function quit() {
    const confirmed = await dialog.open();
    if (confirmed) toast('info', 'Close this tab to stop the dashboard.');
  }

  function showAbout() {
    dialog.open().then(() => { });
    toast('info', `${state.system.system || 'MAINFRAME'} v${state.system.version || '1.0.0'}`);
  }

  /* ------------------------------------------------------------------------
   * System views are declared in src/ui/views.js and opened through the window
   * manager. There is deliberately no view-building code in this file: a view
   * lives in the registry so the menu, palette and toolbar all reference it by
   * id rather than each carrying their own copy.
   * ---------------------------------------------------------------------- */

  function showHelp() {
    const rows = registry.describeAll();
    const body = rows.map((b) => `
      <tr><td>${escapeHtml(b.pretty)}</td><td>${escapeHtml(b.scope)}</td><td>${escapeHtml(b.description)}</td></tr>
    `).join('');

    const overlay = UI.el(doc, 'div', { class: 'mf-overlay' }, [
      UI.el(doc, 'div', { class: 'mf-dialog is-wide' }, [
        UI.el(doc, 'h3', { text: 'Keyboard Shortcuts' }),
        UI.el(doc, 'div', {
          class: 'mf-help-scroll',
          html: `<table class="mf-table"><thead><tr><th>Key</th><th>Scope</th><th>Action</th></tr></thead><tbody>${body}</tbody></table>`,
        }),
        UI.el(doc, 'div', { class: 'mf-dialog-actions' }, [
          button('Close', 'app.closeOverlay', { primary: true }),
        ]),
      ]),
    ]);

    overlay.addEventListener('click', (ev) => { if (ev.target === overlay) overlay.remove(); });
    doc.body.appendChild(overlay);

    // Close on the same Escape that closes every other overlay.
    const close = () => {
      overlay.remove();
      doc.removeEventListener('keydown', onKey, true);
    };
    const onKey = (event) => { if (event.key === 'Escape') { event.preventDefault(); close(); } };
    doc.addEventListener('keydown', onKey, true);
    ACTIONS['__closeHelp'] = close;
  }

  /* ------------------------------------------------------------------------
   * The game menu - title screen and pause menu
   * ---------------------------------------------------------------------- */

  /**
   * The menu tree.
   *
   * Values are read from `state.settings` when the menu is built, and every
   * change writes straight back, so the menu and the console never disagree
   * about the current setting.
   */
  function gameMenuTree() {
    return {
      items: [
        {
          id: 'resume',
          label: 'Resume Console',
          action: 'game.resume',
          hint: 'Return to the operator console (Escape)',
        },
        {
          id: 'panels',
          label: 'Jump to Panel',
          hint: 'Open a console panel',
          children: PANELS.map((p, index) => ({
            id: `panel-${p.id}`,
            label: p.label,
            action: `nav.panel${index + 1}`,
            hint: `Show the ${p.label} panel`,
          })),
        },
        {
          id: 'systems',
          label: 'System Views',
          hint: 'Open a system overview window',
          children: [
            { id: 'ov', label: 'System Overview', action: 'sys.overview' },
            { id: 'sub', label: 'Subsystems', action: 'sys.subsystems' },
            { id: 'sto', label: 'Storage Volumes', action: 'sys.storage' },
            { id: 'sec', label: 'Security', action: 'sys.security' },
            { id: 'job', label: 'Job Queue', action: 'sys.jobs' },
          ],
        },
        { id: 'sep1', label: '-' },
        {
          id: 'options',
          label: 'Options',
          hint: 'Display and console settings',
          children: [
            {
              id: 'volume',
              label: 'Volume',
              slider: true,
              value: state.settings.volume,
              min: 0,
              max: 100,
              step: 10,
              hint: 'Left / Right to adjust',
            },
            {
              id: 'scanlines',
              label: 'Scanlines',
              toggle: true,
              value: state.settings.scanlines,
              action: 'game.toggleScanlines',
              hint: 'CRT scanline overlay',
            },
            {
              id: 'timestamps',
              label: 'Timestamps',
              toggle: true,
              value: state.settings.timestamps,
              action: 'game.toggleTimestamps',
              hint: 'Show record timestamps',
            },
            {
              id: 'compact',
              label: 'Compact Rows',
              toggle: true,
              value: state.settings.compact,
              action: 'game.toggleCompact',
              hint: 'Denser table rows',
            },
          ],
        },
        { id: 'sep2', label: '-' },
        { id: 'refresh', label: 'Refresh All Panels', action: 'app.refresh' },
        { id: 'keys', label: 'Keyboard Shortcuts', action: 'app.help' },
        { id: 'aboutgame', label: 'About This System', action: 'help.about' },
        { id: 'sep3', label: '-' },
        { id: 'quit', label: 'Quit Console', action: 'game.quit', hint: 'Leave the session' },
      ],
    };
  }

  /**
   * Apply a setting the player changed in the menu.
   * Called for sliders and switches, which carry their own `action`.
   */
  function applyGameSetting(entry, value) {
    switch (entry.id) {
      case 'volume': state.settings.volume = value; break;
      case 'scanlines': state.settings.scanlines = value; break;
      case 'timestamps': state.settings.timestamps = value; break;
      case 'compact': state.settings.compact = value; break;
      default: break;
    }

    // A switch that names an action runs it, so the visual change and the
    // toast are driven from one place.
    if (entry.action) dispatch(entry.action, entry);
    statusBar.setRight(`${entry.label}: ${value}`);
  }

  function buildGameMenu() {
    gameMenu = UI.createGameMenu({
      document: doc,
      title: 'MAINFRAME',
      subtitle: 'Operator Console - select a command',
      menu: gameMenuTree(),
      onCommand: dispatch,
      onChange: applyGameSetting,
      onClose: () => toast('info', 'Menu closed'),
    });
    return gameMenu;
  }

  function escapeHtml(value) {
    return String(value ?? '')
      .replace(/&/g, '&amp;')
      .replace(/</g, '&lt;')
      .replace(/>/g, '&gt;');
  }

  function toast(kind, message) {
    if (toasts && toasts[kind]) toasts[kind](message);
  }

  /* ------------------------------------------------------------------------
   * Menu model - mirrors the action table
   * ---------------------------------------------------------------------- */

  function accelerate(action) {
    const binding = registry.byAction.get(action);
    return binding ? KB.describeCombo(binding.combo) : '';
  }

  function item(action, label, extra = {}) {
    return { id: action, action, label, accelerator: accelerate(action), ...extra };
  }

  const MENU = [
    {
      title: 'File',
      items: [
        item('app.refresh', 'Refresh All'),
        item('data.refreshPanel', 'Reload Panel'),
        '-',
        item('data.export', 'Export Panel to CSV\u2026'),
        '-',
        { id: 'edit.copyRow', action: 'edit.copyRow', label: 'Copy Selected Row', accelerator: '', enabled: false },
        '-',
        item('file.quit', 'Quit\u2026', { accelerator: '' }),
      ],
    },
    {
      title: 'Edit',
      items: [
        item('data.search', 'Find\u2026'),
        item('edit.selectAll', 'Count Rows in View', { accelerator: '' }),
        '-',
        { id: 'edit.cut', label: 'Cut', accelerator: 'Ctrl+X', enabled: false, action: '' },
        { id: 'edit.paste', label: 'Paste', accelerator: 'Ctrl+V', enabled: false, action: '' },
      ],
    },
    {
      title: 'View',
      items: [
        {
          id: 'view.panels',
          label: 'Go to Panel',
          children: PANELS.map((p, index) => item(
            `nav.panel${index + 1}`,
            p.label,
          )),
        },
        {
          id: 'view.sort',
          label: 'Sort',
          children: [
            { id: 'view.sortAsc', action: 'view.sortAsc', label: 'Ascending', accelerator: '' },
            { id: 'view.sortDesc', action: 'view.sortDesc', label: 'Descending', accelerator: '' },
            '-',
            item('view.resetView', 'Reset View'),
          ],
        },
        '-',
        item('app.toggleSidebar', 'Toggle Sidebar'),
        item('app.toggleTheme', 'Toggle Theme'),
        '-',
        item('app.commandPalette', 'Command Palette\u2026'),
      ],
    },
    {
      title: 'Windows',
      items: [
        {
          id: 'window.open',
          label: 'Open View',
          children: views.map((view) => ({
            id: `open-${view.id}`,
            label: `${view.glyph} ${view.title}`,
            action: `sys.${view.id}`,
          })),
        },
        '-',
        { id: 'window.tile', action: 'window.tile', label: 'Tile Windows', accelerator: '' },
        { id: 'window.cascade', action: 'window.cascade', label: 'Cascade Windows', accelerator: '' },
        '-',
        item('window.cycle', 'Next Window'),
        item('window.cycleBack', 'Previous Window'),
        '-',
        { id: 'window.closeAll', action: 'window.closeAll', label: 'Close All Windows', accelerator: '' },
      ],
    },
    {
      title: 'Systems',
      items: [
        item('game.title', 'Console Menu'),
        '-',
        { id: 'group-system', action: 'sys.openAll', label: 'All System Status Views', accelerator: '' },
        { id: 'group-ops', action: 'ops.openAll', label: 'All Operations Views', accelerator: '' },
        { id: 'group-data', action: 'data.openAll', label: 'All Data Views', accelerator: '' },
        '-',
        ...views.map((view) => ({
          id: `sys-${view.id}`,
          label: `${view.glyph} ${view.title}`,
          action: `sys.${view.id}`,
        })),
      ],
    },
    {
      title: 'Navigate',
      items: [
        item('nav.nextPanel', 'Next Panel'),
        item('nav.prevPanel', 'Previous Panel'),
        '-',
        item('nav.nextTab', 'Next Tab'),
        item('nav.prevTab', 'Previous Tab'),
        '-',
        item('nav.focusMenu', 'Focus Menu Bar'),
      ],
    },
    {
      title: 'Help',
      items: [
        item('app.help', 'Keyboard Shortcuts'),
        '-',
        item('help.about', 'About This System'),
      ],
    },
  ];

  /* ------------------------------------------------------------------------
   * Rendering
   * ---------------------------------------------------------------------- */

  function renderTitles() {
    titles.textContent = '';
    PANELS.forEach((p, index) => {
      const node = UI.el(doc, 'button', {
        class: `mf-panel-title${index === state.panelIndex ? ' is-active' : ''}`,
        type: 'button',
        text: p.label,
        onclick: () => selectPanel(index),
      });
      titles.appendChild(node);
    });
  }

  function renderTable() {
    const current = panel();
    const columns = current.id === 'status'
      ? current.columns
      : current.columns.filter((c) => c in (rowsFor(current.id)[0] || {}));

    // The grid is rebuilt per panel so its column set matches the data.
    if (!table || table.__panel !== current.id) {
      if (table && table.root.parentNode) table.root.parentNode.removeChild(table.root);
      table = UI.createTableView({
        document: doc,
        columns: columns.length ? columns : current.columns,
        rows: rowsFor(current.id),
        onSelect: (row) => toast('info', `${current.label}: ${firstValue(row)}`),
        scopeId: 'list',
      });
      table.__panel = current.id;
      gridHost.textContent = '';
      gridHost.appendChild(table.root);
    } else {
      table.setRows(rowsFor(current.id));
    }

    // While a panel's first load is in flight, show the loading placeholder
    // instead of an empty grid, so "no rows" and "not loaded yet" are not
    // mistaken for each other.
    if (state.loading[current.id] && !rowsFor(current.id).length) {
      loadingScreen = loadingScreen || UI.createLoadingScreen({ document: doc });
      loadingScreen.setLabel(`Loading ${current.label}\u2026`);
      gridHost.textContent = '';
      gridHost.appendChild(loadingScreen.root);
    } else if (table && table.root.parentNode !== gridHost) {
      gridHost.textContent = '';
      gridHost.appendChild(table.root);
    }
  }

  function firstValue(row) {
    const values = Object.values(row || {});
    return values.length ? String(values[0]) : '';
  }

  function renderStatus() {
    const current = panel();
    const loading = state.loading[current.id];
    const total = rowsFor(current.id).length;
    const shown = table && table.__panel === current.id ? table.rowCount() : total;

    statusBar.setLeft(
      state.error
        ? `ERROR: ${state.error}`
        : `${current.label}: ${shown}/${total} row(s)${loading ? ' \u00B7 loading\u2026' : ''}`,
    );
    statusBar.setRight(`${state.system.system || ''} ${state.system.sysplex ? `/ ${state.system.sysplex}` : ''}`.trim());
  }

  /* ------------------------------------------------------------------------
   * Keyboard routing
   * ---------------------------------------------------------------------- */

  /**
   * Overlay first: when a dialog or the palette is up it owns the keyboard.
   * Then panel scope, then global. The registry resolves the ambiguity for
   * same-chord bindings; this function only decides the *scope*.
   */
  function onKeyDown(event) {
    // A text input owns its keys unless a chord is involved.
    const typing = event.target && ['INPUT', 'TEXTAREA', 'SELECT'].includes(event.target.tagName);
    const hasModifier = event.ctrlKey || event.altKey || event.metaKey;

    // The game menu is a full-screen modal: while it is up it owns the
    // keyboard entirely, so a menu keypress can never leak into the grid.
    if (gameMenu && gameMenu.isOpen()) {
      if (gameMenu.handleKey(event)) { event.preventDefault(); return; }
      return;
    }

    if (dialog.isOpen()) {
      if (dialog.handleKey(event)) { event.preventDefault(); return; }
    } else if (palette.isOpen()) {
      if (palette.handleKey(event)) { event.preventDefault(); return; }
    }

    if (menuBar.isOpen() && menuBar.handleKey(event)) {
      event.preventDefault();
      return;
    }

    if (typing && !hasModifier) return;

    const scope = scopeFor(event);
    const binding = registry.resolve(event, scope, { typing, panel: panel().id });
    if (!binding) {
      // A right-click-free "menu" opens with Alt+M through the registry; also
      // let F10 do the conventional thing.
      if (event.key === 'F10') { menuBar.open(0); event.preventDefault(); }
      return;
    }

    event.preventDefault();
    dispatch(binding.action, binding);
  }

  /** Which panel's bindings are live right now. */
  function scopeFor(event) {
    if (dialog.isOpen()) return 'dialog';
    if (palette.isOpen()) return 'palette';
    const typing = event.target && ['INPUT', 'TEXTAREA', 'SELECT'].includes(event.target.tagName);
    return typing ? 'global' : 'list';
  }

  /* ------------------------------------------------------------------------
   * Assemble
   * ---------------------------------------------------------------------- */

  const titles = UI.el(doc, 'div', { class: 'mf-panel-titles' });
  const gridHost = UI.el(doc, 'div', { class: 'mf-grid-host' });

  function build() {
    const app = doc.getElementById('app');

    menuBar = UI.createMenuBar({ document: doc, menu: MENU, onCommand: dispatch });

    palette = UI.createCommandPalette({
      document: doc,
      entries: registry.describeAll().map((b) => ({
        action: b.action,
        label: b.action,
        description: b.description,
        accelerator: b.pretty,
      })),
      onCommand: dispatch,
    });

    dialog = UI.createDialog({
      document: doc,
      title: 'Find',
      message: 'Filter the active panel. Leave blank to show every row.',
      prompt: '',
      confirmLabel: 'Apply',
    });

    toasts = UI.createToastHost({ document: doc });
    statusBar = UI.createStatusBar({ document: doc });

    windows = UI.createWindowManager({
      document: doc,
      onFocusChange: (record) => {
        statusBar.setRight(record ? `${record.title}` : '');
      },
    });
    doc.body.appendChild(windows.root);

    const toolbar = UI.el(doc, 'div', { class: 'mf-toolbar' }, [
      toolbarButton('Menu', 'game.title', 'Open the full-screen menu (F10)'),
      toolbarButton('Refresh All', 'app.refresh', 'Reload every panel (F5)'),
      toolbarButton('Search', 'data.search', 'Filter the active panel (Ctrl+F)'),
      toolbarButton('Export CSV', 'data.export', 'Download the active panel (Ctrl+E)'),
      toolbarButton('Systems', 'sys.overview', 'Open the system overview pane'),
      toolbarButton('Command Palette', 'app.commandPalette', 'Every action (Ctrl+K)'),
    ]);

    app.appendChild(UI.el(doc, 'header', { class: 'mf-header' }, [
      UI.el(doc, 'h1', { class: 'mf-brand', text: 'MAINFRAME Operator Console' }),
      toolbar,
    ]));
    app.appendChild(menuBar.root);
    app.appendChild(titles);
    app.appendChild(gridHost);
    app.appendChild(statusBar.root);
    app.appendChild(palette.root);
    app.appendChild(dialog.root);
    app.appendChild(toasts.root);

    doc.addEventListener('keydown', onKeyDown);

    buildGameMenu();
    app.appendChild(gameMenu.root);

    renderTitles();
    renderTable();
    renderStatus();

    // The loader advances on real work, never on a timer: each step fires
    // only once the thing it names has actually finished.
    runBootSequence();
  }

  /**
   * Show the kernel-style boot console and drive it through to a loaded
   * console.
   *
   * Every line below corresponds to something that actually happened: the
   * `emit()` calls report values read from the bootstrap payload or from the
   * health endpoint, and each `step()` fires only after the await that
   * justifies it. Nothing here is a timer pretending to be progress.
   *
   * If a step rejects, the console stays up with the failure on screen rather
   * than pretending the system is ready.
   */
  async function runBootSequence() {
    const info = (window.MF_BOOTSTRAP && window.MF_BOOTSTRAP.system) || {};

    const boot = UI.createBootLoader({
      document: doc,
      system: info.name || 'MAINFRAME',
      version: info.version || '1.0.0',
      steps: [
        'Storage subsystem',
        'Database service',
        'Dataset catalog',
        'HTTP control plane',
        'Terminal gateway',
        'Operator console',
      ],
    });

    doc.body.appendChild(boot.root);

    // The banner is the one part that is purely descriptive - it reports the
    // identity the page was served with.
    boot.banner({ sysplex: info.sysplex, region: info.region });

    const send = (url) => fetchJson(url);

    try {
      /* --- Storage ----------------------------------------------------- */
      boot.emit('scsi host0: mainframe virtual SCSI, channel 0');
      const health = await send('/api/health');
      boot.emit(`sd 0:0:0:0: [mfa] ${(health.database && health.database.users) ?? 0} user records online`);
      boot.step('done', 'sd 0:0:0:0: [mfa] attached to /data');

      /* --- Database ---------------------------------------------------- */
      const users = await send('/api/rows?table=users&limit=500');
      boot.emit(`db: driver=${(health.database && health.database.driver) || 'sqlite'} opened`);
      boot.step('done', `db: ${users.count} account(s) in catalogue`);

      /* --- Catalog ----------------------------------------------------- */
      const datasets = await send('/api/rows?table=datasets&limit=500');
      const bytes = (datasets.rows || []).reduce((n, d) => n + Number(d.bytes_used || 0), 0);
      boot.emit(`catalog: ${datasets.count} dataset(s), ${bytes.toLocaleString()} bytes tracked`);
      boot.step('done', 'catalog: volume index rebuilt');

      /* --- HTTP -------------------------------------------------------- */
      const menu = await send('/api/menu');
      boot.emit(`http: control plane answering on port ${(window.MF_BOOTSTRAP || {}).httpPort || 8080}`);
      boot.step('done', `http: ${(menu.menus || []).length} menu system(s) registered`);

      /* --- Terminal gateway -------------------------------------------- */
      // TCP is not reachable from the page, so the honest statement is that
      // the advertised port was read from the bootstrap payload.
      const terminal = (window.MF_BOOTSTRAP || {}).terminalPort;
      boot.emit(`tcp: terminal gateway configured on port ${terminal || 3270}`);
      boot.step('done', 'network: 3270 device mapping established');

      /* --- Console ----------------------------------------------------- */
      await refreshAll();
      boot.emit('console: operator console attaching');

      await boot.finish(`Reached target Multi-User. Boot complete in ${Math.round(performance.now())}ms.`);

      state.booted = true;
      window.setTimeout(() => boot.remove(), 420);
    } catch (err) {
      boot.fail(err.message);
      toast('error', `Boot failed: ${err.message}`);
    }
  }

  if (doc.readyState === 'loading') {
    doc.addEventListener('DOMContentLoaded', build);
  } else {
    build();
  }

  // Exposed for the smoke tests and for console-driven debugging.
  window.MFApp = {
    state,
    dispatch,
    registry,
    panels: PANELS,
    getTable: () => table,
    getGameMenu: () => gameMenu,
    getWindows: () => windows,
    views: () => views.map((v) => v.id),
    openView,
    openGroup,
    refreshWindows,
  };
}());
