'use strict';

/**
 * The console's view registry.
 *
 * Every window the operator can open is declared here as data: an id, a title,
 * an icon glyph, the tables it needs, and a `build` function that turns those
 * tables into DOM nodes. Nothing about *where* a view appears lives in the view
 * itself - the menu, the toolbar, the command palette and the window manager
 * all read this one list, so a view cannot exist in the menu but be missing
 * from the palette, or vice versa.
 *
 * A view is:
 *
 *   {
 *     id,            stable key; also the window id
 *     title,         the window title bar
 *     glyph,         a single character shown in menus
 *     group,         'system' | 'data' | 'operations' - drives menu grouping
 *     description,   one line, shown in the palette
 *     needs,         ['users', 'jobs'] - tables fetched before build() runs
 *     width,         preferred width in px
 *     build(ctx)     returns a node or array of nodes
 *   }
 *
 * `build` receives a context rather than reaching for globals:
 *
 *   { document, tables, health, settings, el, widgets, format }
 *
 * `tables` holds the rows for each name in `needs`, `health` is the /api/health
 * payload, and `widgets` exposes the shared pane content components so every
 * view lays out the same way.
 */

/*
 * This module is loaded both by the browser (as a plain <script>, after
 * components.js) and by Node (for the tests). The browser has no `require`,
 * so the dependency is resolved from whichever global the loader published.
 *
 * It is wrapped in an IIFE because a plain <script> shares one top-level scope:
 * declaring `const el` here would collide with the same name in
 * components.js and stop the whole bundle from parsing.
 */
(function viewsModule(global) {
  const UI = (typeof require === 'function' && typeof module !== 'undefined')
    ? require('./components')
    : (global && global.MFUI);

  if (!UI) {
    throw new Error('views.js requires components.js to be loaded first');
  }

  const { el } = UI;

/* --------------------------------------------------------------------------
 * Formatting helpers shared by every view
 * -------------------------------------------------------------------------- */

/** Group a number for display: 1234567 -> "1,234,567". */
function num(value) {
  const parsed = Number(value);
  if (Number.isNaN(parsed)) return '0';
  return parsed.toLocaleString('en-US');
}

/** Human-readable byte size. */
function bytes(value) {
  const parsed = Number(value) || 0;
  if (parsed < 1024) return `${num(parsed)} B`;
  if (parsed < 1024 * 1024) return `${(parsed / 1024).toFixed(1)} KiB`;
  if (parsed < 1024 * 1024 * 1024) return `${(parsed / (1024 * 1024)).toFixed(1)} MiB`;
  return `${(parsed / (1024 * 1024 * 1024)).toFixed(2)} GiB`;
}

/** Percentage of a total, guarding against a zero denominator. */
function percent(part, total) {
  if (!total) return 0;
  return Math.round((part / total) * 100);
}

/** Count rows whose column matches one of the given values. */
function countWhere(rows, column, ...values) {
  return (rows || []).filter((row) => values.includes(row[column])).length;
}

/** Sum a numeric column. */
function sum(rows, column) {
  return (rows || []).reduce((total, row) => total + (Number(row[column]) || 0), 0);
}

/* --------------------------------------------------------------------------
 * The registry
 * -------------------------------------------------------------------------- */

/**
 * Build the view list.
 *
 * This is a function rather than a constant because a view's `build` closes
 * over nothing - it reads everything from the context - so the same list can
 * be constructed per dashboard instance without shared mutable state.
 *
 * @returns {object[]}
 */
function createViewRegistry() {
  const views = [
    {
      id: 'overview',
      title: 'System Overview',
      glyph: '\u25A3',
      group: 'system',
      description: 'Identity, uptime and queue health at a glance',
      needs: ['jobs', 'datasets', 'transactions'],
      width: 520,
      build: ({ document, tables, health, widgets }) => {
        const jobs = tables.jobs || [];
        const datasets = tables.datasets || [];
        const txns = tables.transactions || [];

        return [
          widgets.section('Identity', widgets.defs([
            ['System', health.system],
            ['Sysplex', health.sysplex],
            ['Version', health.version],
            ['Status', health.status],
            ['Uptime', health.uptimeSeconds === undefined
              ? null : `${num(health.uptimeSeconds)} s`],
            ['Database driver', health.database && health.database.driver],
            ['Accounts', health.database && num(health.database.users)],
          ])),
          widgets.section('Queues', widgets.defs([
            ['Jobs known', num(jobs.length)],
            ['Running', num(countWhere(jobs, 'status', 'RUNNING'))],
            ['Queued', num(countWhere(jobs, 'status', 'QUEUED'))],
            ['Abended', num(countWhere(jobs, 'status', 'ABEND'))],
          ])),
          widgets.section('Storage', widgets.defs([
            ['Datasets', num(datasets.length)],
            ['Bytes tracked', bytes(sum(datasets, 'bytes_used'))],
            ['Distinct volumes', new Set(datasets.map((d) => d.volume)).size],
          ])),
          widgets.section('Transactions', widgets.defs([
            ['Logged', num(txns.length)],
            ['Failed', num(countWhere(txns, 'status', 'FAIL', 'ABEND'))],
            ['Rows written', num(sum(txns, 'rows_written'))],
          ])),
        ];
      },
    },

    {
      id: 'subsystems',
      title: 'Subsystems',
      glyph: '\u2699',
      group: 'system',
      description: 'Control plane, gateway and database service state',
      needs: ['transactions', 'audit_log'],
      width: 460,
      build: ({ health, tables, widgets }) => {
        const txns = tables.transactions || [];
        const audit = tables.audit_log || [];
        const up = health.status === 'ok';

        return [
          widgets.section('Services', widgets.defs([
            ['HTTP control plane', up ? 'ACTIVE' : 'UNKNOWN'],
            ['Terminal gateway', up ? 'ACTIVE' : 'UNKNOWN'],
            ['Database', health.database ? String(health.database.driver).toUpperCase() : null],
            ['Uptime', health.uptimeSeconds === undefined ? null : `${num(health.uptimeSeconds)} s`],
          ])),
          widgets.section('Workload', widgets.defs([
            ['Transactions', num(txns.length)],
            ['Audit entries', num(audit.length)],
          ])),
          widgets.section('Transaction outcomes', widgets.distribution(txns, 'status')),
        ];
      },
    },

    {
      id: 'storage',
      title: 'Storage Volumes',
      glyph: '\u25A4',
      group: 'system',
      description: 'Dataset catalogue, volumes and space in use',
      needs: ['datasets'],
      width: 480,
      build: ({ tables, widgets }) => {
        const datasets = tables.datasets || [];
        const used = sum(datasets, 'bytes_used');
        const records = sum(datasets, 'record_count');

        return [
          widgets.section('Summary', widgets.defs([
            ['Datasets catalogued', num(datasets.length)],
            ['Records stored', num(records)],
            ['Bytes used', bytes(used)],
            ['Average dataset', datasets.length ? bytes(used / datasets.length) : null],
            ['Distinct volumes', new Set(datasets.map((d) => d.volume)).size],
          ])),
          widgets.section('Datasets per volume', widgets.distribution(datasets, 'volume')),
          widgets.section('By organisation', widgets.distribution(datasets, 'dsorg')),
          widgets.section('By record format', widgets.distribution(datasets, 'recfm')),
        ];
      },
    },

    {
      id: 'security',
      title: 'Security',
      glyph: '\u26BF',
      group: 'system',
      description: 'Accounts, roles and audit severity',
      needs: ['users', 'audit_log'],
      width: 470,
      build: ({ tables, widgets }) => {
        const users = tables.users || [];
        const audit = tables.audit_log || [];
        const errors = countWhere(audit, 'severity', 'ERROR', 'CRITICAL');

        return [
          widgets.section('Posture', widgets.defs([
            ['Accounts', num(users.length)],
            ['Active', num(countWhere(users, 'status', 'ACTIVE'))],
            ['Locked', num(countWhere(users, 'status', 'LOCKED'))],
            ['Disabled', num(countWhere(users, 'status', 'DISABLED'))],
            ['Audit entries', num(audit.length)],
            ['Error-level events', num(errors)],
          ])),
          widgets.section('Accounts by role', widgets.distribution(users, 'role')),
          widgets.section('Accounts by status', widgets.distribution(users, 'status')),
          widgets.section('Audit by severity', widgets.distribution(audit, 'severity')),
          widgets.section('Audit by event type', widgets.distribution(audit, 'event_type')),
        ];
      },
    },

    {
      id: 'jobs',
      title: 'Job Queue',
      glyph: '\u25B6',
      group: 'operations',
      description: 'Batch jobs by class, status and return code',
      needs: ['jobs', 'users'],
      width: 460,
      build: ({ document, tables, widgets }) => {
        const jobs = tables.jobs || [];
        const abended = jobs.filter((j) => j.status === 'ABEND');

        const sections = [
          widgets.section('Queue', widgets.defs([
            ['Jobs known', num(jobs.length)],
            ['Running', num(countWhere(jobs, 'status', 'RUNNING'))],
            ['Queued', num(countWhere(jobs, 'status', 'QUEUED'))],
            ['On hold', num(countWhere(jobs, 'status', 'HOLD'))],
            ['Complete', num(countWhere(jobs, 'status', 'COMPLETE'))],
          ])),
          widgets.section('By class', widgets.distribution(jobs, 'job_class')),
          widgets.section('By status', widgets.distribution(jobs, 'status')),
        ];

        // Only surface the abend list when there is something to look at.
        if (abended.length) {
          const list = el(document, 'ul', { class: 'mf-list' });
          for (const job of abended.slice(0, 10)) {
            list.appendChild(el(document, 'li', {
              text: `${job.job_name} (rc ${job.return_code})`,
            }));
          }
          sections.push(widgets.section(`Abended (${abended.length})`, list));
        }

        return sections;
      },
    },

    {
      id: 'terminals',
      title: 'Terminal Sessions',
      glyph: '\u2338',
      group: 'operations',
      description: '3270 gateway sessions and transaction activity',
      needs: ['transactions', 'users'],
      width: 470,
      build: ({ document, tables, widgets }) => {
        const txns = tables.transactions || [];
        const users = tables.users || [];

        // Distinct terminals, with the traffic each one carried.
        const byTerminal = new Map();
        for (const txn of txns) {
          const key = txn.terminal || '(none)';
          const current = byTerminal.get(key) || { count: 0, rows: 0, failed: 0 };
          current.count += 1;
          current.rows += Number(txn.rows_read || 0) + Number(txn.rows_written || 0);
          if (txn.status === 'FAIL' || txn.status === 'ABEND') current.failed += 1;
          byTerminal.set(key, current);
        }

        const table = el(document, 'table', { class: 'mf-table' });
        const head = el(document, 'tr', {}, [
          el(document, 'th', { text: 'Terminal' }),
          el(document, 'th', { text: 'Txns' }),
          el(document, 'th', { text: 'Rows' }),
          el(document, 'th', { text: 'Failed' }),
        ]);
        table.appendChild(el(document, 'thead', {}, [head]));

        const body = el(document, 'tbody');
        for (const [terminal, stats] of byTerminal) {
          body.appendChild(el(document, 'tr', {}, [
            el(document, 'td', { text: terminal }),
            el(document, 'td', { text: num(stats.count) }),
            el(document, 'td', { text: num(stats.rows) }),
            el(document, 'td', { text: num(stats.failed) }),
          ]));
        }
        table.appendChild(body);

        return [
          widgets.section('Summary', widgets.defs([
            ['Transactions', num(txns.length)],
            ['Distinct terminals', num(byTerminal.size)],
            ['Signed-on accounts', num(users.length)],
            ['Error rate', `${percent(
              countWhere(txns, 'status', 'FAIL', 'ABEND'), txns.length,
            )}%`],
          ])),
          widgets.section('Terminal activity', table),
          widgets.section('By transaction code', widgets.distribution(txns, 'txn_code')),
        ];
      },
    },

    {
      id: 'audit',
      title: 'Audit Trail',
      glyph: '\u2261',
      group: 'operations',
      description: 'Severity breakdown and the most recent events',
      needs: ['audit_log'],
      width: 520,
      build: ({ document, tables, widgets }) => {
        const audit = tables.audit_log || [];
        const recent = audit.slice(-12).reverse();

        const list = el(document, 'div', { class: 'mf-event-list' });
        if (!recent.length) {
          list.appendChild(el(document, 'p', { class: 'mf-empty', text: '(no entries)' }));
        }
        for (const entry of recent) {
          list.appendChild(el(document, 'div', {
            class: `mf-event is-${String(entry.severity || 'info').toLowerCase()}`,
          }, [
            el(document, 'span', { class: 'mf-event-time', text: entry.created_at || '' }),
            el(document, 'span', { class: 'mf-event-actor', text: entry.actor || '' }),
            el(document, 'span', { class: 'mf-event-message', text: entry.message || '' }),
          ]));
        }

        return [
          widgets.section('Volume', widgets.defs([
            ['Entries', num(audit.length)],
            ['Critical', num(countWhere(audit, 'severity', 'CRITICAL'))],
            ['Errors', num(countWhere(audit, 'severity', 'ERROR'))],
            ['Warnings', num(countWhere(audit, 'severity', 'WARN'))],
          ])),
          widgets.section('By severity', widgets.distribution(audit, 'severity')),
          widgets.section('Recent events', list),
        ];
      },
    },

    {
      id: 'accounts',
      title: 'Accounts',
      glyph: '\u263A',
      group: 'data',
      description: 'Operator accounts, roles and lock state',
      needs: ['users'],
      width: 500,
      build: ({ document, tables, widgets }) => {
        const users = tables.users || [];

        const table = el(document, 'table', { class: 'mf-table' });
        table.appendChild(el(document, 'thead', {}, [
          el(document, 'tr', {}, [
            el(document, 'th', { text: 'User' }),
            el(document, 'th', { text: 'Name' }),
            el(document, 'th', { text: 'Role' }),
            el(document, 'th', { text: 'Status' }),
            el(document, 'th', { text: 'Failed' }),
          ]),
        ]));

        const body = el(document, 'tbody');
        for (const user of users) {
          body.appendChild(el(document, 'tr', {}, [
            el(document, 'td', { text: user.username || '' }),
            el(document, 'td', { text: user.full_name || '' }),
            el(document, 'td', { text: user.role || '' }),
            el(document, 'td', { text: user.status || '' }),
            el(document, 'td', { text: num(user.failed_logons) }),
          ]));
        }
        table.appendChild(body);

        return [
          widgets.section('Summary', widgets.defs([
            ['Accounts', num(users.length)],
            ['Departments', new Set(users.map((u) => u.department)).size],
            ['Locked out', num(countWhere(users, 'status', 'LOCKED'))],
          ])),
          widgets.section('Directory', table),
          widgets.section('By role', widgets.distribution(users, 'role')),
        ];
      },
    },

    {
      id: 'config',
      title: 'Configuration',
      glyph: '\u2692',
      group: 'operations',
      description: 'The resolved runtime configuration for this session',
      needs: [],
      width: 520,
      build: ({ document, widgets, settings, bootstrap }) => {
        const view = el(document, 'div');

        view.appendChild(widgets.section('Session', widgets.defs([
          ['System', bootstrap && bootstrap.system && bootstrap.system.name],
          ['Region', bootstrap && bootstrap.system && bootstrap.system.region],
          ['HTTP port', bootstrap && bootstrap.httpPort],
          ['Terminal port', bootstrap && bootstrap.terminalPort],
          ['Bootstrap generated', bootstrap && bootstrap.generatedAt],
        ])).root);

        view.appendChild(widgets.section('Display', widgets.defs([
          ['Scanlines', settings.scanlines ? 'on' : 'off'],
          ['Timestamps', settings.timestamps ? 'on' : 'off'],
          ['Compact rows', settings.compact ? 'on' : 'off'],
          ['Volume', `${settings.volume}%`],
        ])).root);

        view.appendChild(widgets.section('Resolution order', (() => {
          const list = el(document, 'ol', { class: 'mf-list' });
          for (const source of [
            'config/default.json',
            'config/local.json (or --config <path>)',
            'MF_* environment variables',
            'CLI flags (--port, --db, --log-level)',
          ]) {
            list.appendChild(el(document, 'li', { text: source }));
          }
          return list;
        })()).root);

        return view;
      },
    },
  ];

  return views;
}

/**
 * Index a view list by id.
 * @param {object[]} views
 * @returns {Map<string, object>}
 */
function indexViews(views) {
  const index = new Map();
  for (const view of views || []) {
    if (!view || !view.id) continue;
    if (index.has(view.id)) {
      throw new Error(`duplicate view id "${view.id}"`);
    }
    index.set(view.id, {
      needs: [],
      width: 460,
      group: 'system',
      description: '',
      glyph: '',
      ...view,
    });
  }
  return index;
}

/** Views in one group, in declaration order. */
function viewsInGroup(views, group) {
  return (views || []).filter((view) => view.group === group);
}

/**
 * Every table name any view needs. Used to load the console's data once at
 * startup instead of per window, so opening a window is instant.
 */
function requiredTables(views) {
  const names = new Set();
  for (const view of views || []) {
    for (const name of view.needs || []) names.add(name);
  }
  return [...names];
}

/**
 * Build the context object a view's `build` receives.
 *
 * Assembled here rather than in the dashboard so a view cannot reach for a
 * global - everything it may use is handed to it, and the tests can supply the
 * same shape without a browser.
 *
 * @param {object} deps
 * @returns {object}
 */
function createViewContext(deps) {
  const document = deps.document;

  return {
    document,
    tables: deps.tables || {},
    health: deps.health || {},
    settings: deps.settings || {},
    bootstrap: deps.bootstrap || null,
    el,
    format: { num, bytes, percent, countWhere, sum },

    // The shared pane widgets, pre-bound to this document.
    widgets: {
      defs: (entries) => UI.createDefinitionList({ document, entries }),
      distribution: (rows, column) => UI.createDistributionList({ document, rows, column }),
      section: (heading, content) => UI.createSection({ document, heading, content }),
    },
  };
}

/**
 * Coerce whatever a view returned into DOM nodes.
 *
 * Views build with the shared widgets, which return `{ root, ... }` handles
 * rather than bare nodes. Accepting both here means a view can pass a widget
 * or a widget's `.root` without the two being silently different - a mistake
 * that renders an empty window rather than raising an error.
 */
function toNodes(value, out = []) {
  if (value === null || value === undefined || value === false) return out;
  if (Array.isArray(value)) {
    for (const item of value) toNodes(item, out);
    return out;
  }
  if (value.root) {
    toNodes(value.root, out);
    return out;
  }
  if (typeof value === 'object' && typeof value.appendChild === 'function') {
    out.push(value);
  }
  return out;
}

/**
 * Run a view's build function against a context and return its nodes.
 * A view that throws must not take the whole console down, so the error is
 * rendered in place of the content and reported to the caller.
 *
 * @returns {{nodes: Array, error: Error|null}}
 */
function buildView(view, context) {
  if (!view || typeof view.build !== 'function') {
    return { nodes: [], error: new Error('view has no build function') };
  }
  try {
    return { nodes: toNodes(view.build(context)), error: null };
  } catch (error) {
    const node = el(context.document, 'div', { class: 'mf-view-error' }, [
      el(context.document, 'strong', { text: 'This view failed to render.' }),
      el(context.document, 'p', { text: error.message }),
    ]);
    return { nodes: [node], error };
  }
}

  const MFViews = {
    createViewRegistry,
    indexViews,
    viewsInGroup,
    requiredTables,
    createViewContext,
    buildView,
    toNodes,
    num,
    bytes,
    percent,
    countWhere,
    sum,
  };

  if (typeof module !== 'undefined' && module.exports) module.exports = MFViews;
  if (global) global.MFViews = MFViews;
}(typeof window !== 'undefined' ? window : null));
